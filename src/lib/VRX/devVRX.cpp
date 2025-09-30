#include "devVRX.h"
#include "vrx.h"
#include "device.h"

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

    if (vrx->getScanComplete()) {
        uint8_t* scanResults = vrx->getScanRssiData();

        // TODO: constructs and send frame back to sdk
    }

    return DURATION_IMMEDIATELY;
}

void VrxTriggerScan(bool autoConnect)
{
    vrx->triggerScan(autoConnect);
    devicesTriggerEvent();
}

static int event()
{
    if (vrx && vrx->getShouldScan() && !vrx->getIsScanning() && !vrx->getScanComplete())
    {
        vrx->startScan();
    }
    
    return DURATION_IGNORE;
}

device_t VRX_device = {
    .initialize = initialize,
    .start = start,
    .event = event,
    .timeout = timeout
};