#pragma once

#include <stdint.h>

#define MIN_TUNE_TIME 25
#define RECEIVER_LAST_DELAY 50
#define RECEIVER_LAST_DATA_SIZE 24

#define RSSI_MIN_VAL 90
#define RSSI_MAX_VAL 220

class VRX {
private:
    uint8_t activeChannel;

    uint8_t rssi;
    uint16_t rssiRaw;
    uint8_t rssiLast[RECEIVER_LAST_DATA_SIZE];

public:
    void setChannel(uint8_t channel);
    uint16_t updateRssi();
    bool isRssiStable();

    void setup();
    void update();
};
