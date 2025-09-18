#pragma once

#include <stdint.h>

#define CHANNELS_SIZE 48

namespace VrxChannels {
    const uint16_t getSynthRegisterB(uint8_t index);
    const uint16_t getFrequency(uint8_t index);
    const char *getName(uint8_t index);
    const uint8_t getOrderedIndex(uint8_t index);
    const uint8_t getOrderedIndexFromIndex(uint8_t index);
}
