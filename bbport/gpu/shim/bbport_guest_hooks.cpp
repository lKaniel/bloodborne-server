// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_guest_hooks.h"
#include "bbport_heap_sites.h"
#include "bbport_toggles.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/uio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size);
extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);

namespace BbGuestHooks {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
constexpr u64 ImageBase = 0x800000000ull;

// The game's code calls into the hooks (the stubs, a call site's memcpy) with the System V ABI;
// on Windows the host's default is the Microsoft one.
#ifdef _WIN32
#define GUEST_ABI __attribute__((sysv_abi))
#else
#define GUEST_ABI
#endif

/// Makes [address, address + size) of the game's code writable; Restore puts the protection back
/// (the one it had, as /proc/self/maps lists it on Linux) and flushes the instruction cache on
/// Windows. The hooks go in while the loader still writes the image (bbgpu_init, before the
/// community patches): a page left read-execute there made the FPS++ write at 0xbbc40f fault.
struct CodeWrite {
    void* page = nullptr;
    std::size_t span = 0;
#ifdef _WIN32
    DWORD previous = 0;
#else
    int previous = PROT_READ | PROT_EXEC;
#endif
    bool Open(u64 address, std::size_t size) {
#ifdef _WIN32
        page = reinterpret_cast<void*>(address);
        span = size;
        return VirtualProtect(page, span, PAGE_EXECUTE_READWRITE, &previous) != 0;
#else
        const long page_size = sysconf(_SC_PAGESIZE);
        page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
        span = (address + size) - reinterpret_cast<u64>(page);
        if (FILE* maps = std::fopen("/proc/self/maps", "r")) {
            char line[512];
            while (std::fgets(line, sizeof(line), maps)) {
                unsigned long long lo = 0, hi = 0;
                char perms[8] = {};
                if (std::sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) == 3 && address >= lo &&
                    address < hi) {
                    previous = (perms[0] == 'r' ? PROT_READ : 0) |
                               (perms[1] == 'w' ? PROT_WRITE : 0) |
                               (perms[2] == 'x' ? PROT_EXEC : 0);
                    break;
                }
            }
            std::fclose(maps);
        }
        return mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
    }
    void Restore() {
#ifdef _WIN32
        DWORD unused = 0;
        VirtualProtect(page, span, previous, &unused);
        FlushInstructionCache(GetCurrentProcess(), page, span);
#else
        mprotect(page, span, previous);
#endif
    }
};

/// The return of the game's GPU memory range allocator 0x26aa070 (rdi allocator, rsi size,
/// rdx alignment; it returns a node: [node + 8] the start, [[node + 0x18] + 8] the end, the next
/// node's start): `add rsp, 0x18` in its epilogue, reached by its own branches only.
constexpr u64 AllocReturn = 0x26aa255;
constexpr unsigned char AllocReturnCode[4] = {0x48, 0x83, 0xc4, 0x18};
RangeCallback on_range_allocated = nullptr;

/// The game's other GPU memory heap (TLSF-like, headers in the memory: free at 0x20858a0, alloc at
/// 0x2085a60), reached through a pointer at 0x54b9f38 set at startup. Its only three callers
/// (wrappers that record {address, size} in a map) pass r8 = rbp - 0x50 for the size handed out and
/// follow the call with `mov rbx, rax` (rax: the address).
constexpr std::array<u64, 3> HeapAllocReturns = {0x11e6629, 0x11e6736, 0x11e6846};
constexpr unsigned char HeapAllocReturnCode[3] = {0x48, 0x89, 0xc3};

/// BB_FRAME_STATS: the range allocator's collector (0x26aa860, `push rbp`; rdi the allocator): its
/// live range count [+0xc8] and byte count [+0xd0] are noted for the frame stats.
constexpr u64 CollectorEntry = 0x26aa860;
constexpr unsigned char CollectorEntryCode[1] = {0x55};

/// BB_HEAP_SITES=1: the game's deferred object release (0xbb5f10) skips the destruction when the
/// registry check (0xbf0790) fails: `js 0xbb5fc7` there, its result counted (eax).
constexpr u64 ReleaseCheck = 0xbb5fb0;
constexpr unsigned char ReleaseCheckCode[2] = {0x78, 0x15};

/// `call memcpy` (0x261a1b0: rdi destination, rsi source, rdx size) in the resource loaders:
/// 0x216d420 and 0x2171d60 copy texture and buffer data into GPU memory (most of the write
/// faults while areas stream in).
struct CallSite {
    u64 offset;
    unsigned char code[5];
};
constexpr std::array<CallSite, 2> MemcpySites = {{
    {0x216d5ea, {0xe8, 0xc1, 0xcb, 0x4a, 0x00}},
    {0x2171fea, {0xe8, 0xc1, 0x81, 0x4a, 0x00}},
}};
std::array<std::atomic<u64>, MemcpySites.size()> hits{};

/// The game's memcpy at those sites: the GPU side is told before (its write traps lift: no fault
/// per page) and after the copy (the data now there), as for file reads.
GUEST_ABI void* LoaderCopy(void* dst, const void* src, std::size_t size) {
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    std::memcpy(dst, src, size);
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    return dst;
}

/// bbport: native detours (default; BB_GUEST_HOOK_TRAPS=1: int3 traps). Each hooked site jumps to
/// a stub next to the image that calls the handler and runs the instructions the jump covers;
/// the copy call sites call LoaderCopy through a trampoline there. An int3 trap costs a signal
/// (a few microseconds in the kernel) per call; a detour costs a call. The int3 written first
/// stays the fallback while the jump's bytes go in: OnTrap sends a thread that meets it to the stub.
struct NativeSite {
    u64 offset;
    std::atomic<u64> stub{0};
};
std::array<NativeSite, 4> native_sites{{{AllocReturn}, {HeapAllocReturns[0]}, {HeapAllocReturns[1]},
                                        {HeapAllocReturns[2]}}};
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 4096;

/// A page within +-2 GiB of the image (below its base: the image grows upwards).
u8* MapStubPage() {
    for (u64 k = 1; k <= 64; ++k) {
        const u64 hint = ImageBase - k * (16ull << 20);
#ifdef _WIN32
        // Plain memory: the guest range's placeholder reservation (runtime_memory.c) starts above
        // the image.
        void* p = VirtualAlloc(reinterpret_cast<void*>(hint), StubPageSize, MEM_RESERVE | MEM_COMMIT,
                               PAGE_EXECUTE_READWRITE);
        if (p) {
            return static_cast<u8*>(p);
        }
#else
        void* p = mmap(reinterpret_cast<void*>(hint), StubPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) {
            return static_cast<u8*>(p);
        }
#endif
    }
    return nullptr;
}

/// The stub page, written: read-execute from now on.
void SealStubPage() {
#ifdef _WIN32
    DWORD old_protect = 0;
    VirtualProtect(stub_page, StubPageSize, PAGE_EXECUTE_READ, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), stub_page, StubPageSize);
#else
    mprotect(stub_page, StubPageSize, PROT_READ | PROT_EXEC);
#endif
}

struct Emitter {
    std::array<u8, 256> code{};
    std::size_t size = 0;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            code[size++] = b;
        }
    }
    void U32(std::uint32_t v) {
        std::memcpy(&code[size], &v, 4);
        size += 4;
    }
    void U64(u64 v) {
        std::memcpy(&code[size], &v, 8);
        size += 8;
    }
    /// Saves the caller-saved registers (the handler may change them), aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
};

/// Copies `e` into the stub page; returns its address there, or 0.
u64 PlaceStub(const Emitter& e) {
    if (!stub_page || stub_used + e.size > StubPageSize) {
        return 0;
    }
    u8* at = stub_page + stub_used;
    std::memcpy(at, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(at);
}

bool Rel32To(u64 from_next, u64 to, std::uint32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::uint32_t(std::int32_t(d));
    return true;
}

/// Replaces bytes [1, 5) at `address` (byte 0 is the trap already there), then byte 0.
void WriteJumpOverTrap(u64 address, u8 opcode, std::uint32_t rel) {
    CodeWrite code;
    code.Open(address, 5);
    auto* p = reinterpret_cast<u8*>(address);
    for (int i = 0; i < 4; ++i) {
        __atomic_store_n(p + 1 + i, u8(rel >> (8 * i)), __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(p, opcode, __ATOMIC_SEQ_CST);
    code.Restore();
}

GUEST_ABI void AllocHook(u64 node) {
    if (!node) {
        return;
    }
    const u64 start = *reinterpret_cast<const u64*>(node + 8);
    const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
    const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
    if (end > start && end - start < (u64(1) << 32)) {
        on_range_allocated(start, end - start);
    }
}

GUEST_ABI void HeapHook(u64 address, u64 size) {
    if (address != 0 && size != 0 && size < (u64(1) << 32)) {
        on_range_allocated(address, size);
    }
}

/// The detour of a site whose int3 is in place (Patch). False: left as a trap.
bool InstallDetour(NativeSite& site) {
    const u64 address = ImageBase + site.offset;
    Emitter e;
    u64 back = 0;
    if (site.offset == AllocReturn) {
        // `add rsp, 0x18; pop rbx` (5 bytes) in the allocator's epilogue; rax: the node.
        if (reinterpret_cast<const u8*>(address)[4] != 0x5b) {
            return false;
        }
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xc7}); // mov rdi, rax (unchanged by the saving)
        e.CallAbs(reinterpret_cast<const void*>(&AllocHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x83, 0xc4, 0x18}); // add rsp, 0x18
        e.Bytes({0x5b});                   // pop rbx
        back = address + 5;
    } else {
        // `mov rbx, rax; test rbx, rbx` (6 bytes) after the heap call; [rbp - 0x50]: the size.
        const auto* code = reinterpret_cast<const u8*>(address);
        if (code[3] != 0x48 || code[4] != 0x85 || code[5] != 0xdb) {
            return false;
        }
        e.Bytes({0x48, 0x89, 0xc3});                               // mov rbx, rax
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128]
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xdf});       // mov rdi, rbx
        e.Bytes({0x48, 0x8b, 0x75, 0xb0}); // mov rsi, [rbp - 0x50]
        e.CallAbs(reinterpret_cast<const void*>(&HeapHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
        e.Bytes({0x48, 0x85, 0xdb});                               // test rbx, rbx
        back = address + 6;
    }
    // jmp back
    const u64 jmp_at = reinterpret_cast<u64>(stub_page + stub_used) + e.size;
    std::uint32_t rel_back = 0;
    if (!Rel32To(jmp_at + 5, back, rel_back)) {
        return false;
    }
    e.Bytes({0xe9});
    e.U32(rel_back);
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    site.stub.store(stub, std::memory_order_release); // a thread at the trap now goes there
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The GX swap-chain's per-frame render command: its 16-byte node comes from a per-frame bump
/// allocator that returns null once the frame's heap is used up; the game writes through it
/// unchecked (`mov [r12 + 8], r13` at 0x2199310, then posts it to the render thread): a guest
/// fault at address 8. Without a node the post is skipped and the pending flag (this + 0x3ab)
/// stays set, so the command goes out with the next frame. From deadinside28's bbport 0.6
/// (InstallGxNodeGuard; their issues #162, #192).
constexpr u64 GxNodeSite = 0x2199310;
constexpr u64 GxNodeSkip = 0x219934e; // past the post and the flag's clear
constexpr unsigned char GxNodeCode[9] = {0x4d, 0x89, 0x6c, 0x24, 0x08,  // mov [r12 + 8], r13
                                         0x49, 0x8b, 0x87, 0xb0};        // mov rax, [r15 + 0xb0]
std::atomic<u64> gx_node_skip_trap{0}; ///< the stub's int3 for a null node (OnTrapAt)

bool InstallGxNodeGuard() {
    const u64 address = ImageBase + GxNodeSite;
    if (std::memcmp(reinterpret_cast<const void*>(address), GxNodeCode, sizeof(GxNodeCode)) != 0) {
        return false;
    }
    Emitter e;
    e.Bytes({0x4d, 0x85, 0xe4});                   // test r12, r12
    e.Bytes({0x74, 0x13});                         // jz null (past the next 19 bytes)
    e.Bytes({0x4d, 0x89, 0x6c, 0x24, 0x08});       // mov [r12 + 8], r13
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + 5);
    const std::size_t trap_at = e.size;
    e.Bytes({0xcc}); // null: OnTrapAt counts it and skips the post
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    gx_node_skip_trap.store(stub + trap_at, std::memory_order_release);
    CodeWrite code;
    if (!code.Open(address, 5)) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<u8*>(address), u8{0xcc}, __ATOMIC_SEQ_CST); // OnTrapAt runs
    code.Restore();                                                             // it meanwhile
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The copy call sites: the call's target becomes a trampoline to LoaderCopy.
bool InstallCallDetour(u64 offset) {
    Emitter e;
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(reinterpret_cast<u64>(&LoaderCopy));
    const u64 stub = PlaceStub(e);
    const u64 address = ImageBase + offset;
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe8, rel);
    return true;
}
/// The registers a trap reads and changes: the thread's saved context (Linux ucontext gregs,
/// Windows CONTEXT).
struct TrapRegs {
    u64* rip;
    u64* rax;
    u64* rbx;
    u64* rsp;
    u64* rbp;
    u64* rdi;
    u64* r12;
    u64* r13;
};

/// A thread at the int3 written at `rip`: runs what the site does and moves it on. False when the
/// trap is not one of these hooks.
bool OnTrapAt(u64 rip, const TrapRegs& g) {
    // The GX node guard: its site while the detour goes in, and its null case.
    const bool gx_site = rip == ImageBase + GxNodeSite;
    if (gx_site || (rip != 0 && rip == gx_node_skip_trap.load(std::memory_order_acquire))) {
        if (gx_site && *g.r12 != 0) {
            *reinterpret_cast<u64*>(*g.r12 + 8) = *g.r13;
            *g.rip = ImageBase + GxNodeSite + 5;
            return true;
        }
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) {
            std::printf("Guest hooks: the GX per-frame command heap was full: a render command "
                        "waits for the next frame\n");
        }
        *g.rip = ImageBase + GxNodeSkip;
        return true;
    }
    for (const auto& site : native_sites) {
        if (rip == ImageBase + site.offset) {
            if (const u64 stub = site.stub.load(std::memory_order_acquire)) {
                *g.rip = stub; // the detour is going in: its stub runs the site
                return true;
            }
        }
    }
    if (rip == ImageBase + ReleaseCheck) {
        const auto result = static_cast<std::int32_t>(*g.rax & 0xffffffff);
        BbHeapSites::NoteReleaseCheck(std::uint32_t(result));
        *g.rip = ImageBase + (result < 0 ? 0xbb5fc7 : ReleaseCheck + 2);
        return true;
    }
    if (rip == ImageBase + CollectorEntry) {
        const u64 allocator = *g.rdi;
        for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
            u64 expected = 0;
            if (BbStats::range_allocators[i].load(std::memory_order_relaxed) == allocator ||
                BbStats::range_allocators[i].compare_exchange_strong(expected, allocator)) {
                BbStats::range_live[i].store(*reinterpret_cast<const u64*>(allocator + 0xc8),
                                             std::memory_order_relaxed);
                BbStats::range_bytes[i].store(*reinterpret_cast<const u64*>(allocator + 0xd0),
                                              std::memory_order_relaxed);
                break;
            }
        }
        *g.rsp -= 8; // push rbp
        *reinterpret_cast<u64*>(*g.rsp) = *g.rbp;
        *g.rip = ImageBase + CollectorEntry + 1;
        return true;
    }
    for (const u64 site : HeapAllocReturns) {
        if (rip != ImageBase + site) {
            continue;
        }
        const u64 address = *g.rax;
        const u64 size = *reinterpret_cast<const u64*>(*g.rbp - 0x50);
        if (address != 0 && size != 0 && size < (u64(1) << 32)) {
            on_range_allocated(address, size);
        }
        *g.rbx = *g.rax;
        *g.rip = ImageBase + site + sizeof(HeapAllocReturnCode);
        return true;
    }
    if (rip == ImageBase + AllocReturn) {
        if (const u64 node = *g.rax) {
            const u64 start = *reinterpret_cast<const u64*>(node + 8);
            const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
            const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
            if (end > start && end - start < (u64(1) << 32)) {
                on_range_allocated(start, end - start);
            }
        }
        *g.rsp += 0x18;
        *g.rip = ImageBase + AllocReturn + sizeof(AllocReturnCode);
        return true;
    }
    for (std::size_t i = 0; i < MemcpySites.size(); ++i) {
        const auto& site = MemcpySites[i];
        if (rip != ImageBase + site.offset) {
            continue;
        }
        hits[i].fetch_add(1, std::memory_order_relaxed);
        // The call, to LoaderCopy instead of the game's memcpy: push the return address, jump.
        const u64 ret = ImageBase + site.offset + 5;
        *g.rsp -= 8;
        *reinterpret_cast<u64*>(*g.rsp) = ret;
        *g.rip = reinterpret_cast<u64>(&LoaderCopy);
        return true;
    }
    return false;
}

#ifdef _WIN32
/// Windows: the traps arrive as breakpoint exceptions (first in the vectored handler chain; the
/// fault handler of probe.c does not take breakpoints). ExceptionAddress is the int3 itself.
LONG CALLBACK OnTrapException(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* c = info->ContextRecord;
    const TrapRegs regs{reinterpret_cast<u64*>(&c->Rip), reinterpret_cast<u64*>(&c->Rax),
                        reinterpret_cast<u64*>(&c->Rbx), reinterpret_cast<u64*>(&c->Rsp),
                        reinterpret_cast<u64*>(&c->Rbp), reinterpret_cast<u64*>(&c->Rdi),
                        reinterpret_cast<u64*>(&c->R12), reinterpret_cast<u64*>(&c->R13)};
    const u64 rip = reinterpret_cast<u64>(info->ExceptionRecord->ExceptionAddress);
    return OnTrapAt(rip, regs) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
#else
struct sigaction previous_action {};

void OnTrap(int sig, siginfo_t* info, void* context) {
    auto* g = static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    const u64 rip = u64(g[REG_RIP]) - 1; // past the int3
    static_assert(sizeof(greg_t) == sizeof(u64));
    const TrapRegs regs{reinterpret_cast<u64*>(&g[REG_RIP]), reinterpret_cast<u64*>(&g[REG_RAX]),
                        reinterpret_cast<u64*>(&g[REG_RBX]), reinterpret_cast<u64*>(&g[REG_RSP]),
                        reinterpret_cast<u64*>(&g[REG_RBP]), reinterpret_cast<u64*>(&g[REG_RDI]),
                        reinterpret_cast<u64*>(&g[REG_R12]), reinterpret_cast<u64*>(&g[REG_R13])};
    if (OnTrapAt(rip, regs)) {
        return;
    }
    if (previous_action.sa_flags & SA_SIGINFO) {
        if (previous_action.sa_sigaction) {
            previous_action.sa_sigaction(sig, info, context);
            return;
        }
    } else if (previous_action.sa_handler != SIG_DFL && previous_action.sa_handler != SIG_IGN &&
               previous_action.sa_handler) {
        previous_action.sa_handler(sig);
        return;
    }
    signal(SIGTRAP, SIG_DFL);
    raise(SIGTRAP);
}
#endif

bool Patch(u64 address, const unsigned char* expected, std::size_t size) {
    unsigned char bytes[16]{};
    iovec local{bytes, size}, remote{reinterpret_cast<void*>(address), size};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != ssize_t(size) ||
        std::memcmp(bytes, expected, size) != 0) {
        return false;
    }
    CodeWrite code;
    if (!code.Open(address, size)) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(address), static_cast<unsigned char>(0xcc),
                     __ATOMIC_SEQ_CST);
    code.Restore();
    return true;
}

/// Render jobs (deadinside28 0.6, the Laurence cinematic above 30 FPS): a runner whose job failed
/// (result 1) keeps the slot it took from the job pool (+0x120), and with every slot held no job
/// runs again: the cinematic stopped. The slot goes back to the pool through the game's own
/// 0x1056990, at the stack check every exit of the job function (0x1068092) ends in, and when a
/// runner is destroyed or reset with a slot still held (0x106798a, 0x1067a5c).
std::atomic<u64> job_slots_released{0};

void NoteJobSlotReleased(const char* how) {
    const u64 n = job_slots_released.fetch_add(1) + 1;
    if (n <= 5 || (n & (n - 1)) == 0) {
        std::printf("Guest hooks: %s gave its render job slot back (%llu so far)\n", how,
                    static_cast<unsigned long long>(n));
    }
}

GUEST_ABI void JobSlotFix(u64 runner, u64 queue, std::uint32_t result, std::uint32_t) {
    if (result != 1 || !runtime_memory_is_mapped(runner + 0x120, 8)) {
        return;
    }
    u64& held = *reinterpret_cast<u64*>(runner + 0x120);
    const u64 item = held;
    if (!item || !queue) {
        return;
    }
    using GiveBack = void(GUEST_ABI*)(u64 queue, u64 item);
    reinterpret_cast<GiveBack>(ImageBase + 0x1056990)(queue, item);
    held = 0;
    NoteJobSlotReleased("a failed render job");
}

GUEST_ABI void JobSlotDrop(u64 runner) {
    u64& held = *reinterpret_cast<u64*>(runner + 0x120);
    const u64 item = held;
    if (!item) {
        return;
    }
    const u64 queue = *reinterpret_cast<const u64*>(ImageBase + 0x54b8788);
    if (!queue) {
        return;
    }
    using GiveBack = void(GUEST_ABI*)(u64 queue, u64 item);
    reinterpret_cast<GiveBack>(ImageBase + 0x1056990)(queue, item);
    held = 0;
    NoteJobSlotReleased("a render job destroyed with a slot");
}

bool InstallJobSlotDropHook(u64 offset) {
    // mov qword [rbx + 0x120], 0
    static constexpr std::array<u8, 11> code = {0x48, 0xc7, 0x83, 0x20, 0x01, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x00};
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), code.data(), code.size())) {
        return false;
    }
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x48, 0x89, 0xdf}); // mov rdi, rbx (the runner)
    e.CallAbs(reinterpret_cast<const void*>(&JobSlotDrop));
    e.RestoreVolatile();
    for (const u8 b : code) {
        e.Bytes({b});
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + code.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// BB_JOB_SLOT_FIX=0: not installed.
bool InstallJobSlotFix() {
    if (const char* env = std::getenv("BB_JOB_SLOT_FIX"); env && env[0] == '0') {
        return false;
    }
    constexpr u64 offset = 0x1068092;
    // mov rax, [rbx]; cmp rax, [rbp-0x30]: the stack check every exit of that function ends in.
    static constexpr std::array<u8, 7> code = {0x48, 0x8b, 0x03, 0x48, 0x3b, 0x45, 0xd0};
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), code.data(), code.size())) {
        return false;
    }
    // The destroy/reset sites first: all three or none.
    static constexpr std::array<u8, 11> drop = {0x48, 0xc7, 0x83, 0x20, 0x01, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x00};
    for (const u64 site : {u64{0x106798a}, u64{0x1067a5c}}) {
        if (std::memcmp(reinterpret_cast<const void*>(ImageBase + site), drop.data(), drop.size())) {
            return false;
        }
    }
    Emitter e;
    e.SaveVolatile();
    e.Bytes({0x4c, 0x89, 0xf7}); // mov rdi, r14 (the runner)
    // SaveVolatile keeps its stack anchor in r12; the game's r12 (the pool) is saved at [r12].
    e.Bytes({0x49, 0x8b, 0x34, 0x24}); // mov rsi, [r12] (the saved pool)
    e.Bytes({0x44, 0x89, 0xfa});       // mov edx, r15d (the result)
    e.Bytes({0x89, 0xc1});             // mov ecx, eax (the original error)
    e.CallAbs(reinterpret_cast<const void*>(&JobSlotFix));
    e.RestoreVolatile();
    for (const u8 b : code) {
        e.Bytes({b}); // the two instructions the jump covers
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + code.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    const bool dropped = InstallJobSlotDropHook(0x106798a);
    return InstallJobSlotDropHook(0x1067a5c) && dropped;
}

/// Yebis's blur-kernel sample parameters (GPUTexUtil_GetRecursiveSampleParameters, 0xbbc3e0): the
/// base sample count, its first argument, must be at least 3 (an assertion the retail build
/// compiled out). Game data now and then gives 2; the function then runs on in a state that
/// does not end (the guest stops submitting: a black screen, crash 0xa0020007). The count is
/// raised to 3 at the entry (deadinside28 0.6, their #207). BB_KERNEL_CLAMP=0: not installed.
bool InstallKernelSampleClamp() {
    if (const char* env = std::getenv("BB_KERNEL_CLAMP"); env && env[0] == '0') {
        return false;
    }
    constexpr u64 offset = 0xbbc3e0;
    // push rbp; mov rbp, rsp; push r15
    static constexpr std::array<u8, 6> entry = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    const u64 address = ImageBase + offset;
    if (std::memcmp(reinterpret_cast<const void*>(address), entry.data(), entry.size())) {
        return false;
    }
    Emitter e;
    e.Bytes({0x83, 0xff, 0x03});             // cmp edi, 3
    e.Bytes({0x7d, 0x05});                   // jge over
    e.Bytes({0xbf, 0x03, 0x00, 0x00, 0x00}); // mov edi, 3
    for (const u8 b : entry) {
        e.Bytes({b}); // the instructions the jump covers
    }
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + entry.size());
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}
} // namespace

void Install(RangeCallback on_gpu_range_allocated) {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) {
        return;
    }
#ifdef _WIN32
    AddVectoredExceptionHandler(1, OnTrapException);
#else
    struct sigaction action {};
    action.sa_sigaction = OnTrap;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTRAP, &action, &previous_action);
#endif
    // Only the PC memory model (a range callback) needs to hear of the game's GPU allocations
    // and resource copies; the crash fixes below go in with either model (deadinside28 PR #214).
    const bool memory_hooks = on_gpu_range_allocated != nullptr;
    int patched = 0;
    for (const auto& site : MemcpySites) {
        patched += memory_hooks && Patch(ImageBase + site.offset, site.code, sizeof(site.code));
    }
    on_range_allocated = on_gpu_range_allocated;
    const bool allocator =
        memory_hooks && Patch(ImageBase + AllocReturn, AllocReturnCode, sizeof(AllocReturnCode));
    // BB_HEAP_HOOKS=0 (bisecting): the heap's ranges are not reported.
    const char* heap_env = std::getenv("BB_HEAP_HOOKS");
    const bool heap_hooks = memory_hooks && (!heap_env || heap_env[0] != '0');
    if (memory_hooks && BbStats::enabled) {
        Patch(ImageBase + CollectorEntry, CollectorEntryCode, sizeof(CollectorEntryCode));
    }
    int heap_sites = 0;
    for (const u64 site : HeapAllocReturns) {
        if (!heap_hooks) {
            break;
        }
        heap_sites += Patch(ImageBase + site, HeapAllocReturnCode, sizeof(HeapAllocReturnCode));
    }
    if (memory_hooks) {
        std::printf("Guest hooks: %d of %zu resource copy sites tell the GPU side of their writes\n",
                    patched, MemcpySites.size());
        std::printf("Guest hooks: GPU heap allocations seen at %d of %zu call sites\n", heap_sites,
                    HeapAllocReturns.size());
        std::printf("Guest hooks: GPU memory allocator %s\n",
                    allocator ? "tells the GPU side of new ranges" : "not found");
    } else {
        std::printf("Guest hooks: none for the GPU memory (this memory model keeps copies)\n");
    }
    // Native detours over the traps just placed (BB_GUEST_HOOK_TRAPS=1 keeps the traps).
    const char* traps_env = std::getenv("BB_GUEST_HOOK_TRAPS");
    if (!(traps_env && traps_env[0] == '1') && (stub_page = MapStubPage()) != nullptr) {
        int detours = 0;
        for (const auto& site : MemcpySites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallCallDetour(site.offset);
            }
        }
        for (auto& site : native_sites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallDetour(site);
            }
        }
        const bool gx_guard = InstallGxNodeGuard();
        const bool job_slots = InstallJobSlotFix();
        const bool kernel_clamp = InstallKernelSampleClamp();
        SealStubPage();
        std::printf("Guest hooks: %d native detours (no traps; BB_GUEST_HOOK_TRAPS=1: int3)\n",
                    detours);
        std::printf("Guest hooks: GX command heap guard %s\n",
                    gx_guard ? "in" : "not installed (other code there)");
        std::printf("Guest hooks: render job slot fix %s\n", job_slots ? "on" : "not installed");
        std::printf("Guest hooks: blur-kernel sample count clamp %s\n",
                    kernel_clamp ? "on" : "not installed");
    }
    if (!memory_hooks) {
        return;
    }
    BbHeapSites::Install();
    if (const char* env = std::getenv("BB_HEAP_SITES"); env && env[0] == '1') {
        Patch(ImageBase + ReleaseCheck, ReleaseCheckCode, sizeof(ReleaseCheckCode));
    }
}
} // namespace BbGuestHooks
