#include "firmware.h"
#include "Df3ReferenceSession.h"
#include "FHSS.h"
#include "OTA.h"
#include "common.h"
#include "options.h"
#include "stubborn_receiver.h"
#include "stubborn_sender.h"
#include "targets.h"
#include "telemetry_protocol.h"
#include <algorithm>
#include <iterator>
#include <string>
#if TARGET_TX
#include "tests/fixtures/LegacyReferenceQueue.h"
#else
#include "rx-serial/SerialCRSF.h"
#include "telemetry.h"
#endif

static bool streamEnabled = false;
#if TARGET_TX
static sitl::LegacyReferenceQueue legacyReferences;
static dfstream::Sender streamSender;
static dfstream::TxSession streamSession;
static dfstream::Frame streamPending;
static uint32_t streamPendingAt = 0;
static bool streamHavePending = false;
static uint32_t streamNonce = 0xdf330000;
#else
static dfstream::Receiver streamReceiver;
static dfstream::RxSession streamSession;
static uint32_t streamLastReference = 0, streamLastHeartbeat = 0;
static bool streamAckEnabled = false;
static bool streamComplete = false;
static uint16_t streamSequence = 0;
#endif
#if TARGET_TX
// Native logging uses the existing no-op stream in all optimization modes.
Stream *TxBackpack = &Serial;
static StubbornReceiver telemetryReceiver;
static StubbornSender mspSender;
static uint8_t telemetryBuffer[64]{};
#else
static StubbornSender telemetrySender;
static StubbornReceiver mspReceiver;
static uint8_t mspBuffer[ELRS_MSP_BUFFER]{};
static uint8_t telemetryBuffer[64]{};
#endif
uint64_t sitl_time_us = 0;
firmware_options_t firmwareOptions{};
extern const char device_name[] = "DF ELRS SITL";
extern const char version[] = "3.0.0 SITL";
extern const char hardware_version[] = "0.0.0 host";
#if TARGET_RX
Telemetry telemetry;
// Unsupported hardware-management requests fail explicitly, never touch hardware.
void reset_into_bootloader()
{
    throw std::runtime_error("bootloader unsupported in SITL");
}
void EnterBindingModeSafely()
{
    throw std::runtime_error("binding unsupported in pre-synchronized SITL");
}
void UpdateModelMatch(uint8_t)
{
    throw std::runtime_error("model-match changes unsupported in SITL");
}
class CaptureStream : public Stream
{
public:
    sitl::Bytes bytes;
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    size_t write(uint8_t b) override
    {
        bytes.push_back(b);
        return 1;
    }
    size_t write(const uint8_t *p, size_t n) override
    {
        bytes.insert(bytes.end(), p, p + n);
        return n;
    }
};
static CaptureStream serial;
static SerialCRSF crsf(serial, serial);
#endif
namespace sitl
{
uint32_t Firmware::frequency(uint64_t slot) const
{
    auto index = (slot / ExpressLRS_currAirRate_Modparams->FHSShopInterval) % FHSSgetSequenceCount();
    return FHSSconfig->freq_start + FHSSsequence[index] * freq_spread / FREQ_SPREAD_SCALE;
}
Bytes Firmware::drain()
{
    Bytes out;
    size_t count = 0;
    for (const auto &b : uart)
    {
        if (b.time > now || count == 400)
            break;
        ++count;
    }
    put(out, count, 2);
    for (size_t i = 0; i < count; ++i)
    {
        auto b = uart.front();
        uart.pop_front();
        put(out, b.time, 8);
        put(out, b.value, 1);
    }
    return out;
}
void Firmware::serialize(const Bytes &bytes)
{
    if (bytes.empty())
        return;
    const uint64_t start = std::max(now, uartEnd);
    for (size_t i = 0; i < bytes.size(); ++i)
        uart.push_back({start + ((i + 1) * 10000000ULL + baud - 1) / baud, bytes[i]});
    uartEnd = uart.back().time;
}
Bytes Firmware::handle(uint16_t op, uint64_t time, const Bytes &payload)
{
    if (time < now || time > UINT64_MAX / 2)
        throw std::runtime_error("time must be monotonic and <= 2^63-1 us");
    Reader r{payload};
    Bytes out;
    if (op == Hello)
    {
        r.end();
        std::string text = "ELRS protocol SITL v1; role=";
#if TARGET_TX
        text += "tx";
#else
        text += "rx";
#endif
        text += "; revision=" SITL_REVISION "; downlink-v1; uplink-v1; pre-synchronized; no RF PHY or embedded main loop";
        return Bytes(text.begin(), text.end());
    }
    if (op == Configure)
    {
        if (configured || time != 0)
            throw std::runtime_error("configure once at time zero; restart for a clean reset");
        auto uid = r.take(6);
        auto rate = r.get(1);
        auto mode = r.get(1);
        auto b = r.get(4);
        r.end();
        if (rate >= RATE_MAX || mode > 2 || b < 9600 || b > 3000000)
            throw std::runtime_error("invalid configuration");
        auto mod = get_elrs_airRateConfig(rate);
        if (mod->numOfSends != 1 || (mod->PayloadLength == 8 && mode == 2))
            throw std::runtime_error("DVDA and 8-byte 12ch mode are outside this proof");
        std::copy(uid.begin(), uid.end(), UID);
        baud = b;
        ExpressLRS_currAirRate_Modparams = mod;
        ExpressLRS_currAirRate_RFperfParams = get_elrs_RFperfParams(rate);
        // RC-only proof: no reserved telemetry/sync slots. This is NOT the complete link scheduler.
        ExpressLRS_currTlmDenom = 1;
        OtaUpdateCrcInitFromUid();
        OtaUpdateSerializers(static_cast<OtaSwitchMode_e>(mode), mod->PayloadLength);
        FHSSrandomiseFHSSsequence(uidMacSeedGet());
        std::fill(std::begin(ChannelData), std::end(ChannelData), CRSF_CHANNEL_VALUE_MID);
        ChannelData[4] = CRSF_CHANNEL_VALUE_1000;
        CRSF::LinkStatistics.uplink_TX_Power = 1;
        CRSF::LinkStatistics.uplink_RSSI_1 = 60;
        CRSF::LinkStatistics.uplink_Link_quality = 100;
        configured = true;
        put(out, mod->interval, 4);
        put(out, ExpressLRS_currAirRate_RFperfParams->TOA, 4);
        put(out, mod->PayloadLength, 1);
        put(out, mod->FHSShopInterval, 1);
        return out;
    }
    if (op == ReferenceStatus)
    {
        r.end();
#if TARGET_TX
        put(out, streamSession.state, 1);
        put(out, streamSession.ready(), 1);
        put(out, streamSession.session, 4);
        put(out, streamSession.epoch, 2);
        // Append the actual production USB status frame for host SDK adapters.
        // Keep the existing eight-byte session prefix for older coordinators.
        const auto status = dfstream::localStatus(streamSession, streamEnabled);
        out.insert(out.end(), status.begin(), status.end());
#else
        put(out, streamSession.active, 1);
        put(out, streamComplete, 1);
        put(out, streamSession.session, 4);
        put(out, streamSession.epoch, 2);
#endif
        return out;
    }
    if (op == ReferenceReset)
    {
        r.end();
        streamSession.reset();
#if TARGET_TX
        streamSender.reset(0);
        streamHavePending = false;
#else
        streamReceiver.reset(0);
        streamComplete = false;
        streamAckEnabled = false;
        streamSequence = 0;
        streamLastReference = time;
        streamLastHeartbeat = uint32_t(time) - dfstream::kHeartbeatPeriodUs;
#endif
        now = sitl_time_us = time;
        return out;
    }
    if (op == Quit)
    {
        r.end();
        return out;
    }
    if (!configured)
        throw std::runtime_error("configure first");
    if (op == EnableDownlink)
    {
        // Empty payload retains the original 1:2 experiment for old clients.
        const auto denominator = payload.empty() ? 2 : r.get(1);
        r.end();
        if (denominator < 2 || denominator > 128 || (denominator & (denominator - 1)))
            throw std::runtime_error("telemetry denominator must be 2, 4, 8, 16, 32, 64 or 128");
        if (time != 0 || haveSlot || downlink || ExpressLRS_currAirRate_Modparams->PayloadLength != 8)
            throw std::runtime_error("enable downlink once before slots; 8-byte OTA only");
        // Explicit protocol experiment: reserve every Nth slot for telemetry.
        // Real ACK bits, fragmentation and CRC; link-statistics ACKs are added by EnableUplink; no sync acquisition.
        // Production telemetry slots have nonce % denominator == 0. Start
        // this pre-synchronized experiment at nonce 1, keeping slot zero RC.
        // Reserving nonce N-1 instead starves HybridWide's ACK at ratio 1:8.
        downlink = true;
        ExpressLRS_currTlmDenom = denominator;
#if TARGET_TX
        telemetryReceiver.setMaxPackageIndex(ELRS4_TELEMETRY_MAX_PACKAGES);
        telemetryReceiver.SetDataToReceive(telemetryBuffer, sizeof(telemetryBuffer));
#else
        telemetrySender.setMaxPackageIndex(ELRS4_TELEMETRY_MAX_PACKAGES);
        telemetrySender.UpdateTelemetryRate(1000000 / ExpressLRS_currAirRate_Modparams->interval, denominator, 1);
#endif
        return out;
    }
    if (op == EnableUplink)
    {
        // Validate the whole request before changing callbacks, queues or mode.
        const auto version = payload.empty() ? 0 : r.get(1);
        r.end();
        if (!payload.empty() && version != dfstream::kTransportVersion)
            throw std::runtime_error("unsupported reference transport version");
        if (time != 0 || haveSlot || !downlink || uplink)
            throw std::runtime_error("enable uplink once after downlink, before slots");
        if (version && (ExpressLRS_currAirRate_Modparams->index != dfstream::kRateIndex || ExpressLRS_currTlmDenom != dfstream::kTelemetryRatio || OtaSwitchModeCurrent != smWideOr8ch))
            throw std::runtime_error("reference v2 requires rate 10, ratio 2, wide mode");
        streamEnabled = version != 0;
#if TARGET_TX
        CRSF::ReferenceHandler = [](const uint8_t *p, uint8_t n) {
            if (!streamEnabled)
            {
                legacyReferences.offer(p, n);
                return;
            }
            // QueueUplink has already validated the complete reference.
            std::copy_n(p, streamPending.size(), streamPending.begin());
            streamPendingAt = sitl_time_us;
            streamHavePending = true;
        };
#endif
        uplink = true;
        nextLinkStats = true;
        telemetryBurstMax = TLMBurstMaxForRateRatio(1000000 / ExpressLRS_currAirRate_Modparams->interval, ExpressLRS_currTlmDenom);
#if TARGET_TX
        CRSF::ResetMspQueue();
        mspSender.setMaxPackageIndex(ELRS_MSP_MAX_PACKAGES);
#else
        mspReceiver.setMaxPackageIndex(ELRS_MSP_MAX_PACKAGES);
        mspReceiver.SetDataToReceive(mspBuffer, sizeof(mspBuffer));
        telemetrySender.UpdateTelemetryRate(1000000 / ExpressLRS_currAirRate_Modparams->interval, ExpressLRS_currTlmDenom, telemetryBurstMax);
#endif
        return out;
    }
#if TARGET_TX
    if (op == QueueUplink)
    {
        if (!uplink)
            throw std::runtime_error("enable uplink first");
        if (payload.size() < 6 || payload.size() > 64 || payload[1] + 2U != payload.size() ||
            payload[2] < CRSF_FRAMETYPE_DEVICE_PING || (payload[3] != CRSF_ADDRESS_FLIGHT_CONTROLLER && payload[3] != CRSF_ADDRESS_CRSF_RECEIVER) ||
            crsf_crc.calc(payload.data() + 2, payload.size() - 3) != payload.back())
            throw std::runtime_error("invalid FC-addressed extended CRSF uplink frame");
        if (streamEnabled && payload[2] == CRSF_FRAMETYPE_DF_REFERENCE)
        {
            dfstream::Frame frame;
            if (payload.size() != frame.size())
                throw std::runtime_error("invalid D5 size");
            std::copy(payload.begin(), payload.end(), frame.begin());
            if (!dfstream::valid(frame))
                throw std::runtime_error("invalid D5 reference");
        }
        now = sitl_time_us = time;
        auto copy = payload;
        CRSF::AddMspMessage(copy.size(), copy.data());
        return out;
    }
    if (op == Transmit)
    {
        uint32_t channels[16];
        for (auto &c : channels)
        {
            c = r.get(2);
            if (c > 1984)
                throw std::runtime_error("channel outside 0..1984");
        }
        r.end();
        uint64_t interval = ExpressLRS_currAirRate_Modparams->interval;
        if (time % interval)
            throw std::runtime_error("TX time must be on the configured RF slot grid");
        uint64_t slot = time / interval;
        if (downlink && (slot + 1) % ExpressLRS_currTlmDenom == 0)
            throw std::runtime_error("slot reserved for telemetry");
        if (haveSlot && slot <= lastSlot)
            throw std::runtime_error("RF slot already transmitted");
        now = sitl_time_us = time;
        if (streamEnabled)
        {
            if (streamHavePending && uint32_t(time - streamPendingAt) > dfstream::kStartDeadlineUs)
                streamHavePending = false;
            const bool armed = channels[4] >= CRSF_CHANNEL_VALUE_MID;
            streamSession.tick(uint32_t(time), true, armed);
            if (streamHavePending)
            {
                const auto epoch = dfstream::referenceEpoch(streamPending);
                if (streamSession.epoch && epoch != streamSession.epoch)
                    streamSession.reset();
                if (!armed && dfstream::referenceInactive(streamPending) && (streamSession.state == dfstream::Disabled || streamSession.state == dfstream::Failed))
                    streamSession.begin(++streamNonce, epoch, uint32_t(time));
            }
            if (streamSession.state != dfstream::Streaming)
                streamSender.reset(0);
            else
            {
                if (streamSender.session != streamSession.session)
                    streamSender.reset(streamSession.session);
                if (streamHavePending && dfstream::referenceEpoch(streamPending) == streamSession.epoch)
                    streamSender.offer(streamPending, streamPendingAt);
            }
            streamHavePending = false;
            uint8_t *queued = nullptr, length = 0;
            CRSF::GetMspMessage(&queued, &length);
            dfstream::Control c;
            if (!armed && !length && !mspSender.IsActive() && streamSession.nextControl(uint32_t(time), c))
                CRSF::AddMspMessage(c.size(), c.data());
        }
        if (uplink && !mspSender.IsActive())
        {
            if (mspMessageActive)
            {
                CRSF::UnlockMspMessage();
                mspMessageActive = false;
            }
            uint8_t *data = nullptr, length = 0;
            CRSF::GetMspMessage(&data, &length);
            const bool legacy = !streamEnabled && legacyReferences.take(data != nullptr, data, length);
            if (data)
            {
                mspSender.SetDataToTransmit(data, length);
                mspMessageActive = !legacy;
            }
        }
        OtaNonce = (slot + (downlink ? 1 : 0)) % 256;
        OTA_Packet_s packet{};
        uint8_t h = 0;
        const bool streaming = streamEnabled && streamSession.state == dfstream::Streaming;
        const bool dataSlot = streaming ? dfstream::dataSlot(OtaNonce) : nextMsp;
        if (streaming && dataSlot && streamSender.next(uint32_t(time), telemetryReceiver.GetCurrentConfirm(), h, packet.std.msp_ul.payload))
        {
            packet.std.type = PACKET_TYPE_MSPDATA;
            packet.std.msp_ul.packageIndex = h & ~dfstream::kTelemetryAck;
            packet.std.msp_ul.tlmFlag = (h & dfstream::kTelemetryAck) != 0;
        }
        else if (uplink && dataSlot && mspSender.IsActive())
        {
            packet.std.type = PACKET_TYPE_MSPDATA;
            packet.std.msp_ul.packageIndex = mspSender.GetCurrentPayload(packet.std.msp_ul.payload, sizeof(packet.std.msp_ul.payload));
            if (streaming)
                packet.std.msp_ul.tlmFlag = telemetryReceiver.GetCurrentConfirm();
            nextMsp = false;
        }
        else
        {
            OtaPackChannelData(&packet, channels, downlink && telemetryReceiver.GetCurrentConfirm(), ExpressLRS_currTlmDenom);
            nextMsp = true;
        }
        OtaGeneratePacketCrc(&packet);
        now = sitl_time_us = time;
        lastSlot = slot;
        haveSlot = true;
        put(out, slot, 8);
        put(out, frequency(slot), 4);
        put(out, ExpressLRS_currAirRate_Modparams->PayloadLength, 1);
        auto bytes = reinterpret_cast<const uint8_t *>(&packet);
        out.insert(out.end(), bytes, bytes + ExpressLRS_currAirRate_Modparams->PayloadLength);
        return out;
    }
    if (op == ReceiveTelemetry)
    {
        if (!downlink)
            throw std::runtime_error("enable downlink first");
        auto slot = r.get(8);
        auto freq = r.get(4);
        auto len = r.get(1);
        auto bytes = r.take(len);
        r.end();
        uint64_t interval = ExpressLRS_currAirRate_Modparams->interval;
        if (len != 8 || slot % ExpressLRS_currTlmDenom != ExpressLRS_currTlmDenom - 1U || slot > UINT64_MAX / interval - 1)
            throw std::runtime_error("invalid telemetry OTA length/slot");
        if (time < slot * interval + ExpressLRS_currAirRate_RFperfParams->TOA)
            throw std::runtime_error("telemetry RX cannot precede airtime");
        if (haveTelemetrySlot && slot <= lastTelemetrySlot)
            throw std::runtime_error("duplicate telemetry slot");
        OTA_Packet_s packet{};
        std::copy(bytes.begin(), bytes.end(), reinterpret_cast<uint8_t *>(&packet));
        OtaNonce = (slot + (downlink ? 1 : 0)) % 256;
        bool accepted = freq == frequency(slot) && packet.std.type == PACKET_TYPE_TLM &&
                        OtaValidatePacketCrc(&packet) && (packet.std.tlm_dl.type == ELRS_TELEMETRY_TYPE_DATA || (uplink && packet.std.tlm_dl.type == ELRS_TELEMETRY_TYPE_LINK));
        now = sitl_time_us = time;
        lastTelemetrySlot = slot;
        haveTelemetrySlot = true;
        put(out, accepted, 1);
        if (accepted && packet.std.tlm_dl.type == ELRS_TELEMETRY_TYPE_LINK)
        {
            mspSender.ConfirmCurrentPayload(packet.std.tlm_dl.ul_link_stats.stats.mspConfirm);
            return out;
        }
        if (accepted)
        {
            telemetryReceiver.ReceiveData(packet.std.tlm_dl.packageIndex,
                                          packet.std.tlm_dl.payload, sizeof(packet.std.tlm_dl.payload));
            if (telemetryReceiver.HasFinishedData())
            {
                const size_t length = telemetryBuffer[1] + 2U;
                if (length < 4 || length > sizeof(telemetryBuffer) ||
                    crsf_crc.calc(telemetryBuffer + 2, length - 3) != telemetryBuffer[length - 1])
                    throw std::runtime_error("invalid reassembled CRSF telemetry");
                if (!streamEnabled || !streamSession.receive(telemetryBuffer, length, uint32_t(time)))
                    out.insert(out.end(), telemetryBuffer, telemetryBuffer + length);
                telemetryReceiver.Unlock();
            }
        }
        return out;
    }
#else
    if (op == Receive)
    {
        auto slot = r.get(8);
        auto freq = r.get(4);
        auto len = r.get(1);
        auto bytes = r.take(len);
        r.end();
        uint64_t interval = ExpressLRS_currAirRate_Modparams->interval;
        if (len != ExpressLRS_currAirRate_Modparams->PayloadLength || slot > UINT64_MAX / interval - 1)
            throw std::runtime_error("invalid OTA length/slot");
        if (time < slot * interval + ExpressLRS_currAirRate_RFperfParams->TOA)
            throw std::runtime_error("RX cannot precede completion of radio airtime");
        if (haveSlot && slot <= lastSlot)
            throw std::runtime_error("duplicate/out-of-order radio slot");
        if (downlink && (slot + 1) % ExpressLRS_currTlmDenom == 0)
            throw std::runtime_error("slot reserved for telemetry");
        if (uart.size() > 4000)
            throw std::runtime_error("UART queue full; advance and drain before receiving more");
        OTA_Packet_s packet{};
        std::copy(bytes.begin(), bytes.end(), reinterpret_cast<uint8_t *>(&packet));
        OtaNonce = (slot + (downlink ? 1 : 0)) % 256;
        bool accepted = freq == frequency(slot) && OtaValidatePacketCrc(&packet) &&
                        (packet.std.type == PACKET_TYPE_RCDATA || (uplink && packet.std.type == PACKET_TYPE_MSPDATA));
        now = sitl_time_us = time;
        lastSlot = slot;
        haveSlot = true;
        if (accepted)
        {
            serial.bytes.clear();
            if (packet.std.type == PACKET_TYPE_RCDATA)
            {
                bool confirm = OtaUnpackChannelData(&packet, ChannelData, ExpressLRS_currTlmDenom);
                if (downlink)
                    telemetrySender.ConfirmCurrentPayload(confirm);
                crsf.sendRCFrame(true, false, ChannelData);
            }
            else if (streamEnabled && (packet.std.msp_ul.packageIndex & dfstream::kStreamMarker))
            {
                if (streamSession.active)
                {
                    streamAckEnabled = true;
                    telemetrySender.ConfirmCurrentPayload(packet.std.msp_ul.tlmFlag);
                    dfstream::Frame f;
                    if (streamReceiver.push(packet.std.msp_ul.packageIndex, packet.std.msp_ul.payload, uint32_t(time), f) && dfstream::referenceEpoch(f) == streamSession.epoch)
                    {
                        crsf.queueMSPFrameTransmission(f.data());
                        streamLastReference = time;
                        streamSequence = dfstream::referenceSequence(f);
                        if (!streamComplete)
                            streamLastHeartbeat = uint32_t(time) - dfstream::kHeartbeatPeriodUs;
                        streamComplete = true;
                    }
                }
            }
            else
            {
                if (streamEnabled && streamAckEnabled)
                    telemetrySender.ConfirmCurrentPayload(packet.std.msp_ul.tlmFlag);
                const bool before = mspReceiver.GetCurrentConfirm();
                mspReceiver.ReceiveData(packet.std.msp_ul.packageIndex & ELRS4_TELEMETRY_MAX_PACKAGES,
                                        packet.std.msp_ul.payload, sizeof(packet.std.msp_ul.payload));
                if (before != mspReceiver.GetCurrentConfirm())
                    nextLinkStats = true;
                if (mspReceiver.HasFinishedData())
                {
                    const size_t length = mspBuffer[1] + 2U;
                    if (length < 6 || length > 64 || (mspBuffer[3] != CRSF_ADDRESS_FLIGHT_CONTROLLER && mspBuffer[3] != CRSF_ADDRESS_CRSF_RECEIVER) ||
                        crsf_crc.calc(mspBuffer + 2, length - 3) != mspBuffer[length - 1])
                        throw std::runtime_error("invalid reassembled CRSF uplink");
                    if (streamEnabled && dfstream::isControl(mspBuffer, length))
                    {
                        dfstream::Control reply;
                        const uint32_t old = streamSession.session;
                        const uint16_t oldEpoch = streamSession.epoch;
                        if (streamSession.receive(mspBuffer, length, uint32_t(time), true, ChannelData[4] >= CRSF_CHANNEL_VALUE_MID, reply))
                        {
                            if (old != streamSession.session || oldEpoch != streamSession.epoch)
                            {
                                streamReceiver.reset(streamSession.session);
                                streamComplete = false;
                                streamAckEnabled = false;
                                streamSequence = 0;
                                streamLastReference = time;
                            }
                            telemetry.AppendTelemetryPackage(reply.data());
                        }
                    }
                    else
                        crsf.queueMSPFrameTransmission(mspBuffer);
                    mspReceiver.Unlock();
                }
            }
            crsf.sendQueuedData(128);
            serialize(serial.bytes);
        }
        put(out, accepted, 1);
        return out;
    }
    if (op == Advance)
    {
        r.end();
        now = sitl_time_us = time;
        return drain();
    }
    if (op == TransmitTelemetry)
    {
        r.end();
        if (!downlink)
            throw std::runtime_error("enable downlink first");
        const uint64_t interval = ExpressLRS_currAirRate_Modparams->interval;
        const uint64_t slot = time / interval;
        if (time % interval || (slot + 1) % ExpressLRS_currTlmDenom != 0)
            throw std::runtime_error("telemetry TX requires reserved RF slot grid");
        if (haveTelemetrySlot && slot <= lastTelemetrySlot)
            throw std::runtime_error("duplicate telemetry slot");
        if (streamEnabled)
        {
            if (streamSession.active && uint32_t(time - streamLastReference) > dfstream::kReferenceTimeoutUs)
            {
                streamSession.reset();
                streamReceiver.reset(0);
                streamComplete = false;
                streamAckEnabled = false;
                streamSequence = 0;
            }
            if (uint32_t(time - streamLastHeartbeat) >= dfstream::kHeartbeatPeriodUs)
            {
                auto hb = dfstream::heartbeat(streamSession.active ? streamSession.session : 0, streamSession.active ? streamSession.epoch : 0, streamSequence, streamSession.active && streamComplete);
                telemetry.AppendTelemetryPackage(hb.data());
                streamLastHeartbeat = time;
            }
        }
        if (!telemetrySender.IsActive())
        {
            uint8_t length = 0;
            if (telemetry.GetNextPayload(&length, telemetryBuffer))
                telemetrySender.SetDataToTransmit(telemetryBuffer, length);
        }
        now = sitl_time_us = time;
        lastTelemetrySlot = slot;
        haveTelemetrySlot = true;
        if (!telemetrySender.IsActive() && !uplink)
            return out;
        OtaNonce = (slot + (downlink ? 1 : 0)) % 256;
        OTA_Packet_s packet{};
        packet.std.type = PACKET_TYPE_TLM;
        if (uplink && (nextLinkStats || !telemetrySender.IsActive()))
        {
            packet.std.tlm_dl.type = ELRS_TELEMETRY_TYPE_LINK;
            auto &stats = packet.std.tlm_dl.ul_link_stats.stats;
            stats.uplink_RSSI_1 = 60;
            stats.modelMatch = 1;
            stats.lq = 100;
            stats.mspConfirm = mspReceiver.GetCurrentConfirm();
            nextLinkStats = false;
            telemetryBurstCount = 1;
        }
        else
        {
            packet.std.tlm_dl.type = ELRS_TELEMETRY_TYPE_DATA;
            packet.std.tlm_dl.packageIndex = telemetrySender.GetCurrentPayload(
                packet.std.tlm_dl.payload, sizeof(packet.std.tlm_dl.payload));
            if (uplink)
            {
                if (telemetryBurstCount < telemetryBurstMax)
                    ++telemetryBurstCount;
                else
                    nextLinkStats = true;
            }
        }
        OtaGeneratePacketCrc(&packet);
        put(out, slot, 8);
        put(out, frequency(slot), 4);
        put(out, 8, 1);
        auto bytes = reinterpret_cast<const uint8_t *>(&packet);
        out.insert(out.end(), bytes, bytes + 8);
        return out;
    }
    if (op == TelemetryIn || op == QueueTelemetry)
    {
        if (op == QueueTelemetry && !downlink)
            throw std::runtime_error("enable downlink first");
        if (op == TelemetryIn && downlink)
            throw std::runtime_error("use QueueTelemetry with downlink");
        // Complete bounded CRSF frames only; validate before passing to firmware parser.
        if (payload.size() < 4 || payload.size() > 64 || payload[1] + 2U != payload.size() ||
            crsf_crc.calc(payload.data() + 2, payload.size() - 3) != payload.back())
            throw std::runtime_error("invalid CRSF telemetry frame");
        // Bound admission before the production queue can evict older frames.
        // The coordinator must explicitly account for rejected admission.
        if (op == QueueTelemetry && telemetry.GetFifoFullPct() > 75)
            throw std::runtime_error("telemetry queue backpressure");
        now = sitl_time_us = time;
        for (auto b : payload)
            telemetry.RXhandleUARTin(b);
        if (op == QueueTelemetry)
            return out;
        uint8_t len = 0;
        uint8_t data[64]{};
        if (telemetry.GetNextPayload(&len, data))
            out.assign(data, data + len);
        return out; // Parsed FC telemetry, not yet transmitted across an RF downlink.
    }
#endif
    throw std::runtime_error("unsupported operation for this role");
}
} // namespace sitl
