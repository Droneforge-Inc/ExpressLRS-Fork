#ifdef HAS_VRX
#include "devVRX.h"
#include "vrx.h"
#include "device.h"
#include "crsf_protocol.h"
#include "handset.h"
#include "CRSF.h"

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

        // Construct and send CRSF_FRAMETYPE_NIMBUS_VRX_TOP_CHANNELS frame
        sendVrxTopChannelsFrame(scanResults);
    }

    return DURATION_IMMEDIATELY;
}


static int event()
{
    if (vrx && vrx->getShouldScan() && !vrx->getIsScanning() && !vrx->getScanComplete())
    {
        vrx->startScan();
    }

    if (vrx && vrx->getShouldConnect())
    {
        vrx->connect();
    }
    
    return DURATION_IGNORE;
}

device_t VRX_device = {
    .initialize = initialize,
    .start = start,
    .event = event,
    .timeout = timeout
};

void VrxTriggerScan(bool autoConnect)
{
    vrx->triggerScan(autoConnect);
    devicesTriggerEvent();
}

void VrxConnect(uint8_t band, uint8_t channel)
{
    vrx->triggerConnect(band, channel);
    devicesTriggerEvent();
}

void sendVrxTopChannelsFrame(uint8_t* scanResults)
{
    // CRSF frame structure:
    // [0] = device_addr (CRSF_ADDRESS_RADIO_TRANSMITTER)
    // [1] = frame_size (payload + type + crc = CHANNELS_SIZE + 2)
    // [2] = type (CRSF_FRAMETYPE_NIMBUS_VRX_TOP_CHANNELS)
    // [3...50] = payload (48 bytes of RSSI data)
    // [51] = crc
    
    constexpr uint8_t payloadLen = CHANNELS_SIZE; // 48 bytes
    uint8_t buffer[payloadLen + 4]; // +4 for addr, size, type, crc
    
    buffer[0] = CRSF_ADDRESS_RADIO_TRANSMITTER;
    buffer[1] = CRSF_FRAME_SIZE(payloadLen); // payloadLen + 2 (type + crc)
    buffer[2] = CRSF_FRAMETYPE_NIMBUS_VRX_TOP_CHANNELS;
    
    // Copy RSSI scan results as payload
    memcpy(&buffer[3], scanResults, payloadLen);
    
    // Calculate CRC over type and payload
    buffer[payloadLen + 3] = crsf_crc.calc(&buffer[2], payloadLen + 1);
    
    // Send the frame
    handset->sendTelemetryToTX(buffer);
}
#endif