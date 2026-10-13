// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/engine/summon_invite.cpp (the gate and the host's extra; its own
// type-1 delivery, off by default there, is not ported), sos_signs.cpp, playlog.cpp and
// chara_id.cpp (there a hook on the login's character step; here the same check from the
// calls the login makes, as this port has no code hooks).
//
// Addresses are Binary Ninja's for the 1.09 eboot at its preferred base 0x400000 - the same
// space patches/Bloodborne.xml uses; the loader's image has that base at its first byte.
#include "hd/hd_engine.h"

#include <atomic>
#include <cstring>

#include "hd/hd.h"

namespace Hd::Engine {

namespace {

constexpr std::uint64_t kPreferredBase = 0x400000;

// summon_invite.cpp
constexpr std::uint64_t kFrpgNetMan = 0x593b120;    // FrpgNetMan* (singleton slot)
constexpr std::uint64_t kSignalingGate = 0x586cc09; // u8, the NpMatching2 callbacks' gate
constexpr std::uint32_t kAreaOff = 0xa7c;           // FrpgNetMan+0xa7c: area id
constexpr std::uint32_t kLevelOff = 0xaa8;          // FrpgNetMan+0xaa8: soul level
constexpr std::uint32_t kPosOff = 0xaac;            // FrpgNetMan+0xaac: position, three floats

// sos_signs.cpp: how long a sign being answered may take, 180.0f.
constexpr std::uint64_t kSignTimeout = 0x4d27a5c;
constexpr float kGameSignTimeout = 180.0f;

// playlog.cpp: the initializer's stores, `mov dword [rip+disp], imm32`; the last four bytes
// are the float. 0x23f9b86: 300.0f into the upload period (0x5a9ecc0).
constexpr std::uint64_t kUploadStore = 0x23f9b86;
constexpr std::uint8_t kUploadExpect[] = {0xc7, 0x05, 0x30, 0x51, 0x6a, 0x03, 0x00, 0x00, 0x96, 0x43};
// 0x23f9b68: 1.5f into the position (RegularLog) period (0x5a9eca8).
constexpr std::uint64_t kSampleStore = 0x23f9b68;
constexpr std::uint8_t kSampleExpect[] = {0xc7, 0x05, 0x36, 0x51, 0x6a, 0x03, 0x00, 0x00, 0xc0, 0x3f};

// chara_id.cpp: the network login's character step. Its prologue also says whether this is
// the build the addresses were read from.
constexpr std::uint64_t kCharaStep = 0x2043ee0;
constexpr std::uint8_t kCharaStepPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40};
constexpr std::uint64_t kGameDataMan = 0x593b130;
constexpr std::uint64_t kPlayerGameData = 0x8;
constexpr std::uint64_t kCharaIdField = 0x690;
constexpr std::uint64_t kUnset = 0x8000000000000000ull;

std::uint8_t* g_image = nullptr;
std::uint64_t g_size = 0;
std::atomic<bool> g_bound{false};

// The host address of a 1.09 address, or null outside the image.
std::uint8_t* At(std::uint64_t address, std::size_t n) {
    if (!g_image || address < kPreferredBase) return nullptr;
    const std::uint64_t offset = address - kPreferredBase;
    if (offset > g_size || n > g_size - offset) return nullptr;
    return g_image + offset;
}

template <typename T>
T Rd(const void* p) {
    T v{};
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// Heap pointers the game hands out are outside the image; they are readable in this
// process, so the check is only that they are not null or tiny.
bool Plausible(std::uint64_t p) {
    return p > 0x10000;
}

const void* Heap(std::uint64_t p) {
    return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(p));
}

void PatchSignTimeout() {
    const int seconds = Cfg().sign_timeout_seconds;
    if (seconds <= 0 || seconds == 180) return; // the game's own
    std::uint8_t* p = At(kSignTimeout, sizeof(float));
    if (!p || std::memcmp(p, &kGameSignTimeout, sizeof(float)) != 0) {
        Log("sos signs: not patched: 0x%llx is not 180.0",
            static_cast<unsigned long long>(kSignTimeout));
        return;
    }
    const float value = static_cast<float>(seconds);
    std::memcpy(p, &value, sizeof(value));
    Log("sos signs: a sign entry times out after %d s (was 180)", seconds);
}

std::uint8_t* StoreAt(std::uint64_t address, const std::uint8_t* expect, std::size_t n) {
    std::uint8_t* p = At(address, n);
    if (!p || std::memcmp(p, expect, n) != 0) {
        Log("playlog: not patched: unexpected bytes at 0x%llx",
            static_cast<unsigned long long>(address));
        return nullptr;
    }
    return p;
}

void PatchPlaylog() {
    const int seconds = Cfg().playlog_upload_seconds;
    const int sample_ms = Cfg().playlog_sample_ms;
    if (seconds > 0) {
        if (std::uint8_t* upload = StoreAt(kUploadStore, kUploadExpect, sizeof(kUploadExpect))) {
            const float period = static_cast<float>(seconds);
            std::memcpy(upload + 6, &period, sizeof(period));
            Log("playlog: the game uploads its play log every %d s (was 300)", seconds);
        }
    }
    if (sample_ms > 0) {
        if (std::uint8_t* sample = StoreAt(kSampleStore, kSampleExpect, sizeof(kSampleExpect))) {
            const float period = static_cast<float>(sample_ms) / 1000.0f;
            std::memcpy(sample + 6, &period, sizeof(period));
            Log("playlog: the game logs the player's position every %d ms (was 1500)", sample_ms);
        }
    }
}

} // namespace

void SetImage(void* image, std::uint64_t size) {
    if (!Enabled() || !image || !size) return;
    if (!Cfg().eboot_109) {
        Log("engine: not game version 1.09: the summon sign timeout, play log period, signaling "
            "gate and character id fix are off");
        return;
    }
    g_image = static_cast<std::uint8_t*>(image);
    g_size = size;
    const std::uint8_t* step = At(kCharaStep, sizeof(kCharaStepPrologue));
    if (!step || std::memcmp(step, kCharaStepPrologue, sizeof(kCharaStepPrologue)) != 0) {
        Log("engine: the code at 0x%llx is not the 1.09 eboot's; the address-based online fixes "
            "are off",
            static_cast<unsigned long long>(kCharaStep));
        g_image = nullptr;
        g_size = 0;
        return;
    }
    // The image is still writable here (the loader protects its segments afterwards).
    PatchSignTimeout();
    PatchPlaylog();
    g_bound.store(true);
}

void SetSignalingGate() {
    if (!g_bound.load()) return;
    std::uint8_t* p = At(kSignalingGate, 1);
    if (!p) return;
    if (*p != 1) {
        *p = 1;
        Log("np: signaling gate set (data_586cc09 = 1)");
    }
}

void HostExtra(Json& extra) {
    if (!g_bound.load()) return;
    const std::uint8_t* slot = At(kFrpgNetMan, 8);
    if (!slot) return;
    const std::uint64_t man = Rd<std::uint64_t>(slot);
    if (!Plausible(man)) return;
    extra["HostArea"] = static_cast<long long>(Rd<std::uint32_t>(Heap(man + kAreaOff)));
    extra["HostLevel"] = static_cast<long long>(Rd<std::uint32_t>(Heap(man + kLevelOff)));
    Json pos = Json::array();
    for (int i = 0; i < 3; ++i) pos.push_back(Rd<float>(Heap(man + kPosOff + 4 * i)));
    extra["HostPos"] = std::move(pos);
    extra["MemberTag"] = 1;
}

void FixCharaId() {
    if (!g_bound.load()) return;
    const std::uint8_t* slot = At(kGameDataMan, 8);
    if (!slot) return;
    const std::uint64_t gdm = Rd<std::uint64_t>(slot);
    if (!Plausible(gdm)) return;
    const std::uint64_t pgd = Rd<std::uint64_t>(Heap(gdm + kPlayerGameData));
    if (!Plausible(pgd)) return;
    auto* field = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(pgd + kCharaIdField));
    const std::uint64_t id = Rd<std::uint64_t>(field);
    if (id == kUnset) {
        const std::uint64_t none = 0;
        std::memcpy(field, &none, sizeof(none));
        static std::atomic<int> cleared{0};
        if (cleared.fetch_add(1) < 4) {
            Log("chara id: this character has none from the server (0x8000000000000000); "
                "asking for one");
        }
    } else if (id) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) Log("chara id: %llu", static_cast<unsigned long long>(id));
    }
}

} // namespace Hd::Engine

extern "C" void bbnet_hd_set_image(void* image, std::uint64_t size) {
    Hd::Engine::SetImage(image, size);
}
