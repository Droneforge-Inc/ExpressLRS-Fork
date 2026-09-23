#pragma once
#include "Df3ReferenceStream.h"

namespace dfstream
{
// Private vendor command: 0x32 / Nimbus 0x80 / reference transport 0x44.
// Fixed profile fields must match exactly; a version number alone is insufficient.
using Control = std::array<uint8_t, 27>;
using Heartbeat = std::array<uint8_t, 14>;
using LocalStatus = std::array<uint8_t, 15>;
constexpr uint32_t kNegotiationTimeoutUs = 2000000;
constexpr uint32_t kControlRetryUs = 250000;
constexpr uint32_t kHeartbeatTimeoutUs = 2500000;
constexpr uint32_t kHeartbeatPeriodUs = 1000000;
constexpr uint32_t kRcFreshnessUs = 250000;
constexpr uint32_t kLocalStatusPeriodUs = 250000;
constexpr uint32_t kLocalControlPeriodUs = 20000;
constexpr uint8_t kIdleFragment = 15;
enum Operation : uint8_t
{
    Hello = 1,
    Ready = 2,
    Commit = 3,
    Active = 4
};
enum State : uint8_t
{
    Disabled = 0,
    Negotiating = 1,
    Streaming = 2,
    Failed = 3
};
inline uint32_t u32(const uint8_t *p)
{
    return uint32_t(u16(p)) << 16 | u16(p + 2);
}
inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v;
}
inline void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v);
}
inline Control control(Operation op, uint32_t session, uint16_t epoch)
{
    Control f{{0xc8, 25, 0x32, 0, 0, 0x80, 0x44, uint8_t(op), kTransportVersion}};
    const bool reply = op == Ready || op == Active;
    f[3] = reply ? 0xee : 0xec;
    f[4] = reply ? 0xec : 0xee;
    put32(&f[9], session);
    put16(&f[13], epoch);
    // rate, switches, telemetry ratio, bytes/fragment, data/parity counts,
    // period, start/finish/assembly deadlines (ms), and session-bound CRC.
    const uint8_t profile[] = {kRateIndex, kSwitchMode, kTelemetryRatio, kFragmentBytes,
                              kDataFragments, kParityFragments, kPeriodUs / 1000, kStartDeadlineUs / 1000,
                              kFinishDeadlineUs / 1000, kFinishDeadlineUs / 1000, 1};
    std::copy_n(profile, sizeof(profile), &f[15]);
    f[26] = crc8(&f[2], 24);
    return f;
}
inline bool isControl(const uint8_t *p, unsigned n)
{
    return n >= 7 && p[2] == 0x32 && p[5] == 0x80 && p[6] == 0x44;
}
inline bool parseControl(const uint8_t *p, unsigned n, Operation &op, uint32_t &session, uint16_t &epoch)
{
    if (n != 27 || p[1] != 25 || !isControl(p, n) || p[7] < Hello || p[7] > Active)
        return false;
    op = Operation(p[7]);
    session = u32(p + 9);
    epoch = u16(p + 13);
    const auto expected = control(op, session, epoch);
    return session && epoch && std::equal(expected.begin() + 1, expected.end(), p + 1);
}
inline Heartbeat heartbeat(uint32_t session, uint16_t epoch, uint16_t sequence, bool complete)
{
    Heartbeat f{{0xc8, 12, 0xd9, kTransportVersion, uint8_t(complete)}};
    put32(&f[5], session);
    put16(&f[9], epoch);
    put16(&f[11], sequence);
    f[13] = crc8(&f[2], 11);
    return f;
}
// References expire independently of the FC's receiver channels, so DF3 can
// hold/descend on reference loss. The caller must reset on radio/session loss;
// a new session needs a fresh disarm before it may arm. UART repeats never renew
// a reference's source/receipt lease or extend calibration gestures.
struct ReferenceControls
{
    uint32_t received = 0, source = 0, sourceLocal = 0, lease = 0;
    uint16_t sequence = 0, epoch = 0;
    uint8_t flags = Inactive;
    bool have = false, blocked = true;
    void reset() { *this = ReferenceControls{}; }
    bool fresh(uint32_t now) const
    {
        return have && !blocked && uint32_t(now - received) <= lease && uint32_t(now - sourceLocal) <= lease;
    }
    bool accept(const Frame &f, uint32_t at, uint32_t now)
    {
        if (!valid(f))
            return false;
        const uint16_t seq = referenceSequence(f), e = referenceEpoch(f);
        const uint32_t stamp = u32(&f[11]);
        uint32_t mapped = at;
        if (have)
        {
            const uint16_t delta = uint16_t(seq - sequence);
            const uint32_t dt = stamp - source;
            if (e != epoch || !delta || delta >= 32768 || !dt || dt >= 0x80000000U ||
                uint64_t(dt) * 1000 > uint64_t(uint32_t(at - received)) + 250000)
                return false;
            mapped = sourceLocal + dt * 1000;
            if (int32_t(mapped - at) > 0)
                mapped = at;
        }
        const uint32_t ttl = uint32_t(u16(&f[35])) * 1000;
        // Validate at service time before changing permissions: a queued arm
        // or calibration command must not take effect after its lease expired.
        if (uint32_t(now - at) > ttl || uint32_t(now - mapped) > ttl)
            return false;
        have = true;
        received = at;
        source = stamp;
        sourceLocal = mapped;
        lease = ttl;
        sequence = seq;
        epoch = e;
        flags = f[6];
        if (!(flags & Armed))
            blocked = false;
        return !blocked;
    }
    bool channels(uint32_t now, uint32_t out[16])
    {
        if (!have || blocked)
            return false;
        const uint8_t permissions = fresh(now) ? flags : flags & (Inactive | Armed | Assist);
        std::fill_n(out, 16, 992);
        out[2] = (permissions & Inactive) ? 172 : 350;
        out[4] = (permissions & Armed) ? 1800 : 172;
        out[5] = 172; // Nimbus ANGLE mode; no stick piloting in DF3.
        out[6] = (permissions & Assist) ? 1800 : 172;
        out[8] = (permissions & CalibrateInflight) ? 1800 : 172;
        if (permissions & (CalibrateGyro | CalibrateAccel))
        {
            out[1] = out[3] = 172;
            out[2] = (permissions & CalibrateAccel) ? 1800 : 172;
        }
        return true;
    }
};
struct TxSession
{
    State state = Disabled;
    uint32_t session = 0, started = 0, lastControl = 0, lastHeartbeat = 0;
    uint16_t epoch = 0, lastSequence = 0;
    Operation waiting = Ready;
    bool sent = false, haveHeartbeat = false, rxComplete = false;
    void reset() { *this = TxSession{}; }
    void begin(uint32_t nonce, uint16_t e, uint32_t now)
    {
        reset();
        if (!nonce || !e)
            return;
        session = nonce;
        epoch = e;
        started = now;
        state = Negotiating;
    }
    void tick(uint32_t now, bool allowed, bool armed)
    {
        if (!allowed || (state == Negotiating && armed))
        {
            reset();
            return;
        }
        if (state == Negotiating && uint32_t(now - started) > kNegotiationTimeoutUs)
            state = Failed;
        if (state == Streaming && uint32_t(now - (haveHeartbeat ? lastHeartbeat : started)) > kHeartbeatTimeoutUs)
            state = Failed;
    }
    bool nextControl(uint32_t now, Control &out)
    {
        if (state != Negotiating || (sent && uint32_t(now - lastControl) < kControlRetryUs))
            return false;
        out = control(waiting == Ready ? Hello : Commit, session, epoch);
        lastControl = now;
        sent = true;
        return true;
    }
    bool receive(const uint8_t *p, unsigned n, uint32_t now)
    {
        if (isControl(p, n))
        {
            Operation op;
            uint32_t s;
            uint16_t e;
            if (!parseControl(p, n, op, s, e) || s != session || e != epoch || state != Negotiating)
                return true;
            if (op == Ready && waiting == Ready)
            {
                waiting = Active;
                sent = false;
            }
            else if (op == Active && waiting == Active)
            {
                state = Streaming;
                started = now;
            }
            return true;
        }
        if (n != 14 || p[2] != 0xd9)
            return false;
        if (p[1] != 12 || p[3] != kTransportVersion || p[4] > 1 || crc8(p + 2, 11) != p[13])
            return true;
        const auto s = u32(p + 5);
        const auto e = u16(p + 9);
        if (state == Streaming && s == 0)
        {
            state = Failed;
            rxComplete = false;
            return true;
        }
        if (state == Streaming && s == session && e == epoch)
        {
            lastHeartbeat = now;
            haveHeartbeat = true;
            rxComplete = p[4];
            lastSequence = u16(p + 11);
        }
        return true;
    }
    bool ready() const { return state == Streaming && haveHeartbeat && rxComplete; }
};
// USB-only readiness report. Encoding lives beside the session contract so the
// embedded adapter and host compatibility tests cannot drift apart.
inline LocalStatus localStatus(const TxSession &session, bool allowed)
{
    LocalStatus f{{0xea, 13, 0xe5, kTransportVersion, uint8_t(session.state), uint8_t(allowed && session.ready())}};
    put16(&f[6], session.epoch);
    put32(&f[8], session.session);
    put16(&f[12], session.lastSequence);
    f.back() = crc8(&f[2], f.size() - 3);
    return f;
}
struct RxSession
{
    uint32_t session = 0, preparedAt = 0;
    uint16_t epoch = 0;
    bool active = false;
    void reset() { *this = RxSession{}; }
    bool receive(const uint8_t *p, unsigned n, uint32_t now, bool allowed, bool armed, Control &reply)
    {
        Operation op;
        uint32_t s;
        uint16_t e;
        if (!allowed || armed || !parseControl(p, n, op, s, e) || (op != Hello && op != Commit))
            return false;
        if (op == Hello)
        {
            if (s != session || e != epoch)
            {
                reset();
                session = s;
                epoch = e;
                preparedAt = now;
            }
            reply = control(Ready, session, epoch);
            return true;
        }
        if (s != session || e != epoch || (!active && uint32_t(now - preparedAt) > kNegotiationTimeoutUs))
            return false;
        active = true;
        reply = control(Active, session, epoch);
        return true;
    }
};
// At 1:2, every odd nonce is an uplink. Piloting permissions now travel inside
// complete references; normal RC remains available before/after DF3 ownership.
inline bool dataSlot(uint8_t nonce)
{
    return (nonce & 1) == 1;
}
} // namespace dfstream
