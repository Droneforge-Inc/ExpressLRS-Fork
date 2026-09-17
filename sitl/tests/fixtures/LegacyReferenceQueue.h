#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace sitl
{
// Historical reliable-D5 experiment, used only by the native comparison mode.
// Keep one immutable active reference and one latest waiting reference.
// Alternate with ordinary management messages so neither queue starves the
// other.
class LegacyReferenceQueue
{
public:
    void offer(const uint8_t *data, uint8_t length)
    {
        if (length > pending.size())
            return;
        std::copy_n(data, length, pending.begin());
        pendingLength = length;
    }

    bool take(bool managementWaiting, uint8_t *&data, uint8_t &length)
    {
        if (!pendingLength || (referenceCompleted && managementWaiting))
        {
            referenceCompleted = false;
            return false;
        }
        active = pending;
        data = active.data();
        length = pendingLength;
        pendingLength = 0;
        referenceCompleted = true;
        return true;
    }

private:
    std::array<uint8_t, 64> pending{}, active{};
    uint8_t pendingLength = 0;
    bool referenceCompleted = false;
};
} // namespace sitl
