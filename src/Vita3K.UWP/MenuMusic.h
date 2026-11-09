// SPDX-License-Identifier: GPL-2.0-or-later
// Original synthesized interface sounds. The menu music ships as files in
// Assets/Music, made by scripts/music/compose.py. No third-party audio.
#pragma once
#include <cstdint>
#include <vector>
namespace vita3k_uwp {
// Short, original UI cues: focus (0), select (1), back (2), shelf swipe (3),
// page change (4) and confirm shimmer (5).
constexpr unsigned kInterfaceCueCount = 6;
std::vector<std::uint8_t> GenerateInterfaceCue(unsigned cue);
}
