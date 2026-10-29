// The PC memory model's hooks in the game's code (gpu/shim/bbport_guest_hooks.cpp), run against
// small functions this test writes at the hooked offsets of the 1.09 eboot (image 0x800000000):
// the GPU memory range allocator's return, a GPU heap call site and a resource loader's memcpy
// call. Native detours by default; "traps" runs them as int3 traps (BB_GUEST_HOOK_TRAPS=1:
// SIGTRAP on Linux, the vectored breakpoint handler on Windows). The game's code calls with the
// System V ABI, as the test's calls into these functions do.
//   ninja -C out/gpu guest-hooks-test && out/gpu/guest-hooks-test.exe && out/gpu/guest-hooks-test.exe traps
#include "bbport_guest_hooks.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

extern "C" std::uint64_t runtime_disabled_optimizations = 0;
extern "C" std::uint64_t runtime_experiment_bits = 0;

namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;
#define GUEST_ABI __attribute__((sysv_abi))

constexpr u64 ImageBase = 0x800000000ull;
// Up to the HUD wrapper's vtable (0x539a070), the aspect hooks' last address.
constexpr u64 ImageSize = 0x5400000ull;
constexpr u64 AllocReturn = 0x26aa255, HeapReturn = 0x11e6629, CopyCall = 0x216d5ea,
              Memcpy = 0x261a1b0, GxNodeSite = 0x2199310, GxNodeSkip = 0x219934e;
// The output's aspect ratio: a camera parameter store, a HUD marker placement and its constants,
// Scaleform's SetViewport and the in-game screen's update.
// The GX occlusion queries: begin and end, the creation (its exit) and the release.
constexpr u64 GxQueryBegin = 0xe2f130, GxQueryEnd = 0xe2f160, GxQueryCreate = 0xe2f080,
              GxQueryRelease = 0xe2f020, GxQueryCreateExit = 0x21675b0;
constexpr u64 CameraStore = 0x1436c2c, MarkerSite = 0x1644379, MarkerScale = 0x48f9a00,
              MarkerBias = 0x48f9a10, SetViewport = 0x1f510, HudFrame = 0x1bf9e10;
using v4 = float __attribute__((vector_size(16)));

u64 noted_address = 0, noted_size = 0, range_start = 0, range_size = 0;
int ranges = 0;

u8* Put(u64 offset, std::initializer_list<u8> bytes) {
    u8* at = reinterpret_cast<u8*>(ImageBase + offset);
    for (const u8 b : bytes) {
        *at++ = b;
    }
    return at;
}
u8* Put64(u8* at, u64 v) {
    std::memcpy(at, &v, 8);
    return at + 8;
}
u8* Put32(u8* at, std::uint32_t v) {
    std::memcpy(at, &v, 4);
    return at + 4;
}
u8* More(u8* at, std::initializer_list<u8> bytes) {
    for (const u8 b : bytes) {
        *at++ = b;
    }
    return at;
}

/// A function ending in the allocator's epilogue: returns `node` through `add rsp, 0x18; pop rbx`
/// at AllocReturn.
u64 WriteAllocFunction(u64 node) {
    const u64 entry = AllocReturn - 15;
    u8* at = Put(entry, {0x53, 0x48, 0x83, 0xec, 0x18, 0x48, 0xb8}); // push rbx; sub rsp, 0x18; mov rax,
    at = Put64(at, node);
    More(at, {0x48, 0x83, 0xc4, 0x18, 0x5b, 0xc3}); // add rsp, 0x18; pop rbx; ret
    return ImageBase + entry;
}

/// A heap caller: [rbp - 0x50] the size, rax the address, then `mov rbx, rax; test rbx, rbx` at
/// HeapReturn; returns rbx.
u64 WriteHeapFunction(u64 address, std::uint32_t size) {
    const u64 entry = HeapReturn - 27;
    // push rbp; mov rbp, rsp; push rbx; sub rsp, 0x58; mov qword [rbp - 0x50], size
    u8* at = Put(entry, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x58, 0x48, 0xc7, 0x45, 0xb0});
    at = Put32(at, size);
    at = More(at, {0x48, 0xb8}); // mov rax, address
    at = Put64(at, address);
    // mov rbx, rax; test rbx, rbx; mov rax, rbx; add rsp, 0x58; pop rbx; pop rbp; ret
    More(at, {0x48, 0x89, 0xc3, 0x48, 0x85, 0xdb, 0x48, 0x89, 0xd8, 0x48, 0x83, 0xc4, 0x58, 0x5b, 0x5d,
              0xc3});
    return ImageBase + entry;
}

/// A resource loader's copy: `call memcpy` at CopyCall; the game's memcpy here copies nothing.
u64 WriteCopyFunction() {
    const u64 entry = CopyCall - 4;
    // sub rsp, 8; call memcpy; add rsp, 8; ret
    Put(entry, {0x48, 0x83, 0xec, 0x08, 0xe8, 0xc1, 0xcb, 0x4a, 0x00, 0x48, 0x83, 0xc4, 0x08, 0xc3});
    Put(Memcpy, {0x48, 0x89, 0xf8, 0xc3}); // mov rax, rdi; ret
    return ImageBase + entry;
}

/// The GX per-frame render command (rdi the node, rsi the value): `mov [r12 + 8], r13` at
/// GxNodeSite, then 1 when it went on, 0 from GxNodeSkip (where the game skips the post).
u64 WriteGxNodeFunction() {
    const u64 entry = GxNodeSite - 15;
    // push r12; push r13; push r15; mov r12, rdi; mov r13, rsi; mov r15, rsp
    Put(entry, {0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x49, 0x89, 0xfc, 0x49, 0x89, 0xf5, 0x49, 0x89,
                0xe7});
    // mov [r12 + 8], r13; mov rax, [r15 + 0xb0]; mov eax, 1; jmp done
    Put(GxNodeSite, {0x4d, 0x89, 0x6c, 0x24, 0x08, 0x49, 0x8b, 0x87, 0xb0, 0x00, 0x00, 0x00, 0xb8,
                     0x01, 0x00, 0x00, 0x00, 0xe9, 0x2a, 0x00, 0x00, 0x00});
    // xor eax, eax; done: pop r15; pop r13; pop r12; ret
    Put(GxNodeSkip, {0x31, 0xc0, 0x41, 0x5f, 0x41, 0x5d, 0x41, 0x5c, 0xc3});
    return ImageBase + entry;
}
/// The GX query's begin (0xe2f130) and end (0xe2f160), rdi the query ([0] its handle, [+8] its
/// state): the game's code up to the state, then a read through the handle as the game's begin
/// (0x2169ec0) and end (0x216a070) do (the fault at 0x2169edc without one), then `mov al, 1`.
void WriteGxQueryFunctions() {
    // push rbp; mov rbp, rsp; cmp dword [rdi + 8], 0; jne +0x1d; mov dword [rdi + 8], 2
    u8* at = Put(GxQueryBegin, {0x55, 0x48, 0x89, 0xe5, 0x83, 0x7f, 0x08, 0x00, 0x75, 0x1d, 0xc7, 0x47,
                                0x08, 0x02, 0x00, 0x00, 0x00});
    at = More(at, {0x48, 0x8b, 0x07, 0x48, 0x8b, 0x00}); // mov rax, [rdi]; mov rax, [rax]
    while (at < reinterpret_cast<u8*>(ImageBase + GxQueryBegin + 0x27)) {
        at = More(at, {0x90});
    }
    More(at, {0xb0, 0x01, 0x5d, 0xc3}); // 0xe2f157: mov al, 1; pop rbp; ret
    // push rbp; mov rbp, rsp; mov dword [rdi + 8], 1; mov rax, [rdi]; mov rax, [rax]
    at = Put(GxQueryEnd, {0x55, 0x48, 0x89, 0xe5, 0xc7, 0x47, 0x08, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b,
                          0x07, 0x48, 0x8b, 0x00});
    while (at < reinterpret_cast<u8*>(ImageBase + GxQueryEnd + 0x21)) {
        at = More(at, {0x90});
    }
    More(at, {0xb0, 0x01, 0x5d, 0xc3}); // 0xe2f181
    // The release (0xe2f020): mov qword [rdi], 0; ret
    Put(GxQueryRelease, {0x48, 0xc7, 0x07, 0x00, 0x00, 0x00, 0x00, 0xc3});
}

/// The creation (0xe2f080(query, kind) -> its exit at 0x21675b0): the handle from `handle_from`
/// ([0] the handle to give, 0: the allocation fails), result 0 or 9, through the game's exit
/// `mov rcx, [r15]; cmp rcx, [rbp - 0x20]` with rbx the handle and r14 the query.
void WriteGxQueryCreate(const u64* handle_from) {
    // The creation's entry jumps to the code before its exit.
    constexpr u64 Body = GxQueryCreateExit - 0x40;
    u8* at = Put(GxQueryCreate, {0xe9});
    Put32(at, std::uint32_t(std::int32_t(Body - (GxQueryCreate + 5))));
    // push rbp; mov rbp, rsp; push r15; push r14; push rbx; sub rsp, 0x28
    at = Put(Body, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x28});
    // mov qword [rbp - 0x20], 0; lea r15, [rbp - 0x20]; mov r14, rdi; mov rax, handle_from
    at = More(at, {0x48, 0xc7, 0x45, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x8d, 0x7d, 0xe0, 0x49, 0x89,
                   0xfe, 0x48, 0xb8});
    at = Put64(at, reinterpret_cast<u64>(handle_from));
    // mov rbx, [rax]; mov [r14], rbx; xor eax, eax; test rbx, rbx; jnz exit; mov eax, 9
    at = More(at, {0x48, 0x8b, 0x18, 0x49, 0x89, 0x1e, 0x31, 0xc0, 0x48, 0x85, 0xdb, 0x75, 0x05, 0xb8,
                   0x09, 0x00, 0x00, 0x00});
    while (at < reinterpret_cast<u8*>(ImageBase + GxQueryCreateExit)) {
        at = More(at, {0x90});
    }
    // mov rcx, [r15]; cmp rcx, [rbp - 0x20]; add rsp, 0x28; pop rbx; pop r14; pop r15; pop rbp;
    // test eax, eax; sete al; ret (the game's: true when created)
    More(at, {0x49, 0x8b, 0x0f, 0x48, 0x3b, 0x4d, 0xe0, 0x48, 0x83, 0xc4, 0x28, 0x5b, 0x41, 0x5e, 0x41,
              0x5f, 0x5d, 0x85, 0xc0, 0x0f, 0x94, 0xc0, 0xc3});
}

/// The camera manager's copy (rdi the current camera): `vmovss %xmm0, 0x5c(%rbx)` at CameraStore.
u64 WriteCameraFunction() {
    const u64 entry = CameraStore - 4;
    // push rbx; mov rbx, rdi; vmovss [rbx + 0x5c], xmm0; pop rbx; ret
    Put(entry, {0x53, 0x48, 0x89, 0xfb, 0xc5, 0xfa, 0x11, 0x43, 0x5c, 0x5b, 0xc3});
    return ImageBase + entry;
}

/// A marker's place: `vmulps scale(%rip), %xmm0, %xmm0; vaddps bias(%rip), %xmm0, %xmm0; ret`.
u64 WriteMarkerFunction() {
    const auto rel = [](u64 at, u64 target) { return std::uint32_t(std::int32_t(target - (at + 8))); };
    u8* at = Put(MarkerSite, {0xc5, 0xf8, 0x59, 0x05});
    at = Put32(at, rel(MarkerSite, MarkerScale));
    at = More(at, {0xc5, 0xf8, 0x58, 0x05});
    at = Put32(at, rel(MarkerSite + 8, MarkerBias));
    More(at, {0xc3});
    const float scale[4] = {0.5f, -0.5f, 1.0f, 1.0f}, bias[4] = {0.5f, 0.5f, 0.0f, 0.0f};
    std::memcpy(reinterpret_cast<void*>(ImageBase + MarkerScale), scale, sizeof(scale));
    std::memcpy(reinterpret_cast<void*>(ImageBase + MarkerBias), bias, sizeof(bias));
    return ImageBase + MarkerSite;
}

/// A function entry `push rbp; mov rbp, rsp; push r15` that returns at once.
u64 WriteEntryFunction(u64 offset) {
    Put(offset, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x5f, 0x5d, 0xc3});
    return ImageBase + offset;
}
} // namespace

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size) {
    noted_address = address;
    noted_size = size;
}

// The render job slot fix checks its runner is mapped (not exercised here).
extern "C" int runtime_memory_is_mapped(uintptr_t, uint64_t) {
    return 0;
}

int main(int argc, char** argv) {
    const bool traps = argc > 1 && std::strcmp(argv[1], "traps") == 0;
    const bool crowded = argc > 1 && std::strcmp(argv[1], "crowded") == 0;
    if (traps) {
#ifdef _WIN32
        _putenv_s("BB_GUEST_HOOK_TRAPS", "1");
#else
        setenv("BB_GUEST_HOOK_TRAPS", "1", 1);
#endif
    }
#ifdef _WIN32
    void* image = VirtualAlloc(reinterpret_cast<void*>(ImageBase), ImageSize, MEM_RESERVE | MEM_COMMIT,
                               PAGE_EXECUTE_READWRITE);
#else
    void* image = mmap(reinterpret_cast<void*>(ImageBase), ImageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (image == MAP_FAILED) {
        image = nullptr;
    }
#endif
    assert(image == reinterpret_cast<void*>(ImageBase));
    // "crowded": something else (on a player's PC, 2026-10-25: NVIDIA's driver with DLSS frame
    // generation) holds the gigabyte below the image, where the detour stubs used to go only.
    // All of the reachable space below is taken here, so the stubs must go above the image.
    if (crowded) {
        // In 16 MiB pieces: a piece something else already holds is taken anyway.
        constexpr u64 Crowd = 0x71000000ull, Piece = 16ull << 20;
        for (u64 at = ImageBase - Crowd; at < ImageBase; at += Piece) {
#ifdef _WIN32
            VirtualAlloc(reinterpret_cast<void*>(at), Piece, MEM_RESERVE, PAGE_NOACCESS);
#else
            mmap(reinterpret_cast<void*>(at), Piece, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
#endif
        }
    }

    static u64 next[2] = {0, 0x5000 + 0x3000};
    static u64 node[4] = {0, 0x5000, 0, reinterpret_cast<u64>(next)};
    using Call = GUEST_ABI u64 (*)();
    using Copy = GUEST_ABI void* (*)(void*, const void*, std::size_t);
    const auto alloc_fn = reinterpret_cast<Call>(WriteAllocFunction(reinterpret_cast<u64>(node)));
    const auto heap_fn = reinterpret_cast<Call>(WriteHeapFunction(0x7700000, 0x4000));
    const auto copy_fn = reinterpret_cast<Copy>(WriteCopyFunction());
    using GxNode = GUEST_ABI u64 (*)(u64*, u64);
    const auto gx_fn = reinterpret_cast<GxNode>(WriteGxNodeFunction());
    using CameraCopy = GUEST_ABI void (*)(float* camera, float far);
    const auto camera_fn = reinterpret_cast<CameraCopy>(WriteCameraFunction());
    using Marker = GUEST_ABI v4 (*)(v4);
    const auto marker_fn = reinterpret_cast<Marker>(WriteMarkerFunction());
    using Viewport = GUEST_ABI void (*)(void* movie, std::int32_t* viewport);
    const auto viewport_fn = reinterpret_cast<Viewport>(WriteEntryFunction(SetViewport));
    using Screen = GUEST_ABI void (*)(u64 screen);
    const auto screen_fn = reinterpret_cast<Screen>(WriteEntryFunction(HudFrame));
    static u64 gx_handle_data[3] = {};
    static u64 gx_next_handle[1] = {reinterpret_cast<u64>(gx_handle_data)};
    WriteGxQueryFunctions();
    WriteGxQueryCreate(gx_next_handle);
    using GxQueryCall = GUEST_ABI u8 (*)(u64* query);
    using GxCreate = GUEST_ABI u8 (*)(u64* query, std::uint32_t kind);
    const auto gx_begin = reinterpret_cast<GxQueryCall>(ImageBase + GxQueryBegin);
    const auto gx_end = reinterpret_cast<GxQueryCall>(ImageBase + GxQueryEnd);
    const auto gx_create = reinterpret_cast<GxCreate>(ImageBase + GxQueryCreate);

    BbGuestHooks::Install([](u64 address, u64 size) {
        range_start = address;
        range_size = size;
        ++ranges;
    });
    const u8* code = reinterpret_cast<const u8*>(ImageBase);
    // Detours: jumps over the sites; traps: the int3s stay.
    const u8 expected_alloc = traps ? 0xcc : 0xe9, expected_call = traps ? 0xcc : 0xe8;
    assert(code[AllocReturn] == expected_alloc && code[HeapReturn] == expected_alloc &&
           code[CopyCall] == expected_call);

    for (int round = 0; round < 2; ++round) {
        ranges = 0;
        assert(alloc_fn() == reinterpret_cast<u64>(node));
        assert(ranges == 1 && range_start == 0x5000 && range_size == 0x3000);
        assert(heap_fn() == 0x7700000);
        assert(ranges == 2 && range_start == 0x7700000 && range_size == 0x4000);
        char from[64] = "resource data", to[64] = {};
        noted_address = noted_size = 0;
        assert(copy_fn(to, from, sizeof(from)) == to);
        assert(std::strcmp(to, "resource data") == 0);
        assert(noted_address == reinterpret_cast<u64>(to) && noted_size == sizeof(from));
    }
    // The GX command heap guard (a detour only): a node is written as before; without one the
    // game's write to address 8 is skipped with the post.
    if (!traps) {
        assert(code[GxNodeSite] == 0xe9);
        for (int round = 0; round < 2; ++round) {
            u64 gx_node[2] = {0, 0};
            assert(gx_fn(gx_node, 0x1234) == 1 && gx_node[1] == 0x1234);
            assert(gx_fn(nullptr, 0x1234) == 0);
        }
    }
    // The GX occlusion query guard (detours only; shadPS4's crash 0x2169edc): a query without a
    // handle is never begun or ended; one whose creation failed is created again at its begin.
    if (!traps) {
        assert(code[GxQueryBegin + 4] == 0xe9 && code[GxQueryEnd + 4] == 0xe9 &&
               code[GxQueryCreateExit] == 0xe9);
        // Created: begun and ended as before.
        u64 created[6] = {};
        assert(gx_create(created, 1) == 1 && created[0] == reinterpret_cast<u64>(gx_handle_data));
        assert(gx_begin(created) == 1 && std::uint32_t(created[1]) == 2);
        assert(gx_end(created) == 1 && std::uint32_t(created[1]) == 1);
        // Its creation failed (no memory): created again when it begins, then begun.
        u64 failed[6] = {};
        gx_next_handle[0] = 0;
        assert(gx_create(failed, 1) == 0 && failed[0] == 0);
        gx_next_handle[0] = reinterpret_cast<u64>(gx_handle_data);
        assert(gx_begin(failed) == 1 && failed[0] != 0 && std::uint32_t(failed[1]) == 2);
        assert(gx_end(failed) == 1 && std::uint32_t(failed[1]) == 1);
        // Still no memory at its begin: not begun, not ended, its state stays 0 (no fault).
        u64 still[6] = {};
        gx_next_handle[0] = 0;
        assert(gx_create(still, 1) == 0);
        assert(gx_begin(still) == 1 && still[0] == 0 && std::uint32_t(still[1]) == 0);
        assert(gx_end(still) == 1 && std::uint32_t(still[1]) == 0);
        // No failed creation seen (released?): only skipped, never created into.
        gx_next_handle[0] = reinterpret_cast<u64>(gx_handle_data);
        u64 released[6] = {};
        assert(gx_begin(released) == 1 && released[0] == 0 && std::uint32_t(released[1]) == 0);
        assert(gx_end(released) == 1 && std::uint32_t(released[1]) == 0);
    }
    // The output's aspect ratio (detours only; traps: not installed, the output keeps 16:9).
    assert(BbGuestHooks::AspectSupported() == !traps);
    if (!traps) {
        assert(code[CameraStore] == 0xe9 && code[MarkerSite] == 0xe9 && code[SetViewport] == 0xe9 &&
               code[HudFrame] == 0xe9);
        float camera[24] = {};
        const float fov = 0.75f;
        const auto copy = [&](float aspect_then) {
            camera[0x50 / 4] = fov;
            camera[0x54 / 4] = aspect_then;
            camera_fn(camera, 5000.0f);
            assert(camera[0x5c / 4] == 5000.0f && "the game's own store still runs");
        };
        copy(16.0f / 9.0f);
        assert(camera[0x54 / 4] == 16.0f / 9.0f && camera[0x50 / 4] == fov && "16:9: as the game has it");
        // 21:9: the same height, more to the sides.
        BbGuestHooks::SetDisplayAspect(2560.0f / 1080.0f);
        copy(16.0f / 9.0f);
        assert(camera[0x54 / 4] == 2560.0f / 1080.0f && camera[0x50 / 4] == fov);
        // A marker in the middle stays there; one at the 16:9 screen's right edge (NDC x 1) lands
        // at the output's edge, 4/3 of the half width out in the 16:9 UI.
        const v4 middle = marker_fn(v4{0.0f, 0.0f, 0.0f, 0.0f});
        assert(middle[0] == 0.5f && middle[1] == 0.5f);
        const v4 edge = marker_fn(v4{1.0f, 1.0f, 0.0f, 0.0f});
        assert(std::abs(edge[0] - (0.5f + 0.5f * 2560.0f / 1080.0f * 9.0f / 16.0f)) < 1e-5f);
        assert(edge[1] == 0.0f);
        const auto shift = BbGuestHooks::HudEdgeShift();
        assert(std::abs(shift.dx - 320.0f) < 0.01f && shift.dy == 0.0f);
        // The game's full-screen movies get the output's pixel aspect ratio; others keep theirs.
        std::int32_t full[16] = {1920, 1080, 0, 0, 1920, 1080};
        std::int32_t small[16] = {512, 512, 0, 0, 512, 512};
        const float one = 1.0f;
        std::memcpy(full + 12, &one, 4);
        std::memcpy(small + 12, &one, 4);
        char movie[0x100] = {};
        viewport_fn(movie, full);
        viewport_fn(movie, small);
        float pixel_aspect = 0.0f;
        std::memcpy(&pixel_aspect, full + 12, 4);
        assert(std::abs(pixel_aspect - 4.0f / 3.0f) < 1e-5f);
        std::memcpy(&pixel_aspect, small + 12, 4);
        assert(pixel_aspect == 1.0f);
        // The screen update without a HUD yet: nothing to move.
        static u8 screen[0xb00] = {};
        screen_fn(reinterpret_cast<u64>(screen));
        // 4:3: the same width, the field of view widened (more above and below).
        BbGuestHooks::SetDisplayAspect(4.0f / 3.0f);
        copy(16.0f / 9.0f);
        const float widened = 2.0f * std::atan(std::tan(fov / 2.0f) * (16.0f / 9.0f) / (4.0f / 3.0f));
        assert(camera[0x54 / 4] == 4.0f / 3.0f && std::abs(camera[0x50 / 4] - widened) < 1e-5f);
        assert(std::abs(BbGuestHooks::HudEdgeShift().dy - 180.0f) < 0.01f);
        const v4 top = marker_fn(v4{0.0f, 1.0f, 0.0f, 0.0f});
        assert(std::abs(top[1] - (0.5f - 0.5f * (16.0f / 9.0f) / (4.0f / 3.0f))) < 1e-5f);
        // Back to 16:9: the game's values.
        BbGuestHooks::SetDisplayAspect(16.0f / 9.0f);
        copy(16.0f / 9.0f);
        assert(camera[0x54 / 4] == 16.0f / 9.0f && camera[0x50 / 4] == fov);
        assert(BbGuestHooks::HudEdgeShift().dx == 0.0f && BbGuestHooks::HudEdgeShift().dy == 0.0f);
        viewport_fn(movie, full);
        std::memcpy(&pixel_aspect, full + 12, 4);
        assert(pixel_aspect == 1.0f);
    }
    std::printf("guest hooks (%s): ok\n", traps ? "int3 traps" : "native detours");
    return 0;
}
