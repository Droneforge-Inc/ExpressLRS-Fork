#include "devVRX.h"
#include "vrx.h"

VRX *vrx;

static void initialize()
{
    vrx = new VRX();
}

static int start()
{
    vrx->setup();
    vrx->setChannel(0);
    return DURATION_IMMEDIATELY;
}

static int timeout()
{
    vrx->update();
    return DURATION_IMMEDIATELY;
}

static int event()
{
    return DURATION_IGNORE;
}

device_t VRX_device = {
    .initialize = initialize,
    .start = start,
    .event = event,
    .timeout = timeout
};