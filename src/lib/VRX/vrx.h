#pragma once

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
#define VRX_RECEIVER_COUNT 2
#define DIVERSITY_HYSTERESIS 5
#define DIVERSITY_HYSTERESIS_PERIOD 5

enum class VrxReceiver : uint8_t {
    VRX1 = 0,
    VRX2 = 1
};

class VRX {
private:
    uint8_t activeChannel;
    VrxReceiver activeReceiver;
    VrxReceiver diversityTargetReceiver;

    uint8_t rssiValue;
    uint16_t rssiRawValue;
    uint8_t receiverRssi[VRX_RECEIVER_COUNT];
    uint16_t receiverRssiRaw[VRX_RECEIVER_COUNT];
    uint8_t rssiLast[RECEIVER_LAST_DATA_SIZE];
    
    // RSSI sampling state
    uint16_t rssiSamples[VRX_RECEIVER_COUNT][NUM_RSSI_SAMPLES];
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
    VrxTimer diversityHysteresisTimer;

    bool useDiversity();
    uint16_t readRssiRaw(uint8_t receiver);
    uint8_t mapRssi(uint16_t raw);
    void updateCombinedRssi();
    void logRssi();
    void setActiveReceiver(VrxReceiver receiver);
    void updateDiversity();

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
    void stopScan();

    void triggerConnect(uint8_t band, uint8_t channel);
    void connect();

    void setup();
    void update();
    void writeSerialData();
};
