#pragma once
#include <stdint.h>

// USB clients can restart without an RF disconnect. Repeat the current link
// snapshot locally so discovery never depends on observing a past transition.
// This schedules no RF packets and never turns an inactive link into an active one.
class NimbusLinkSnapshot
{
public:
    bool due(uint32_t now, bool linked)
    {
        if (sent && linked == lastLinked && uint32_t(now - lastSentMs) < 1000U)
            return false;
        sent = true;
        lastLinked = linked;
        lastSentMs = now;
        return true;
    }
private:
    uint32_t lastSentMs = 0;
    bool sent = false;
    bool lastLinked = false;
};
