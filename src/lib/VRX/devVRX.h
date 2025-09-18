#pragma once

#include "device.h"

#if defined(GPIO_PIN_BBS_DATA) \
    && defined(GPIO_PIN_BBS_CS) \
    && defined(GPIO_PIN_BBS_SCK)
extern device_t VRX_device;
#define HAS_VRX
#endif