#pragma once

#include <cstdint>
#include <stdint.h>
#include "vrx_timer.h"
#include "vrx_channels.h"

#define MIN_TUNE_TIME 25
#define RECEIVER_LAST_DELAY 50
#define RECEIVER_LAST_DATA_SIZE 24

#define RSSI_MIN_VAL 300
#define RSSI_MAX_VAL 1200

#define NUM_RSSI_SAMPLES 3
#define CHANNEL_SWITCH_DELAY 50

class VRX {
private:
    uint8_t activeChannel;

    uint8_t rssi;
    uint16_t rssiRaw;
    uint8_t rssiLast[RECEIVER_LAST_DATA_SIZE];

    // RSSI sampling state
    uint16_t rssiSamples[NUM_RSSI_SAMPLES];
    uint8_t rssiSampleCount;

    bool shouldScan;
    bool isScanning;
    bool scanAutoConnect;
    bool scanComplete;

    bool shouldConnect;
    VrxBand connectBand;
    uint8_t connectChannel;

    uint8_t scanIndex;
    uint8_t bestRssiIndex;
    uint8_t originalChannelIndex;
    uint8_t scanRssiData[CHANNELS_SIZE];

    VrxTimer rssiSampleTimer;
    VrxTimer rssiStableTimer;
    VrxTimer rssiLogTimer;
    VrxTimer serialLogTimer;

public:
    VRX();

    bool getShouldScan();
    bool getIsScanning();
    bool getScanComplete();
    bool getShouldConnect();
    uint8_t* getScanRssiData();

    void setChannel(uint8_t channel);
    bool updateRssi(bool useAveraging);
    bool isRssiStable();

    void triggerScan(bool autoConnect);
    void startScan();
    bool setFrequency(uint16_t frequencyMHz);
    void stopScan();

    void triggerConnect(uint8_t band, uint8_t channel);
    void connect();

    void setup();
    void update();
    void writeSerialData();
};
