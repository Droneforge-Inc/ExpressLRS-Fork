#pragma once

#include "targets.h"
#include "device.h"

#if defined(TARGET_TX) \
    && defined(GPIO_PIN_VTX_BBS_DATA) \
    && defined(GPIO_PIN_VTX_BBS_CS) \
    && defined(GPIO_PIN_VTX_BBS_SCK)

#define HAS_VRX

extern device_t VRX_device;
void VrxTriggerScan(bool autoConnect);
void VrxConnect(uint8_t band, uint8_t channel);
void sendVrxTopChannelsFrame(uint8_t* scanResults);

#endif