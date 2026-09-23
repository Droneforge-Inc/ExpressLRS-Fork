#include "Df3ReferenceSession.h"
#include <cstdio>
#include <cstdlib>
using namespace dfstream;
static unsigned checks = 0;
static void check(bool yes)
{
    ++checks;
    if (!yes)
    {
        std::fprintf(stderr, "failed check %u\n", checks);
        std::abort();
    }
}
static Frame sample(uint16_t seq, bool inactive)
{
    Frame f{{0xee, 36, 0xd5, 0xc8, 0xea, kReferenceVersion, uint8_t(inactive ? Inactive : Armed | Assist)}};
    put16(&f[7], 7);
    put16(&f[9], seq);
    put32(&f[11], seq * 100);
    put16(&f[35], 200);
    f[37] = crc8(&f[2], 35);
    return f;
}
static Frame command(uint16_t seq, uint32_t sourceMs, uint8_t flags)
{
    auto f = sample(seq, true);
    put32(&f[11], sourceMs);
    f[6] = flags;
    f[37] = crc8(&f[2], 35);
    return f;
}
static void testControls()
{
    for (uint32_t start : {1000000U, 0xffff0000U})
    {
        ReferenceControls controls;
        uint32_t channels[16];
        check(!controls.channels(start, channels));
        check(!controls.accept(command(65534, 0xfffffff0U, Armed | Assist), start, start));
        check(!controls.channels(start, channels)); // boot cannot arm
        check(controls.accept(command(65535, 34, Inactive), start + 50000, start + 50000));
        check(controls.channels(start + 50000, channels));
        check(channels[2] == 172 && channels[4] == 172 && channels[6] == 172);
        check(controls.accept(command(0, 84, Inactive | Armed | Assist), start + 100000, start + 100000));
        check(controls.channels(start + 100000, channels));
        check(channels[4] > 1700 && channels[6] > 1700 && channels[2] == 172);
        auto active = command(1, 134, Armed | Assist);
        check(controls.accept(active, start + 150000, start + 150000));
        check(controls.channels(start + 150000, channels));
        check(channels[0] == 992 && channels[1] == 992 && channels[3] == 992 && channels[2] == 350);
        check(controls.accept(command(2, 184, Armed | Assist | CalibrateInflight), start + 200000, start + 200000));
        check(controls.channels(start + 200000, channels) && channels[8] > 1700);
        check(!controls.accept(active, start + 250000, start + 250000)); // reordering cannot renew
        check(controls.channels(start + 400000, channels));
        check(!controls.fresh(start + 400001)); // UART repeats cannot renew the reference.
        check(controls.channels(start + 400001, channels));
        check(channels[4] == 1800 && channels[6] == 1800 && channels[2] == 350 && channels[8] == 172);
        check(controls.channels(start + 3000000, channels)); // Reference loss does not create RX LOSS.
        check(!controls.fresh(start + 3000000));
        check(controls.accept(command(3, 3034, Armed | Assist), start + 3050000, start + 3050000));
        check(controls.channels(start + 3050000, channels) && channels[4] == 1800);
        // Reset time for the following calibration cases with an explicit disarm.
        controls.reset();
        check(controls.accept(command(4, 484, Inactive), start + 500000, start + 500000));
        check(controls.channels(start + 500000, channels) && channels[4] == 172);
        check(controls.accept(command(5, 534, Armed | Assist), start + 550000, start + 550000));
        check(controls.channels(start + 550000, channels) && channels[4] > 1700);
        check(controls.accept(command(6, 584, Inactive | CalibrateGyro), start + 600000, start + 600000));
        check(controls.channels(start + 600000, channels));
        check(channels[4] == 172 && channels[1] == 172 && channels[3] == 172 && channels[2] == 172);
        check(controls.accept(command(7, 634, Inactive | CalibrateAccel), start + 650000, start + 650000));
        check(controls.channels(start + 650000, channels));
        check(channels[4] == 172 && channels[1] == 172 && channels[3] == 172 && channels[2] == 1800);
        check(!controls.accept(command(7, 634, Inactive | CalibrateAccel), start + 700000, start + 700000));
        auto wrongEpoch = command(8, 684, Inactive);
        put16(&wrongEpoch[7], 8); wrongEpoch[37] = crc8(&wrongEpoch[2], 35);
        check(!controls.accept(wrongEpoch, start + 700000, start + 700000));
        check(!controls.accept(command(8, 1634, Inactive), start + 700000, start + 700000)); // impossible source jump
        check(controls.channels(start + 850001, channels));
        check(channels[4] == 172 && channels[1] == 992 && channels[3] == 992 && channels[2] == 172);
        controls.reset();
        check(!controls.accept(command(8, 684, Armed | Assist), start + 700000, start + 700000));
    }
    ReferenceControls controls;
    check(controls.accept(command(0, 0, Inactive), 1000000, 1000000));
    check(controls.accept(command(1, 10, Armed | Assist), 1100000, 1100000));
    check(controls.accept(command(2, 20, Armed | Assist), 1200000, 1200000));
    check(!controls.accept(command(3, 30, Armed | Assist), 1300000, 1300000)); // queued sources, fresh receipt
    controls.reset();
    check(controls.accept(command(0, 0, Inactive), 1000000, 1000000));
    check(!controls.accept(command(1, 50, Armed | Assist), 1050000, 1250001)); // Expired in the foreground queue.
    uint32_t channels[16];
    check(controls.channels(1250001, channels) && channels[4] == 172);
    for (uint8_t flags : {uint8_t(0), uint8_t(Armed), uint8_t(Assist),
                         uint8_t(Inactive | CalibrateAccel | CalibrateGyro),
                         uint8_t(Inactive | Armed | CalibrateGyro), uint8_t(Inactive | 0x80)})
        check(!controls.accept(command(4, 400, flags), 1400000, 1400000));
    check(controls.accept(command(4, 400, Inactive), 1400000, 1400000));
}
int main()
{
    testControls();
    for (uint32_t start : {0U, 0xffff0000U})
    {
        TxSession tx;
        RxSession rx;
        Control msg, reply;
        tx.begin(123, 7, start);
        check(!tx.ready());
        check(tx.nextControl(start, msg));
        check(!rx.receive(msg.data(), msg.size(), start, true, true,
                          reply)); // armed RX
        check(!rx.receive(msg.data(), msg.size(), start, false, false,
                          reply)); // mode/model mismatch
        auto bad = msg;
        bad[17] = 4;
        bad[26] = crc8(&bad[2], 24);
        check(!rx.receive(bad.data(), bad.size(), start, true, false,
                          reply)); // wrong ratio
        check(rx.receive(msg.data(), msg.size(), start, true, false, reply));
        check(!tx.nextControl(start + 100000, msg)); // lost READY; bounded retry
        check(tx.nextControl(start + 250000, msg));
        check(
            rx.receive(msg.data(), msg.size(), start + 250000, true, false, reply));
        check(tx.receive(reply.data(), reply.size(), start + 250000));
        check(tx.nextControl(start + 252000, msg));
        check(
            rx.receive(msg.data(), msg.size(), start + 252000, true, false, reply));
        check(rx.active && !tx.ready()); // lost ACTIVE
        check(tx.nextControl(start + 502000, msg));
        check(
            rx.receive(msg.data(), msg.size(), start + 502000, true, false, reply));
        check(tx.receive(reply.data(), reply.size(), start + 502000));
        check(tx.state == Streaming && !tx.ready());
        auto hb = heartbeat(123, 7, 12, true);
        check(tx.receive(hb.data(), hb.size(), start + 600000));
        check(tx.ready());
        tx.tick(start + 650000, true, true);
        check(tx.ready()); // arming does not reset active session
        auto wrong = heartbeat(124, 7, 12, true);
        tx.receive(wrong.data(), wrong.size(), start + 700000);
        check(tx.session == 123 && tx.ready());
        tx.tick(start + 3100001, true, true);
        check(tx.state == Failed && !tx.ready()); // health expired across wrap
        tx.begin(125, 7, start);
        tx.tick(start, true, true);
        check(tx.state == Disabled); // armed handshake prohibited
        tx.begin(126, 7, start);
        tx.tick(start + 2000001, true, false);
        check(tx.state == Failed);
        tx.begin(127, 7, start);
        tx.tick(start, false, false);
        check(tx.state == Disabled);
    }
    { // One-sided RX reboot is reported before it can accept old-session data.
        TxSession tx;
        RxSession rx;
        Control msg, reply;
        tx.begin(123, 7, 0);
        tx.nextControl(0, msg);
        rx.receive(msg.data(), msg.size(), 0, true, false, reply);
        tx.receive(reply.data(), reply.size(), 0);
        tx.nextControl(1, msg);
        rx.receive(msg.data(), msg.size(), 1, true, false, reply);
        tx.receive(reply.data(), reply.size(), 1);
        auto hb = heartbeat(123, 7, 12, true);
        tx.receive(hb.data(), hb.size(), 2);
        check(tx.ready());
        rx.reset();
        hb = heartbeat(0, 0, 0, false);
        tx.receive(hb.data(), hb.size(), 3);
        check(!tx.ready() && tx.state == Failed);
    }
    for (uint32_t start : {0U, 0x80000000U, 0xffff8000U})
    {
        Sender tx;
        Receiver rx;
        tx.reset(123);
        rx.reset(123);
        check(tx.offer(sample(0, false), start));
        uint8_t h, p[5];
        Frame out;
        for (unsigned i = 0; i < 9; ++i)
        {
            check(tx.next(start + i * 8000, false, h, p));
            if (rx.push(h, p, start + i * 8000, out))
                check(out[9] == 0 && out[10] == 0);
        }
        check(rx.delivered == 1);
        check(tx.offer(sample(1, false), start + 100000));
        check(tx.next(start + 100000, false, h, p));
    }
    { // A 100 Hz inactive producer cannot repeatedly abort the active snapshot.
        Sender tx;
        Receiver rx;
        tx.reset(123);
        rx.reset(123);
        Frame out;
        uint8_t h, p[5];
        for (uint32_t t = 0; t < 800000; t += 2000)
        {
            if (t % 10000 == 0)
                check(tx.offer(sample(t / 10000, true), t));
            if (t % 4000 == 0 && tx.next(t, false, h, p))
                rx.push(h, p, t, out);
        }
        check(rx.delivered == 16);
        check(tx.packets == 144);
    }
    std::printf("PASS: %u reference session, admission and timer-wrap checks\n",
                checks);
}
