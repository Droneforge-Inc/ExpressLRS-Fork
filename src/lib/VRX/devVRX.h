#pragma once

#include "targets.h"
#include "device.h"

#if defined(GPIO_PIN_VTX_BBS_DATA) \
    && defined(GPIO_PIN_VTX_BBS_CS) \
    && defined(GPIO_PIN_VTX_BBS_SCK)
extern device_t VRX_device;
void VrxTriggerScan(bool autoConnect);
void sendVrxTopChannelsFrame(uint8_t* scanResults);
#define HAS_VRX
#endif