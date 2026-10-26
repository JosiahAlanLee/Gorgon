// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Gorgon contributors. See the repository LICENSE.
#ifndef GORGON_SOUND_LINK_V1_HPP
#define GORGON_SOUND_LINK_V1_HPP
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace gorgon_sound {
constexpr uint64_t MAGIC = 0x474f5247534e4431ull; // GORGSND1
constexpr uint32_t VERSION = 1;
constexpr int LANES = 4, PARAMS = 8;
struct Sound {
    int16_t machine = -1;
    uint16_t mask = 0;
    float values[PARAMS] = {};
};
struct Lane {
    Sound sound;
    float gate = 0, velocity = 0, pitch = 0;
};
struct Packet {
    uint64_t magic = MAGIC;
    uint32_t version = VERSION, bytes = sizeof(Packet);
    int64_t frame = -1, transport = -1;
    float reset = 0;
    uint32_t moving = 0;
    Lane lanes[LANES];
};
static_assert(sizeof(Sound) == 36 && sizeof(Lane) == 48 && sizeof(Packet) == 232, "Sound link ABI layout changed");
static_assert(std::is_trivially_copyable<Packet>::value, "Sound packets must be copyable without allocation");
inline bool valid(const Packet& p, int64_t now) {
    if (p.magic != MAGIC || p.version != VERSION || p.bytes != sizeof(Packet) ||
        p.frame < 0 || p.frame > now || now - p.frame > 2 || p.moving > 1 || !std::isfinite(p.reset)) return false;
    for (const auto& l : p.lanes) {
        if (l.sound.machine < -1 || l.sound.machine > 191 || l.sound.mask > 255 ||
            !std::isfinite(l.gate) || !std::isfinite(l.pitch) || !std::isfinite(l.velocity) || l.velocity < 0 || l.velocity > 1) return false;
        for (int i = 0; i < PARAMS; ++i)
            if ((l.sound.mask & (1u << i)) && (!std::isfinite(l.sound.values[i]) || l.sound.values[i] < 0 || l.sound.values[i] > 127)) return false;
    }
    return true;
}
} // namespace gorgon_sound

#endif // GORGON_SOUND_LINK_V1_HPP
