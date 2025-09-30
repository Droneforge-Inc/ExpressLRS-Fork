#include <Arduino.h>
#include <cstring>

#include "vrx.h"
#include "bbs_protocol.h"
#include "vrx_channels.h"
#include "targets.h"
#include "logging.h"

#include "vrx_timer.h"


VRX::VRX() : rssiStableTimer(MIN_TUNE_TIME), rssiLogTimer(RECEIVER_LAST_DELAY), serialLogTimer(25) {
    this->activeChannel = 0;
    this->rssi = 0;
    this->rssiRaw = 0;
    this->shouldScan = false;
    this->isScanning = false;
    this->scanAutoConnect = false;
    this->scanComplete = false;
    this->scanIndex = 0;
    this->bestRssiIndex = 0;
    this->originalChannelIndex = 0;
    
    memset(this->rssiLast, 0, sizeof(this->rssiLast));
    memset(this->scanRssiData, 0, sizeof(this->scanRssiData));
}

bool VRX::getShouldScan()
{
    return this->shouldScan;
}

bool VRX::getIsScanning()
{
    return this->isScanning;
}

bool VRX::getScanComplete()
{
    return this->scanComplete;
}

uint8_t* VRX::getScanRssiData()
{
    this->scanComplete = false;
    return this->scanRssiData;
}

void VRX::setChannel(uint8_t channel)
{
    BbsProtocol::setVtxChannel(VrxChannels::getSynthRegisterB(channel));

    this->rssiStableTimer.reset();
    this->activeChannel = channel;
}

bool VRX::isRssiStable() {
    return this->rssiStableTimer.hasTicked();
}

void VRX::updateRssi() {
    analogRead(GPIO_PIN_VTX_RSSI); // Fake read to let ADC settle.
    this->rssiRaw = analogRead(GPIO_PIN_VTX_RSSI);

    this->rssi = constrain(
        map(
            this->rssiRaw,
            RSSI_MIN_VAL,
            RSSI_MAX_VAL,
            0,
            100
        ),
        0,
        100
    );

    if (this->rssiLogTimer.hasTicked()) {
        for (uint8_t i = 0; i < RECEIVER_LAST_DATA_SIZE - 1; i++) {
            this->rssiLast[i] = this->rssiLast[i + 1];
        }

        this->rssiLast[RECEIVER_LAST_DATA_SIZE - 1] = this->rssi;
        this->rssiLogTimer.reset();
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
    this->shouldScan = true;
    this->scanAutoConnect = autoConnect;
}

void VRX::startScan()
{
    DBGLN("VRX: Starting scan. AutoConnect: %d", this->scanAutoConnect);
    this->isScanning = true;
    this->scanComplete = false;
    this->shouldScan = false;
    this->scanIndex = 0;
    this->bestRssiIndex = 0;
    this->originalChannelIndex = this->activeChannel;
    memset(this->scanRssiData, 0, sizeof(this->scanRssiData));
    setChannel(VrxChannels::getOrderedIndex(this->scanIndex));
}

void VRX::stopScan()
{
    DBGLN("VRX: Stopping scan");
    this->isScanning = false;
    this->scanAutoConnect = false;
    setChannel(this->originalChannelIndex);
}

void VRX::update() {
    if (this->rssiStableTimer.hasTicked()) {
        updateRssi();
        writeSerialData();
        
        // Handle scan logic during update
        if (this->isScanning) {
            this->scanRssiData[this->scanIndex] = this->rssi;
            if (this->rssi > this->scanRssiData[this->bestRssiIndex]) {
                this->bestRssiIndex = this->scanIndex;
            }

            this->scanIndex = (this->scanIndex + 1) % CHANNELS_SIZE;
            setChannel(VrxChannels::getOrderedIndex(this->scanIndex));

            if (this->scanIndex == 0) {
                if (this->scanAutoConnect) {
                    setChannel(VrxChannels::getOrderedIndex(this->bestRssiIndex));
                } else {
                    setChannel(this->originalChannelIndex);
                }

                this->isScanning = false;
                this->scanAutoConnect = false;
                this->scanComplete = true;
                
                DBGLN("VRX: Best RSSI index: %d", this->bestRssiIndex);
                DBGLN("VRX: Scan complete");
            }
        }
    }
}

void VRX::writeSerialData() {
    if (this->serialLogTimer.hasTicked()) {
        DBGLN("Active channel: %d", this->activeChannel);
        DBGLN("RSSI: %d", this->rssi);
        DBGLN("RSSI raw: %d", this->rssiRaw);
        DBGLN("RSSI last: %d", this->rssiLast[RECEIVER_LAST_DATA_SIZE - 1]);
        this->serialLogTimer.reset();
    }
}
