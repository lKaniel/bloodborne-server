// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The server link's handshake and datagram tags (security review section 5, option 1), shared by
// shadNet (server_link.h) and the game (the emulator's net_link.h): no dependency, so both build
// it the same everywhere.
//
// - A hello without a valid cookie gets only a cookie back: HMAC(server key, the hello's source
//   address and port, a 30 s bucket), smaller than the hello. A spoofed source never sees it, so it
//   makes no link, no account and no password check, and the server keeps nothing for it.
// - The hello that echoes the cookie carries the player's secret (as before: the game's NP sign-in
//   sends the same secret through the Rpc stream anyway, so a challenge-response here would hide
//   nothing; hiding it takes encryption, option 2) and a fresh nonce of the game's. Both sides
//   derive the link's keys from the secret, the nonce, the cookie and the id.
// - Every datagram after the hello (both ways, the hello's answer too) ends in a counter (8 bytes)
//   and the first 16 bytes of HMAC-SHA256 over the datagram and the counter, one key each way.
//   A bad tag, or a counter seen already or older than the window, is dropped without a word.
//
// What it stops: anyone off the path (spoofed hellos, reflection, frames or Rpc bytes injected
// from a victim's address, replays). Someone on the path still reads the secret.
#pragma once

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "coop/coop_types.h"

namespace LinkAuth {

constexpr size_t CookieBytes = 16;
constexpr size_t NonceBytes = 16;
constexpr size_t CounterBytes = 8;
constexpr size_t TagBytes = 16;
constexpr size_t TrailerBytes = CounterBytes + TagBytes;
constexpr s64 CookieBucketMs = 30'000; // a cookie holds for 30-60 s
using Cookie = std::array<u8, CookieBytes>;
using Nonce = std::array<u8, NonceBytes>;
using Digest = std::array<u8, 32>;
using Trailer = std::array<u8, TrailerBytes>;

inline std::span<const u8> Bytes(std::string_view text) {
    return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

class Sha256 {
public:
    void Update(std::span<const u8> data) {
        total += data.size();
        size_t at = 0;
        if (used != 0) {
            const size_t take = std::min(data.size(), 64 - used);
            std::memcpy(buffer + used, data.data(), take);
            used += take;
            at = take;
            if (used < 64) {
                return;
            }
            Block(buffer);
            used = 0;
        }
        for (; data.size() - at >= 64; at += 64) {
            Block(data.data() + at);
        }
        std::memcpy(buffer, data.data() + at, data.size() - at);
        used = data.size() - at;
    }

    Digest Final() {
        const u64 bits = total * 8;
        static constexpr u8 pad[64] = {0x80};
        Update({pad, (used < 56 ? 56 : 120) - used});
        u8 length[8];
        for (int index = 0; index < 8; ++index) {
            length[index] = static_cast<u8>(bits >> (56 - 8 * index));
        }
        Update(length);
        Digest out;
        for (int index = 0; index < 8; ++index) {
            for (int byte = 0; byte < 4; ++byte) {
                out[index * 4 + byte] = static_cast<u8>(state[index] >> (24 - 8 * byte));
            }
        }
        return out;
    }

    static Digest Of(std::span<const u8> data) {
        Sha256 sha;
        sha.Update(data);
        return sha.Final();
    }

private:
    static u32 Rotr(u32 value, int bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    void Block(const u8* block) {
        static constexpr u32 k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        u32 w[64];
        for (int index = 0; index < 16; ++index) {
            w[index] = u32{block[index * 4]} << 24 | u32{block[index * 4 + 1]} << 16 |
                       u32{block[index * 4 + 2]} << 8 | u32{block[index * 4 + 3]};
        }
        for (int index = 16; index < 64; ++index) {
            const u32 s0 = Rotr(w[index - 15], 7) ^ Rotr(w[index - 15], 18) ^ (w[index - 15] >> 3);
            const u32 s1 = Rotr(w[index - 2], 17) ^ Rotr(w[index - 2], 19) ^ (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        u32 a = state[0], b = state[1], c = state[2], d = state[3];
        u32 e = state[4], f = state[5], g = state[6], h = state[7];
        for (int index = 0; index < 64; ++index) {
            const u32 t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                           k[index] + w[index];
            const u32 t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    u32 state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u8 buffer[64] = {};
    size_t used = 0;
    u64 total = 0;
};

// HMAC-SHA256 with its key's inner and outer blocks hashed once (a datagram costs its own blocks
// and two more).
class HmacKey {
public:
    HmacKey() : HmacKey(std::span<const u8>{}) {}
    explicit HmacKey(std::span<const u8> key) {
        u8 block[64] = {};
        if (key.size() > 64) {
            const Digest hashed = Sha256::Of(key);
            std::memcpy(block, hashed.data(), hashed.size());
        } else if (!key.empty()) {
            std::memcpy(block, key.data(), key.size());
        }
        u8 pad[64];
        for (int index = 0; index < 64; ++index) {
            pad[index] = block[index] ^ 0x36;
        }
        inner.Update(pad);
        for (int index = 0; index < 64; ++index) {
            pad[index] = block[index] ^ 0x5c;
        }
        outer.Update(pad);
    }

    Digest Mac(std::span<const u8> first, std::span<const u8> second = {}) const {
        Sha256 sha = inner;
        sha.Update(first);
        sha.Update(second);
        const Digest inside = sha.Final();
        Sha256 out = outer;
        out.Update(inside);
        return out.Final();
    }

private:
    Sha256 inner;
    Sha256 outer;
};

// Equal without telling, by its time, how much was.
inline bool SameBytes(std::span<const u8> a, std::span<const u8> b) {
    if (a.size() != b.size()) {
        return false;
    }
    u8 differ = 0;
    for (size_t index = 0; index < a.size(); ++index) {
        differ |= a[index] ^ b[index];
    }
    return differ == 0;
}

// The cookie for a source (address and port in host byte order) in a time bucket.
inline Cookie CookieFor(const HmacKey& server_key, u32 ipv4, u16 port, s64 bucket) {
    u8 message[14];
    for (int index = 0; index < 4; ++index) {
        message[index] = static_cast<u8>(ipv4 >> (24 - 8 * index));
    }
    message[4] = static_cast<u8>(port >> 8);
    message[5] = static_cast<u8>(port);
    for (int index = 0; index < 8; ++index) {
        message[6 + index] = static_cast<u8>(static_cast<u64>(bucket) >> (8 * index));
    }
    const Digest mac = server_key.Mac(message);
    Cookie cookie;
    std::memcpy(cookie.data(), mac.data(), cookie.size());
    return cookie;
}

// This bucket's or the last one's.
inline bool CookieValid(const HmacKey& server_key, u32 ipv4, u16 port, s64 now_ms,
                        std::span<const u8> cookie) {
    const s64 bucket = now_ms / CookieBucketMs;
    return SameBytes(cookie, CookieFor(server_key, ipv4, port, bucket)) ||
           SameBytes(cookie, CookieFor(server_key, ipv4, port, bucket - 1));
}

// Hosted: a hello that would make a new account carries a work nonce (docs/HOSTED_SERVER.md): one
// such that SHA-256(cookie, id, nonce as 8 bytes little-endian) starts with the server's number of
// zero bits. The cookie ties it to the hello's source and the next minute; the server checks it
// with one hash, before any password hash or account. An account once made never pays again.
constexpr int MaxWorkBits = 40;

inline Sha256 WorkPrefix(const Cookie& cookie, std::string_view online_id) {
    Sha256 sha;
    sha.Update(cookie);
    sha.Update(Bytes(online_id));
    return sha;
}

inline Digest WorkDigest(Sha256 prefix, u64 nonce) {
    u8 bytes[8];
    for (int index = 0; index < 8; ++index) {
        bytes[index] = static_cast<u8>(nonce >> (8 * index));
    }
    prefix.Update(bytes);
    return prefix.Final();
}

inline int LeadingZeroBits(const Digest& digest) {
    int bits = 0;
    for (const u8 byte : digest) {
        if (byte != 0) {
            for (u8 mask = 0x80; (byte & mask) == 0; mask >>= 1) {
                ++bits;
            }
            return bits;
        }
        bits += 8;
    }
    return bits;
}

inline bool WorkDone(const Cookie& cookie, std::string_view online_id, u64 nonce, int bits) {
    return bits <= 0 || (bits <= MaxWorkBits &&
                         LeadingZeroBits(WorkDigest(WorkPrefix(cookie, online_id), nonce)) >= bits);
}

// The first nonce of [from, from + count) that does the work, if one does (a caller that may have
// to stop asks in slices).
inline std::optional<u64> SolveWork(const Cookie& cookie, std::string_view online_id, int bits,
                                    u64 from, u64 count) {
    if (bits > MaxWorkBits) {
        return std::nullopt;
    }
    const Sha256 prefix = WorkPrefix(cookie, online_id);
    for (u64 nonce = from; nonce - from < count; ++nonce) {
        if (bits <= 0 || LeadingZeroBits(WorkDigest(prefix, nonce)) >= bits) {
            return nonce;
        }
    }
    return std::nullopt;
}

// The link's two keys: the game's datagrams (up) and the server's (down).
struct Keys {
    Digest up;
    Digest down;
};
inline Keys KeysFor(std::string_view secret, const Nonce& nonce, const Cookie& cookie,
                    std::string_view online_id) {
    const HmacKey from_secret{Bytes(secret)};
    Sha256 context;
    context.Update(Bytes("shadNet link 3"));
    context.Update(nonce);
    context.Update(cookie);
    context.Update(Bytes(online_id));
    const Digest base = from_secret.Mac(context.Final());
    const HmacKey from_base{base};
    return {from_base.Mac(Bytes("up")), from_base.Mac(Bytes("down"))};
}

// The counters taken, the last Size of them remembered: a datagram late by more is dropped too.
class ReplayWindow {
public:
    static constexpr u64 Size = 1024;

    bool Fresh(u64 counter) const {
        if (counter == 0) {
            return false; // counters start at 1
        }
        if (counter > top) {
            return true;
        }
        return top - counter < Size && !(bits[(counter % Size) / 64] >> (counter % 64) & 1);
    }

    void Take(u64 counter) {
        if (counter > top) {
            if (counter - top >= Size) {
                bits.fill(0);
            } else {
                for (u64 skipped = top + 1; skipped < counter; ++skipped) {
                    bits[(skipped % Size) / 64] &= ~(u64{1} << (skipped % 64));
                }
            }
            top = counter;
        }
        bits[(counter % Size) / 64] |= u64{1} << (counter % 64);
    }

private:
    u64 top = 0;
    std::array<u64, Size / 64> bits{};
};

// One side of a link: tags what it sends with one key, checks what comes with the other.
class Session {
public:
    enum class Verdict { Ok, Short, BadTag, Replay };

    Session(const Digest& send_key, const Digest& receive_key)
        : send{send_key}, receive{receive_key}, id{send_key} {}

    // The counter and tag that go after `datagram`.
    Trailer Seal(std::span<const u8> datagram) {
        Trailer trailer;
        const u64 counter = next++;
        for (size_t index = 0; index < CounterBytes; ++index) {
            trailer[index] = static_cast<u8>(counter >> (8 * index));
        }
        const Digest tag = send.Mac(datagram, std::span{trailer.data(), CounterBytes});
        std::memcpy(trailer.data() + CounterBytes, tag.data(), TagBytes);
        return trailer;
    }

    // Whether `datagram` (its trailer last) came from the other side and not before. On Ok the
    // datagram is its first size - TrailerBytes bytes.
    Verdict Open(std::span<const u8> datagram) {
        if (datagram.size() < TrailerBytes) {
            return Verdict::Short;
        }
        const size_t body = datagram.size() - TrailerBytes;
        const auto counter_bytes = datagram.subspan(body, CounterBytes);
        const Digest tag = receive.Mac(datagram.first(body), counter_bytes);
        if (!SameBytes(std::span{tag.data(), TagBytes}, datagram.subspan(body + CounterBytes))) {
            return Verdict::BadTag;
        }
        u64 counter = 0;
        for (size_t index = 0; index < CounterBytes; ++index) {
            counter |= u64{counter_bytes[index]} << (8 * index);
        }
        if (!window.Fresh(counter)) {
            return Verdict::Replay;
        }
        window.Take(counter);
        return Verdict::Ok;
    }

    // The same keys (a hello again from the same game and nonce keeps its session and counters).
    bool SameKeys(const Session& other) const {
        return id == other.id;
    }

private:
    HmacKey send;
    HmacKey receive;
    Digest id;
    u64 next = 1;
    ReplayWindow window;
};

// The server's side of a link.
inline Session ServerSession(const Keys& keys) {
    return {keys.down, keys.up};
}

// The game's side: its nonce, the server's cookie, the session of the hello under way (pending)
// and the one the server welcomed (current).
class Client {
public:
    // A new link (configured, or the server may have restarted): a new nonce; the cookie stays.
    void Restart(const Nonce& fresh) {
        nonce = fresh;
        pending.reset();
    }
    void TakeCookie(const Cookie& from_server) {
        cookie = from_server;
        has_cookie = true;
    }
    bool HasCookie() const {
        return has_cookie;
    }
    const Cookie& CookieToEcho() const {
        return cookie;
    }
    const Nonce& NonceToSend() const {
        return nonce;
    }

    // Hosted: the server asked this id's hello for work (no account yet). Whether the work is to
    // be done now: not when it is done or under way for this cookie. A new nonce goes with it: the
    // hello's session took the server's answer's counter, and the welcome starts at that one too.
    bool WorkAsked(int bits, const Nonce& fresh) {
        if (bits == work_bits && (working || WorkToSend())) {
            return false;
        }
        work_bits = bits;
        work.reset();
        working = true;
        Restart(fresh);
        return true;
    }
    bool Working() const {
        return working;
    }
    int WorkBits() const {
        return work_bits;
    }
    // The work done for a cookie (none: given up); kept for the hellos that echo that cookie.
    void TakeWork(const Cookie& done_for, int bits, std::optional<u64> work_nonce) {
        working = false;
        if (work_nonce && bits == work_bits) {
            work = std::pair{done_for, *work_nonce};
        }
    }
    std::optional<u64> WorkToSend() const {
        if (work && has_cookie && SameBytes(work->first, cookie)) {
            return work->second;
        }
        return std::nullopt;
    }

    // The session a hello with this cookie and nonce makes (none without a cookie: that hello
    // only asks for one).
    void ForHello(std::string_view secret, std::string_view online_id) {
        if (!has_cookie) {
            return;
        }
        const Keys keys = KeysFor(secret, nonce, cookie, online_id);
        Session made{keys.up, keys.down};
        if ((current && current->SameKeys(made)) || (pending && pending->SameKeys(made))) {
            return;
        }
        pending.emplace(std::move(made));
    }

    // The trailer for a datagram to the server, nothing while there is no session.
    std::optional<Trailer> Seal(std::span<const u8> datagram) {
        if (current) {
            return current->Seal(datagram);
        }
        if (pending) {
            return pending->Seal(datagram);
        }
        return std::nullopt;
    }

    enum class From { Nobody, Current, Pending };
    // Which session a datagram from the server opens with (its trailer last).
    From Open(std::span<const u8> datagram) {
        if (current && current->Open(datagram) == Session::Verdict::Ok) {
            return From::Current;
        }
        if (pending && pending->Open(datagram) == Session::Verdict::Ok) {
            return From::Pending;
        }
        return From::Nobody;
    }

    // The server welcomed the pending hello: its session is the link's.
    void Welcomed() {
        if (pending) {
            current = std::move(pending);
            pending.reset();
        }
    }

private:
    Nonce nonce{};
    Cookie cookie{};
    bool has_cookie = false;
    int work_bits = 0;
    bool working = false;
    std::optional<std::pair<Cookie, u64>> work;
    std::optional<Session> current;
    std::optional<Session> pending;
};

} // namespace LinkAuth
