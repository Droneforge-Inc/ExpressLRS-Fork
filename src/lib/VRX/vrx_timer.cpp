#include <Arduino.h>
#include "vrx_timer.h"

VrxTimer::VrxTimer(uint16_t delay) {
    this->delay = delay;
    this->nextTick = millis() + this->delay;
    this->ticked = false;
}

const bool VrxTimer::hasTicked() {
    if (this->ticked)
        return true;

    if (millis() >= this->nextTick) {
        this->ticked = true;
        return true;
    }

    return false;
}

void VrxTimer::reset() {
    this->nextTick = millis() + this->delay;
    this->ticked = false;
}
