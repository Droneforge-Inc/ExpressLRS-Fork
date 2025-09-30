#include <Arduino.h>
#include <cstring>

#include "vrx.h"
#include "bbs_protocol.h"
#include "vrx_channels.h"
#include "targets.h"
#include "logging.h"

#include "vrx_timer.h"

static void writeSerialData();

uint8_t activeChannel = 0;

uint8_t rssi = 0;
uint16_t rssiRaw = 0;
uint8_t rssiLast[RECEIVER_LAST_DATA_SIZE] = { 0 };

bool shouldScan = false;
bool isScanning = false;
bool scanAutoConnect = false;
bool scanComplete = false;

uint8_t scanIndex = 0;
uint8_t bestRssiIndex = 0;
uint8_t originalChannelIndex = 0;
uint8_t scanRssiData[CHANNELS_SIZE] = { 0 };

static VrxTimer rssiStableTimer = VrxTimer(MIN_TUNE_TIME);
static VrxTimer rssiLogTimer = VrxTimer(RECEIVER_LAST_DELAY);
static VrxTimer serialLogTimer = VrxTimer(25);

bool VRX::getShouldScan()
{
    return shouldScan;
}

bool VRX::getIsScanning()
{
    return isScanning;
}

bool VRX::getScanComplete()
{
    return scanComplete;
}

uint8_t* VRX::getScanRssiData()
{
    return scanRssiData;
}

void VRX::setChannel(uint8_t channel)
{
    BbsProtocol::setVtxChannel(VrxChannels::getSynthRegisterB(channel));

    rssiStableTimer.reset();
    activeChannel = channel;
}

bool VRX::isRssiStable() {
    return rssiStableTimer.hasTicked();
}

void VRX::updateRssi() {
    analogRead(GPIO_PIN_VTX_RSSI); // Fake read to let ADC settle.
    rssiRaw = analogRead(GPIO_PIN_VTX_RSSI);

    rssi = constrain(
        map(
            rssiRaw,
            RSSI_MIN_VAL,
            RSSI_MAX_VAL,
            0,
            100
        ),
        0,
        100
    );

    if (rssiLogTimer.hasTicked()) {
        for (uint8_t i = 0; i < RECEIVER_LAST_DATA_SIZE - 1; i++) {
            rssiLast[i] = rssiLast[i + 1];
        }

        rssiLast[RECEIVER_LAST_DATA_SIZE - 1] = rssi;
        rssiLogTimer.reset();
    }
}

void VRX::setup() {
  if (GPIO_PIN_VTX_RSSI != UNDEF_PIN)
  {
    pinMode(GPIO_PIN_VTX_RSSI, INPUT_PULLUP);
  }
  if (GPIO_PIN_VTX_BBS_DATA != UNDEF_PIN)
  {
    pinMode(GPIO_PIN_VTX_BBS_DATA, OUTPUT);
    digitalWrite(GPIO_PIN_VTX_BBS_DATA, LOW);
  }
  if (GPIO_PIN_VTX_BBS_CS != UNDEF_PIN)
  {
    pinMode(GPIO_PIN_VTX_BBS_CS, OUTPUT);
    digitalWrite(GPIO_PIN_VTX_BBS_CS, HIGH);
  }
  if (GPIO_PIN_VTX_BBS_SCK != UNDEF_PIN)
  {
    pinMode(GPIO_PIN_VTX_BBS_SCK, OUTPUT);
    digitalWrite(GPIO_PIN_VTX_BBS_SCK, LOW);
  }

  BbsProtocol::setPowerDownRegister(0b00010000110111110011);
}

void VRX::triggerScan(bool autoConnect)
{
    shouldScan = true;
    scanAutoConnect = autoConnect;
}

void VRX::startScan()
{
    DBGLN("VRX: Starting scan. AutoConnect: %d", scanAutoConnect);
    isScanning = true;
    scanComplete = false;
    shouldScan = false;
    scanIndex = 0;
    bestRssiIndex = 0;
    originalChannelIndex = activeChannel;
    memset(scanRssiData, 0, sizeof(scanRssiData));
    setChannel(VrxChannels::getOrderedIndex(scanIndex));
}

void VRX::stopScan()
{
    DBGLN("VRX: Stopping scan");
    isScanning = false;
    scanAutoConnect = false;
    setChannel(originalChannelIndex);
}

void VRX::update() {
    if (rssiStableTimer.hasTicked()) {
        updateRssi();
        writeSerialData();
        
        // Handle scan logic during update
        if (isScanning) {
            scanRssiData[scanIndex] = rssi;
            if (rssi > scanRssiData[bestRssiIndex]) {
                bestRssiIndex = scanIndex;
            }

            scanIndex = (scanIndex + 1) % CHANNELS_SIZE;
            setChannel(VrxChannels::getOrderedIndex(scanIndex));

            if (scanIndex == 0) {
                if (scanAutoConnect) {
                    setChannel(VrxChannels::getOrderedIndex(bestRssiIndex));
                } else {
                    setChannel(originalChannelIndex);
                }

                isScanning = false;
                scanAutoConnect = false;
                scanComplete = true;
            }
        }
    }
}

static void writeSerialData() {
    if (serialLogTimer.hasTicked()) {
        DBGLN("Active channel: %d", activeChannel);
        DBGLN("RSSI: %d", rssi);
        DBGLN("RSSI raw: %d", rssiRaw);
        DBGLN("RSSI last: %d", rssiLast);
        serialLogTimer.reset();
    }
}
