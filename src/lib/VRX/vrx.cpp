#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include <sys/types.h>

#include "vrx.h"
#include "bbs_protocol.h"
#include "vrx_channels.h"
#include "targets.h"
#include "logging.h"

#include "vrx_timer.h"


VRX::VRX() : rssiSampleTimer(CHANNEL_SWITCH_DELAY), rssiStableTimer(MIN_TUNE_TIME), rssiLogTimer(RECEIVER_LAST_DELAY), serialLogTimer(1000) {
    this->activeChannel = 0;
    this->rssi = 0;
    this->rssiRaw = 0;
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
    BbsProtocol::setVtxChannel(VrxChannels::getSynthRegisterB(channel));

    this->rssiStableTimer.reset();
    this->activeChannel = channel;
    this->rssiSampleCount = 0; // Reset sample count when changing channels
}

bool VRX::isRssiStable() {
    return this->rssiStableTimer.hasTicked();
}

bool VRX::updateRssi(bool useAveraging) {
    if (!useAveraging) {
        // Single reading mode
        analogRead(GPIO_PIN_VTX_RSSI); // Fake read to let ADC settle
        this->rssiRaw = analogRead(GPIO_PIN_VTX_RSSI);

        this->rssi = constrain(
            map(
                this->rssiRaw,
                RSSI_MIN_VAL,
                RSSI_MAX_VAL,
                0,
                255
            ),
            0,
            255
        );

        if (this->rssiLogTimer.hasTicked()) {
            for (uint8_t i = 0; i < RECEIVER_LAST_DATA_SIZE - 1; i++) {
                this->rssiLast[i] = this->rssiLast[i + 1];
            }

            this->rssiLast[RECEIVER_LAST_DATA_SIZE - 1] = this->rssi;
            this->rssiLogTimer.reset();
        }

        return true;
    }

    if (rssiSampleCount == 0 && !rssiSampleTimer.hasTicked()) {
        return false;
    }

    analogRead(GPIO_PIN_VTX_RSSI); // Fake read to let ADC settle on first sample

    // Store the current sample
    this->rssiSamples[this->rssiSampleCount] = analogRead(GPIO_PIN_VTX_RSSI);
    this->rssiSampleCount++;

    // Only calculate average when we have all samples
    if (this->rssiSampleCount >= NUM_RSSI_SAMPLES) {
        uint32_t rssiSum = 0;
        for (uint8_t i = 0; i < NUM_RSSI_SAMPLES; i++) {
            rssiSum += this->rssiSamples[i];
        }

        // Calculate average
        this->rssiRaw = rssiSum / NUM_RSSI_SAMPLES;
        this->rssiSampleCount = 0; // Reset for next averaging cycle

        this->rssi = constrain(
            map(
                this->rssiRaw,
                RSSI_MIN_VAL,
                RSSI_MAX_VAL,
                0,
                255
            ),
            0,
            255
        );

        if (this->rssiLogTimer.hasTicked()) {
            for (uint8_t i = 0; i < RECEIVER_LAST_DATA_SIZE - 1; i++) {
                this->rssiLast[i] = this->rssiLast[i + 1];
            }

            this->rssiLast[RECEIVER_LAST_DATA_SIZE - 1] = this->rssi;
            this->rssiLogTimer.reset();
        }
    }

    return true;
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

bool VRX::setFrequency(uint16_t frequencyMHz)
{
    if (frequencyMHz < 5000 || frequencyMHz > 6000)
        return false;

    // The U suffixed constants are unsigned integers assigned to specific synthesizer actions
    // 2U: The synthesizer tunes to 2MHz steps.
    // 32U: Splits the synthesizer divider into it's N and A components (217 and 218)
    // 7U: Shifts N left seven bits because A occupies the low seven bits of the Synth Register B
    // 479U: The receiver's intermediate frequency offset used by the existing RX5808 conversion

    const uint16_t flo = (frequencyMHz - 479U) / 2U;
    const uint16_t n = flo / 32U;
    const uint16_t a = flo % 32U;
    const uint16_t synthRegisterB = (n << 7U) | a;

    BbsProtocol::setVtxChannel(synthRegisterB);
    rssiStableTimer.reset();
    rssiSampleCount = 0;
    return true;
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
            this->scanRssiData[this->scanIndex] = this->rssi;
            auto num = VrxChannels::getOrderedIndex(this->scanIndex);
            DBGLN("VRX: band: %d, channel: %d, RSSI raw: %d, rssi: %d, freq: %d", (num / 8) + 1, num % 8 + 1, this->rssiRaw, this->rssi, VrxChannels::getFrequency(num));
            if (this->rssi > this->scanRssiData[this->bestRssiIndex]) {
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
        }
    }
}

void VRX::writeSerialData() {
    // if (this->serialLogTimer.hasTicked()) {
    //     DBGLN("Active channel: %d", this->activeChannel);
    //     DBGLN("RSSI: %d", this->rssi);
    //     DBGLN("RSSI raw: %d", this->rssiRaw);
    //     DBGLN("RSSI last: %d", this->rssiLast[RECEIVER_LAST_DATA_SIZE - 1]);
    //     this->serialLogTimer.reset();
    // }
}
