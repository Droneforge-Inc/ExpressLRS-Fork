#pragma once

#include <stdint.h>
#include "vrx_timer.h"
#include "vrx_channels.h"

#define MIN_TUNE_TIME 25
#define RECEIVER_LAST_DELAY 50
#define RECEIVER_LAST_DATA_SIZE 24

#define RSSI_MIN_VAL 300 
#define RSSI_MAX_VAL 1200

class VRX {
private:
    uint8_t activeChannel;

    uint8_t rssi;
    uint16_t rssiRaw;
    uint8_t rssiLast[RECEIVER_LAST_DATA_SIZE];
    
    bool shouldScan;
    bool isScanning;
    bool scanAutoConnect;
    bool scanComplete;

    uint8_t scanIndex;
    uint8_t bestRssiIndex;
    uint8_t originalChannelIndex;
    uint8_t scanRssiData[CHANNELS_SIZE];

    VrxTimer rssiStableTimer;
    VrxTimer rssiLogTimer;
    VrxTimer serialLogTimer;

public:
    VRX();
    
    bool getShouldScan();
    bool getIsScanning();
    bool getScanComplete();
    uint8_t* getScanRssiData();

    void setChannel(uint8_t channel);
    void updateRssi();
    bool isRssiStable();
    
    void triggerScan(bool autoConnect);
    void startScan();
    void stopScan();

    void setup();
    void update();
    void writeSerialData();
};
