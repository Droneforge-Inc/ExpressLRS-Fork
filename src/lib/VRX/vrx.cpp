#include <Arduino.h>

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

static VrxTimer rssiStableTimer = VrxTimer(MIN_TUNE_TIME);
static VrxTimer rssiLogTimer = VrxTimer(RECEIVER_LAST_DELAY);
static VrxTimer serialLogTimer = VrxTimer(25);

void VRX::setChannel(uint8_t channel)
{
    BbsProtocol::setVtxChannel(VrxChannels::getSynthRegisterB(channel));

    rssiStableTimer.reset();
    activeChannel = channel;
}

bool VRX::isRssiStable() {
    return rssiStableTimer.hasTicked();
}

uint16_t VRX::updateRssi() {
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
    pinMode(GPIO_PIN_VTX_RSSI, INPUT);
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

// #ifdef DISABLE_AUDIO
//     ReceiverSpi::setPowerDownRegister(0b00010000110111110011);
// #endif
}

void VRX::update() {
    if (rssiStableTimer.hasTicked()) {
        updateRssi();
        writeSerialData();
    }
}

static void writeSerialData() {
    if (VRX::serialLogTimer.hasTicked()) {
        DBGLN("Active channel: %d", VRX::activeChannel);
        DBGLN("RSSI: %d", VRX::rssi);
        DBGLN("RSSI raw: %d", VRX::rssiRaw);
        DBGLN("RSSI last: %d", VRX::rssiLast);
        VRX::serialLogTimer.reset();
    }
}
