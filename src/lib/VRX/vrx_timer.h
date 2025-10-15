#pragma once

#include <stdint.h>

class VrxTimer {
    private:
        uint32_t nextTick;
        uint16_t delay;
        bool ticked;

    public:
        VrxTimer(uint16_t delay);
        const bool hasTicked();
        void reset();
};
