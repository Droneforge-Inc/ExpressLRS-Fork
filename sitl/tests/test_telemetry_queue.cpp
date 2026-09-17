#include "telemetry.h"
#include <array>
#include <cstdio>
#include <cstdlib>

GENERIC_CRC8 crsf_crc(CRSF_CRC_POLY);
using Frame = std::array<uint8_t, CRSF_MAX_PACKET_LEN>;

static void check(bool result)
{
    if (!result)
        std::abort();
}

static Frame frame(crsf_frame_type_e type, uint8_t length, uint8_t value)
{
    Frame f{};
    f[0] = CRSF_ADDRESS_FLIGHT_CONTROLLER;
    f[1] = length - 2;
    f[2] = type;
    f[3] = value;
    return f;
}

static void expect(Telemetry &queue, const Frame &expected)
{
    uint8_t length = 0;
    Frame actual{};
    check(queue.GetNextPayload(&length, actual.data()));
    check(length == expected[1] + 2);
    check(std::equal(actual.begin(), actual.begin() + length, expected.begin()));
}

int main()
{
    for (auto type : {CRSF_FRAMETYPE_DF_STATE, CRSF_FRAMETYPE_DF_REFERENCE_HEALTH,
                      CRSF_FRAMETYPE_PARAMETER_SETTINGS_ENTRY})
    {
        Telemetry queue;
        auto ordinary = frame(CRSF_FRAMETYPE_ATTITUDE, 10, 0);
        queue.AppendTelemetryPackage(ordinary.data());
        // In-place overwrites, growing replacements, and tombstones must all
        // represent exactly one waiting priority item, across uint8 wrap counts.
        Frame latest{};
        for (unsigned i = 0; i < 1024; ++i)
        {
            latest = frame(type, i % 2 ? 38 : 14, uint8_t(i));
            queue.AppendTelemetryPackage(latest.data());
        }
        check(queue.UpdatedPayloadCount() == 2);
        expect(queue, latest);
        expect(queue, ordinary);
        uint8_t length;
        Frame unused;
        check(!queue.GetNextPayload(&length, unused.data()));
    }
    {
        Telemetry queue;
        // Force eviction of live priority frames using ordinary, non-replaced
        // sensor frames; repeated eviction must not accumulate phantom priority.
        auto ordinary = frame(CRSF_FRAMETYPE_DF_RAW_IMU, 64, 0);
        auto health = frame(CRSF_FRAMETYPE_DF_REFERENCE_HEALTH, 14, 2);
        for (unsigned i = 0; i < 255; ++i)
        {
            queue.AppendTelemetryPackage(health.data());
            for (unsigned j = 0; j < 8; ++j)
                queue.AppendTelemetryPackage(ordinary.data());
        }
        queue.AppendTelemetryPackage(health.data());
        expect(queue, health);
        check(queue.UpdatedPayloadCount() > 0);
    }
    std::puts(
        "PASS: telemetry priority survives replacement, resizing and eviction");
}
