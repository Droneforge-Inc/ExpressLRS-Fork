#include <Arduino.h>
#include <cstring>

#include "vrx.h"
#include "bbs_protocol.h"
#include "vrx_channels.h"
#include "targets.h"
#include "logging.h"

#include "vrx_timer.h"


VRX::VRX() : rssiSampleTimer(CHANNEL_SWITCH_DELAY), rssiStableTimer(MIN_TUNE_TIME), rssiLogTimer(RECEIVER_LAST_DELAY), serialLogTimer(1000), diversityHysteresisTimer(DIVERSITY_HYSTERESIS_PERIOD) {
    this->activeChannel = 0;
    this->activeReceiver = VrxReceiver::VRX1;
    this->diversityTargetReceiver = VrxReceiver::VRX1;
    this->rssiValue = 0;
    this->rssiRawValue = 0;
    this->rssiSampleCount = 0;
    this->shouldScan = false;
    this->isScanning = false;
    this->scanAutoConnect = false;
    this->scanComplete = false;
    this->shouldConnect = false;
    this->connectBand = BAND_Unknown;
    this->connectChannel = 0;
    this->scanIndex = 0;
    this->bestRssiIndex = 0;
    this->originalChannelIndex = 0;
    
    memset(this->receiverRssi, 0, sizeof(this->receiverRssi));
    memset(this->receiverRssiRaw, 0, sizeof(this->receiverRssiRaw));
    memset(this->rssiLast, 0, sizeof(this->rssiLast));
    memset(this->scanRssiData, 0, sizeof(this->scanRssiData));
    memset(this->rssiSamples, 0, sizeof(this->rssiSamples));
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

bool VRX::getShouldConnect()
{
    return this->shouldConnect;
}

uint8_t* VRX::getScanRssiData()
{
    this->scanComplete = false;

    if (GPIO_PIN_LED != UNDEF_PIN)
    {
        digitalWrite(GPIO_PIN_LED, LOW);
    }

    return this->scanRssiData;
}

void VRX::setChannel(uint8_t channel)
{
    BbsProtocol::setVrxChannel(VrxChannels::getSynthRegisterB(channel));

    this->rssiStableTimer.reset();
    this->activeChannel = channel;
    this->rssiSampleCount = 0; // Reset sample count when changing channels
}

bool VRX::isRssiStable() {
    return this->rssiStableTimer.hasTicked();
}

bool VRX::useDiversity()
{
    return OPT_USE_VRX_DIVERSITY
        && GPIO_PIN_VRX_RSSI_1 != UNDEF_PIN
        && GPIO_PIN_VRX_RSSI_2 != UNDEF_PIN
        && GPIO_PIN_VID_SW_A0 != UNDEF_PIN
        && GPIO_PIN_VID_SW_A1 != UNDEF_PIN;
}

uint16_t VRX::readRssiRaw(uint8_t receiver)
{
    int pin = receiver == 0 ? GPIO_PIN_VRX_RSSI_1 : GPIO_PIN_VRX_RSSI_2;
    if (pin == UNDEF_PIN)
    {
        return 0;
    }

    analogRead(pin); // Fake read to let ADC settle
    return analogRead(pin);
}

uint8_t VRX::mapRssi(uint16_t raw)
{
    return constrain(
        map(
            raw,
            RSSI_MIN_VAL,
            RSSI_MAX_VAL,
            0,
            255
        ),
        0,
        255
    );
}

void VRX::updateCombinedRssi()
{
    uint8_t receiver = 0;
    if (this->useDiversity() && this->receiverRssi[1] > this->receiverRssi[0])
    {
        receiver = 1;
    }

    this->rssiRawValue = this->receiverRssiRaw[receiver];
    this->rssiValue = this->receiverRssi[receiver];
}

void VRX::logRssi()
{
    if (this->rssiLogTimer.hasTicked()) {
        for (uint8_t i = 0; i < RECEIVER_LAST_DATA_SIZE - 1; i++) {
            this->rssiLast[i] = this->rssiLast[i + 1];
        }

        this->rssiLast[RECEIVER_LAST_DATA_SIZE - 1] = this->rssiValue;
        this->rssiLogTimer.reset();
    }
}

bool VRX::updateRssi(bool useAveraging) {
    if (!useAveraging) {
        uint8_t receiverCount = this->useDiversity() ? VRX_RECEIVER_COUNT : 1;
        for (uint8_t receiver = 0; receiver < receiverCount; receiver++) {
            this->receiverRssiRaw[receiver] = this->readRssiRaw(receiver);
            this->receiverRssi[receiver] = this->mapRssi(this->receiverRssiRaw[receiver]);
        }

        this->updateCombinedRssi();
        this->logRssi();
        return true;
    }
    
    if (rssiSampleCount == 0 && !rssiSampleTimer.hasTicked()) {
        return false;
    }

    uint8_t receiverCount = this->useDiversity() ? VRX_RECEIVER_COUNT : 1;
    for (uint8_t receiver = 0; receiver < receiverCount; receiver++) {
        this->rssiSamples[receiver][this->rssiSampleCount] = this->readRssiRaw(receiver);
    }
    this->rssiSampleCount++;
    
    // Only calculate average when we have all samples
    if (this->rssiSampleCount >= NUM_RSSI_SAMPLES) {
        for (uint8_t receiver = 0; receiver < receiverCount; receiver++) {
            uint32_t rssiSum = 0;
            for (uint8_t i = 0; i < NUM_RSSI_SAMPLES; i++) {
                rssiSum += this->rssiSamples[receiver][i];
            }

            this->receiverRssiRaw[receiver] = rssiSum / NUM_RSSI_SAMPLES;
            this->receiverRssi[receiver] = this->mapRssi(this->receiverRssiRaw[receiver]);
        }

        this->rssiSampleCount = 0; // Reset for next averaging cycle
        this->updateCombinedRssi();
        this->logRssi();
    }

    return true;
}

void VRX::setup() {
  if (GPIO_PIN_VRX_RSSI_1 != UNDEF_PIN)
  {
    pinMode(GPIO_PIN_VRX_RSSI_1, INPUT_PULLUP);
  }
  if (this->useDiversity() && GPIO_PIN_VRX_RSSI_2 != UNDEF_PIN)
  {
    pinMode(GPIO_PIN_VRX_RSSI_2, INPUT_PULLUP);
  }
  if (GPIO_PIN_VRX_BBS_DATA != UNDEF_PIN)
  {
    pinMode(GPIO_PIN_VRX_BBS_DATA, OUTPUT);
    digitalWrite(GPIO_PIN_VRX_BBS_DATA, LOW);
  }
  if (GPIO_PIN_VRX_BBS_CS != UNDEF_PIN)
  {
    pinMode(GPIO_PIN_VRX_BBS_CS, OUTPUT);
    digitalWrite(GPIO_PIN_VRX_BBS_CS, HIGH);
  }
  if (GPIO_PIN_VRX_BBS_SCK != UNDEF_PIN)
  {
    pinMode(GPIO_PIN_VRX_BBS_SCK, OUTPUT);
    digitalWrite(GPIO_PIN_VRX_BBS_SCK, LOW);
  }
  if (this->useDiversity())
  {
    if (GPIO_PIN_VID_SW_EN != UNDEF_PIN)
    {
        pinMode(GPIO_PIN_VID_SW_EN, OUTPUT);
        digitalWrite(GPIO_PIN_VID_SW_EN, HIGH);
    }
    pinMode(GPIO_PIN_VID_SW_A0, OUTPUT);
    pinMode(GPIO_PIN_VID_SW_A1, OUTPUT);
    setActiveReceiver(VrxReceiver::VRX1);
  }

  BbsProtocol::setPowerDownRegister(0b00010000110111110011);
}

void VRX::setActiveReceiver(VrxReceiver receiver)
{
    if (!this->useDiversity())
    {
        this->activeReceiver = VrxReceiver::VRX1;
        return;
    }

    digitalWrite(GPIO_PIN_VID_SW_A0, LOW);
    digitalWrite(GPIO_PIN_VID_SW_A1, receiver == VrxReceiver::VRX2 ? HIGH : LOW);
    this->activeReceiver = receiver;
}

void VRX::updateDiversity()
{
    if (!this->useDiversity() || this->isScanning)
    {
        return;
    }

    int16_t rssiDiff = (int16_t)this->receiverRssi[0] - (int16_t)this->receiverRssi[1];
    uint8_t rssiDiffAbs = rssiDiff < 0 ? -rssiDiff : rssiDiff;
    VrxReceiver bestReceiver = this->activeReceiver;

    if (rssiDiff > 0)
    {
        bestReceiver = VrxReceiver::VRX1;
    }
    else if (rssiDiff < 0)
    {
        bestReceiver = VrxReceiver::VRX2;
    }

    if (rssiDiffAbs < DIVERSITY_HYSTERESIS)
    {
        this->diversityHysteresisTimer.reset();
        return;
    }

    if (bestReceiver != this->diversityTargetReceiver)
    {
        this->diversityTargetReceiver = bestReceiver;
        this->diversityHysteresisTimer.reset();
        return;
    }

    if (this->diversityHysteresisTimer.hasTicked())
    {
        this->setActiveReceiver(bestReceiver);
    }
}

void VRX::triggerScan(bool autoConnect)
{
    this->shouldScan = true;
    this->scanAutoConnect = autoConnect;

    if (GPIO_PIN_LED != UNDEF_PIN)
    {
        digitalWrite(GPIO_PIN_LED, HIGH);
    }
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
    this->rssiSampleTimer.reset();
}

void VRX::stopScan()
{
    DBGLN("VRX: Stopping scan");
    this->isScanning = false;
    this->scanAutoConnect = false;
    setChannel(this->originalChannelIndex);
}

void VRX::triggerConnect(uint8_t band, uint8_t channel)
{
    this->shouldConnect = true;
    this->connectBand = (VrxBand)band;
    this->connectChannel = channel;
}

void VRX::connect() {
    if (this->connectBand == BAND_Unknown) {
        DBGLN("VRX: Invalid band");
        return;
    }

    DBGLN("VRX: Connecting to band: %d, channel: %d", this->connectBand, this->connectChannel);
    uint8_t index = this->connectBand + this->connectChannel - 1;
    setChannel(index);

    this->shouldConnect = false;
    this->connectBand = BAND_Unknown;
    this->connectChannel = 0;
}

void VRX::update() {
    if (this->rssiStableTimer.hasTicked()) {
        auto didUpdate = updateRssi(this->isScanning);
        writeSerialData();
        
        // Handle scan logic during update - only proceed when we have a complete RSSI reading
        if (this->isScanning && this->rssiSampleCount == 0 && didUpdate) { // rssiSampleCount == 0 means we just completed averaging
            this->scanRssiData[this->scanIndex] = this->rssiValue;
            auto num = VrxChannels::getOrderedIndex(this->scanIndex);
            DBGLN("VRX: band: %d, channel: %d, RSSI raw: %d, rssi: %d, freq: %d", (num / 8) + 1, num % 8 + 1, this->rssiRawValue, this->rssiValue, VrxChannels::getFrequency(num));
            if (this->rssiValue > this->scanRssiData[this->bestRssiIndex]) {
                this->bestRssiIndex = this->scanIndex;
            }

            this->scanIndex = (this->scanIndex + 1) % CHANNELS_SIZE;
            
            if (this->scanIndex == 0) {
                // Scan complete
                if (this->scanAutoConnect) {
                    setChannel(VrxChannels::getOrderedIndex(this->bestRssiIndex));
                } else {
                    setChannel(this->originalChannelIndex);
                }

                this->isScanning = false;
                this->scanAutoConnect = false;
                this->scanComplete = true;
                
                auto bestNum = VrxChannels::getOrderedIndex(this->bestRssiIndex);
                DBGLN("VRX: Best band: %d, channel: %d, RSSI: %d, freq: %d", (bestNum / 8) + 1, bestNum % 8 + 1, this->scanRssiData[this->bestRssiIndex], VrxChannels::getFrequency(bestNum));
                DBGLN("VRX: Scan complete");
            } else {
                setChannel(VrxChannels::getOrderedIndex(this->scanIndex));
                this->rssiSampleTimer.reset();
            }
        } else if (didUpdate && this->rssiSampleCount == 0) {
            this->updateDiversity();
        }
    }
}

void VRX::writeSerialData() {
    // if (this->serialLogTimer.hasTicked()) {
    //     DBGLN("Active channel: %d", this->activeChannel);
    //     DBGLN("RSSI: %d", this->rssiValue);
    //     DBGLN("RSSI raw: %d", this->rssiRawValue);
    //     DBGLN("RSSI last: %d", this->rssiLast[RECEIVER_LAST_DATA_SIZE - 1]);
    //     this->serialLogTimer.reset();
    // }
}
