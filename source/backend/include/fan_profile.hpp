#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace prospero {

constexpr std::size_t kFanAutoservoSize = 28;
constexpr std::size_t kFanTargetOffset = 5;
using FanAutoservoProfile = std::array<uint8_t, kFanAutoservoSize>;

bool BuildFanTargetProfile(const FanAutoservoProfile &current, int target_c,
                           FanAutoservoProfile *request);
bool FanTargetMatches(const FanAutoservoProfile &profile, int target_c);

} // namespace prospero
