// Lossless DF3 datagrams. Callers serialize access between the main loop and RF ISR.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
namespace dfstream
{
// Fixed v3 radio profile. All piloting permissions share the reference's CRC,
// sequence and lease; no independent RC packets consume streaming uplink slots.
constexpr uint8_t kTransportVersion = 3;
constexpr uint8_t kReferenceVersion = 2;
constexpr uint8_t kRateIndex = 10; // This fork's 500 Hz DF rate table entry.
constexpr uint8_t kSwitchMode = 0; // Wide / 8-channel mode.
constexpr uint8_t kTelemetryRatio = 2;
constexpr uint8_t kFragmentBytes = 4;
constexpr uint8_t kDataFragments = 8;
constexpr uint8_t kParityFragments = 1;
constexpr uint8_t kFragmentCount = kDataFragments + kParityFragments;
constexpr uint8_t kStreamMarker = 0x40;
constexpr uint8_t kTelemetryAck = 0x80;
constexpr uint16_t kTagMask = 0x3ff;
constexpr uint32_t kPeriodUs = 50000;
constexpr uint32_t kStartDeadlineUs = 12000;
constexpr uint32_t kFinishDeadlineUs = 80000;
using Frame = std::array<uint8_t, 38>;
using Block = std::array<uint8_t, kDataFragments * kFragmentBytes>;
enum ControlFlags : uint8_t
{
    Inactive = 1, Armed = 2, Assist = 4,
    CalibrateGyro = 8, CalibrateAccel = 16, CalibrateInflight = 32
};
inline bool validFlags(uint8_t flags)
{
    const bool calibration = flags & (CalibrateGyro | CalibrateAccel);
    return !(flags & 0xc0) &&
           ((flags & Inactive) || (flags & (Armed | Assist)) == (Armed | Assist)) &&
           (!(flags & Assist) || (flags & Armed)) &&
           (!(flags & CalibrateInflight) || (flags & Armed)) &&
           (!calibration || ((flags & Inactive) && !(flags & (Armed | Assist | CalibrateInflight)) &&
                            (flags & (CalibrateGyro | CalibrateAccel)) != (CalibrateGyro | CalibrateAccel)));
}
inline uint16_t u16(const uint8_t *p)
{
    return uint16_t(p[0]) << 8 | p[1];
}
// Offsets refer to the original extended CRSF D5 frame, including its header.
inline uint16_t referenceEpoch(const Frame &f)
{
    return u16(&f[7]);
}
inline uint16_t referenceSequence(const Frame &f)
{
    return u16(&f[9]);
}
inline bool referenceInactive(const Frame &f)
{
    return f[6] & Inactive;
}
inline bool referenceArmed(const Frame &f) { return f[6] & Armed; }
inline uint8_t crc8(const uint8_t *p, unsigned n)
{
    uint8_t c = 0;
    while (n--)
    {
        c ^= *p++;
        for (int i = 0; i < 8; ++i)
            c = uint8_t((c << 1) ^ ((c & 128) ? 0xd5 : 0));
    }
    return c;
}
inline uint16_t blockCrc(const Block &b, uint32_t session)
{
    uint16_t c = 0xffff;
    auto add = [&](uint8_t v) {
        c ^= uint16_t(v) << 8;
        for (int i = 0; i < 8; ++i)
            c = uint16_t((c << 1) ^ ((c & 0x8000) ? 0x1021 : 0));
    };
    for (int i = 3; i >= 0; --i)
        add(uint8_t(session >> (8 * i)));
    for (unsigned i = 0; i < 30; ++i)
    {
        add(b[i]);
    }
    return c;
}
inline bool valid(const Frame &f)
{
    return (f[0] == 0xee || f[0] == 0xc8) && f[1] == 36 && f[2] == 0xd5 && f[3] == 0xc8 && f[4] == 0xea &&
           f[5] == kReferenceVersion && validFlags(f[6]) && u16(&f[7]) != 0 && u16(&f[35]) >= 50 && u16(&f[35]) <= 250 &&
           f.back() == crc8(&f[2], 35);
}
inline bool pack(const Frame &f, uint32_t session, Block &b)
{
    if (!session || !valid(f))
        return false;
    b[0] = f[6];                   // version is negotiated by the CRC-bound session
    std::copy_n(&f[7], 8, &b[1]);    // epoch16, sequence16, source32 unchanged
    b[9] = f[36];                  // valid lease 50..250 fits one byte
    std::copy_n(&f[15], 20, &b[10]); // p/v/a/yaw unchanged signed 16-bit integers
    const auto c = blockCrc(b, session);
    b[30] = uint8_t(c >> 8);
    b[31] = uint8_t(c);
    return true;
}
inline bool unpack(const Block &b, uint32_t session, Frame &f)
{
    if (!session || !validFlags(b[0]) || u16(&b[1]) == 0 || b[9] < 50 || b[9] > 250 ||
        u16(&b[30]) != blockCrc(b, session))
        return false;
    f = {};
    f[0] = 0xc8;
    f[1] = 36;
    f[2] = 0xd5;
    f[3] = 0xc8;
    f[4] = 0xea;
    f[5] = kReferenceVersion;
    f[6] = b[0];
    std::copy_n(&b[1], 8, &f[7]);
    std::copy_n(&b[10], 20, &f[15]);
    f[35] = 0;
    f[36] = b[9];
    f[37] = crc8(&f[2], 35);
    return true;
}
// The FC retains its v1 trajectory contract. Control flags become local UART
// channels only after the receiver has validated the complete reference.
inline Frame flightControllerFrame(Frame f)
{
    f[5] = 1;
    f[6] &= Inactive;
    f[37] = crc8(&f[2], 35);
    return f;
}
struct Sender
{
    uint32_t session = 0, expired = 0, superseded = 0, packets = 0;
    Frame pending{};
    Block active{};
    std::array<uint8_t, kFragmentBytes> parity{};
    uint32_t pendingAt = 0, activeAt = 0, nextStart = 0;
    uint16_t lastEpoch = 0, lastSequence = 0, tag = 0;
    uint8_t index = 0;
    bool havePending = false, haveActive = false, haveLast = false, urgent = false, lastInactive = false, lastArmed = false, haveStart = false;
    void reset(uint32_t s)
    {
        *this = Sender{};
        session = s;
    }
    bool offer(const Frame &f, uint32_t now)
    {
        if (!session || !valid(f))
            return false;
        const auto epoch = referenceEpoch(f), seq = referenceSequence(f);
        if (haveLast && epoch == lastEpoch)
        {
            const auto d = uint16_t(seq - lastSequence);
            if (!d || d >= 32768)
                return false;
        }
        // A new SDK epoch may reset sequence numbers. Renegotiate/reset both peers
        // while disarmed before admitting it; otherwise the short tag is ambiguous.
        if (haveLast && epoch != lastEpoch)
            return false;
        // Preempt on transition to inactive, not every preflight sample. A fast
        // inactive producer must not repeatedly abort the snapshot before completion.
        urgent = urgent || (referenceInactive(f) && !lastInactive) || (!referenceArmed(f) && lastArmed);
        if (urgent)
        {
            haveActive = false;
            havePending = false;
            haveStart = false;
        }
        if (havePending)
            ++superseded;
        pending = f;
        pendingAt = now;
        havePending = true;
        haveLast = true;
        lastEpoch = epoch;
        lastSequence = seq;
        lastInactive = referenceInactive(f);
        lastArmed = referenceArmed(f);
        return true;
    }
    bool next(uint32_t now, bool ack, uint8_t &header, uint8_t payload[5])
    {
        if (!session)
            return false;
        if (haveActive && (uint32_t(now - activeAt) > kFinishDeadlineUs))
        {
            haveActive = false;
            ++expired;
        }
        if (!haveActive && havePending && (!haveStart || urgent || int32_t(now - nextStart) >= 0))
        {
            havePending = false;
            if (uint32_t(now - pendingAt) > kStartDeadlineUs)
            {
                ++expired;
                return false;
            }
            if (!pack(pending, session, active))
                return false;
            activeAt = pendingAt;
            tag = u16(&active[3]) & kTagMask;
            index = 0;
            haveActive = true;
            urgent = false;
            haveStart = true;
            nextStart = now - now % kPeriodUs + kPeriodUs;
            parity = {};
            for (unsigned i = 0; i < active.size(); ++i)
                parity[i % kFragmentBytes] ^= active[i];
        }
        if (!haveActive)
            return false;
        header = uint8_t((ack ? kTelemetryAck : 0) | kStreamMarker | ((tag >> 8) << 4) | index);
        payload[0] = uint8_t(tag);
        for (unsigned i = 0; i < kFragmentBytes; ++i)
            payload[i + 1] = (index == kDataFragments) ? parity[i] : active[kFragmentBytes * index + i];
        ++packets;
        if (++index == kFragmentCount)
            haveActive = false;
        return true;
    }
};
struct Receiver
{
    uint32_t session = 0, recovered = 0, delivered = 0, rejected = 0, expired = 0;
    std::array<std::array<uint8_t, kFragmentBytes>, kFragmentCount> chunks{};
    uint32_t started = 0;
    uint16_t tag = 0, mask = 0;
    bool haveTag = false, closed = false;
    void reset(uint32_t s)
    {
        *this = Receiver{};
        session = s;
    }
    bool push(uint8_t header, const uint8_t payload[5], uint32_t now, Frame &out)
    {
        if (!session || !(header & kStreamMarker) || (header & 15) >= kFragmentCount)
        {
            ++rejected;
            return false;
        }
        const uint16_t incoming = uint16_t((header >> 4) & 3) * 256 + payload[0];
        if (!haveTag)
        {
            haveTag = true;
            tag = incoming;
            mask = 0;
            closed = false;
            started = now;
        }
        else if (incoming != tag)
        {
            const auto delta = uint16_t((incoming - tag) & kTagMask);
            if (delta >= (kTagMask + 1) / 2)
            {
                ++rejected;
                return false;
            }
            tag = incoming;
            mask = 0;
            closed = false;
            started = now;
        }
        if (closed)
            return false;
        if (uint32_t(now - started) > kFinishDeadlineUs)
        {
            closed = true;
            ++expired;
            return false;
        }
        const uint8_t index = header & 15;
        if (mask & (1u << index))
        {
            if (std::memcmp(chunks[index].data(), payload + 1, kFragmentBytes))
            {
                closed = true;
                ++rejected;
            }
            return false;
        }
        std::copy_n(payload + 1, kFragmentBytes, chunks[index].begin());
        mask |= uint16_t(1u << index);
        unsigned count = 0, missing = 0;
        for (unsigned i = 0; i < kDataFragments; ++i)
        {
            if (mask & (1u << i))
                ++count;
            else
                missing = i;
        }
        if (count < kDataFragments && !(count == kDataFragments - 1 && (mask & (1u << kDataFragments))))
            return false;
        if (count == kDataFragments - 1)
        {
            chunks[missing] = chunks[kDataFragments];
            for (unsigned j = 0; j < kDataFragments; ++j)
                if (j != missing)
                    for (unsigned i = 0; i < kFragmentBytes; ++i)
                        chunks[missing][i] ^= chunks[j][i];
        }
        Block block{};
        for (unsigned j = 0; j < kDataFragments; ++j)
            std::copy_n(chunks[j].begin(), kFragmentBytes, &block[j * kFragmentBytes]);
        closed = true;
        if ((u16(&block[3]) & kTagMask) != tag || !unpack(block, session, out))
        {
            ++rejected;
            return false;
        }
        if (count == kDataFragments - 1)
        {
            ++recovered;
        }
        ++delivered;
        return true;
    }
};
} // namespace dfstream
