#include "fan_profile.hpp"

namespace prospero {

bool BuildFanTargetProfile(const FanAutoservoProfile &current, int target_c,
                           FanAutoservoProfile *request) {
    if (!request || target_c < 0 || target_c > 255) return false;
    *request = current;
    (*request)[kFanTargetOffset] = static_cast<uint8_t>(target_c);
    return true;
}

bool FanTargetMatches(const FanAutoservoProfile &profile, int target_c) {
    return target_c >= 0 && target_c <= 255 &&
           profile[kFanTargetOffset] == static_cast<uint8_t>(target_c);
}

} // namespace prospero
