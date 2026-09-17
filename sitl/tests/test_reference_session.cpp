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
    Frame f{{0xee, 36, 0xd5, 0xc8, 0xea, 1, uint8_t(inactive)}};
    put16(&f[7], 7);
    put16(&f[9], seq);
    put32(&f[11], seq * 100);
    put16(&f[35], 200);
    f[37] = crc8(&f[2], 35);
    return f;
}
int main()
{
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
            if (t % 8000 == 4000 && tx.next(t, false, h, p))
                rx.push(h, p, t, out);
        }
        check(rx.delivered == 8);
        check(tx.packets == 72);
    }
    std::printf("PASS: %u reference session, admission and timer-wrap checks\n",
                checks);
}
