#pragma once

#include <stdint.h>

#define CHANNELS_SIZE 48

enum VrxBand {
    BAND_A = 0,
    BAND_B = 8,
    BAND_E = 16,
    BAND_F = 24,
    BAND_R = 32,  // Raceband
    BAND_L = 40,   // Low band
    BAND_Unknown = 0xFF   // Unknown band
};

namespace VrxChannels {
    const uint16_t getSynthRegisterB(uint8_t index);
    const uint16_t getFrequency(uint8_t index);
    const char *getName(uint8_t index);
    const uint8_t getOrderedIndex(uint8_t index);
    const uint8_t getOrderedIndexFromIndex(uint8_t index);
}
