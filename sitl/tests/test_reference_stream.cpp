#include "Df3ReferenceStream.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dfstream;
constexpr uint32_t SESSION = 0xdf330002;
static unsigned checks = 0;
void check(bool yes)
{
    ++checks;
    if (!yes)
    {
        std::fprintf(stderr, "failed check %u\n", checks);
        std::abort();
    }
}
void put16(Frame &f, unsigned at, uint16_t n)
{
    f[at] = n >> 8;
    f[at + 1] = n;
}
void finish(Frame &f)
{
    f[37] = crc8(&f[2], 35);
}
Frame frame(uint16_t seq, uint32_t stamp, bool inactive = false)
{
    Frame f{};
    f[0] = 0xee;
    f[1] = 36;
    f[2] = 0xd5;
    f[3] = 0xc8;
    f[4] = 0xea;
    f[5] = kReferenceVersion;
    f[6] = inactive ? Inactive : Armed | Assist;
    put16(f, 7, 7);
    put16(f, 9, seq);
    f[11] = stamp >> 24;
    f[12] = stamp >> 16;
    f[13] = stamp >> 8;
    f[14] = stamp;
    for (unsigned i = 0; i < 9; ++i)
        put16(f, 15 + 2 * i, uint16_t(seq * 17 + i * 151));
    put16(f, 33, int16_t(int(seq % 5000) - 2500));
    put16(f, 35, 200);
    finish(f);
    return f;
}
Frame fcFrame(Frame f)
{
    f[0] = 0xc8;
    return f;
}
struct Fragment
{
    uint8_t h;
    std::array<uint8_t, 5> data;
};
std::vector<Fragment> fragments(Frame f)
{
    Sender s;
    s.reset(SESSION);
    check(s.offer(f, 0));
    std::vector<Fragment> out;
    for (unsigned i = 0; i < 9; ++i)
    {
        Fragment v{};
        check(s.next(i * 8000, true, v.h, v.data.data()));
        out.push_back(v);
    }
    Fragment unused{};
    check(!s.next(72000, true, unused.h, unused.data.data()));
    return out;
}
int main()
{
    std::mt19937 random(923);
    for (unsigned i = 0; i < 10000; ++i)
    {
        Frame f = frame(uint16_t(random()), random());
        put16(f, 35, 50 + random() % 201);
        finish(f);
        Block b{};
        Frame got{};
        check(pack(f, SESSION, b));
        check(unpack(b, SESSION, got));
        check(got == fcFrame(f));
    }
    Frame f = frame(13, 1300);
    auto parts = fragments(f);
    Frame out{};
    for (int lost = -1; lost < 9; ++lost)
    {
        Receiver r;
        r.reset(SESSION);
        unsigned deliveries = 0;
        for (unsigned i = 0; i < 9; ++i)
            if (int(i) != lost)
            {
                bool done = r.push(parts[i].h, parts[i].data.data(), i * 8000, out);
                if (done)
                {
                    ++deliveries;
                    check(out == fcFrame(f));
                }
                check(!r.push(parts[i].h, parts[i].data.data(), i * 8000 + 1,
                              out)); // duplicates do not renew or re-publish
            }
        check(deliveries == 1);
        check(r.recovered == unsigned(lost >= 0 && lost < 8));
    }
    for (unsigned a = 0; a < 9; ++a)
        for (unsigned b = a + 1; b < 9; ++b)
        {
            Receiver r;
            r.reset(SESSION);
            for (unsigned i = 0; i < 9; ++i)
                if (i != a && i != b)
                    check(!r.push(parts[i].h, parts[i].data.data(), i * 8000, out));
            check(r.delivered == 0);
            auto next = fragments(frame(14, 1400));
            for (unsigned i = 0; i < 9; ++i)
                if (r.push(next[i].h, next[i].data.data(), 100000 + i * 8000, out))
                    check(out == fcFrame(frame(14, 1400)));
            check(r.delivered == 1);
        }
    for (unsigned trial = 0; trial < 1000; ++trial)
    {
        auto shuffled = parts;
        std::shuffle(shuffled.begin(), shuffled.end(), random);
        Receiver r;
        r.reset(SESSION);
        for (unsigned i = 0; i < 9; ++i)
            if (r.push(shuffled[i].h, shuffled[i].data.data(), i * 8000, out))
                check(out == fcFrame(f));
        check(r.delivered == 1);
    }
    { // old/new fragments cannot produce a mixed snapshot
        auto old = fragments(frame(12, 1200));
        Receiver r;
        r.reset(SESSION);
        for (unsigned i = 0; i < 4; ++i)
            check(!r.push(old[i].h, old[i].data.data(), i * 2000, out));
        for (unsigned i = 0; i < 9; ++i)
        {
            if (r.push(parts[i].h, parts[i].data.data(), 16000 + i * 4000, out))
                check(out == fcFrame(f));
            check(!r.push(old[i].h, old[i].data.data(), 16001 + i * 4000, out));
        }
        check(r.delivered == 1);
    }
    { // conflicting duplicate closes this assembly
        Receiver r;
        r.reset(SESSION);
        check(!r.push(parts[0].h, parts[0].data.data(), 0, out));
        auto bad = parts[0];
        bad.data[2] ^= 1;
        check(!r.push(bad.h, bad.data.data(), 1, out));
        for (unsigned i = 1; i < 9; ++i)
            check(!r.push(parts[i].h, parts[i].data.data(), i * 8000, out));
        check(r.delivered == 0 && r.rejected == 1);
    }
    { // every single block-bit corruption is detected by end-to-end CRC
        Block b{};
        check(pack(f, SESSION, b));
        for (unsigned bit = 0; bit < 256; ++bit)
        {
            Block bad = b;
            bad[bit / 8] ^= 1u << (bit % 8);
            check(!unpack(bad, SESSION, out));
        }
        check(!unpack(b, SESSION + 1, out));
    }
    { // disabled capability/session means no reference transport
        Sender s;
        Receiver r;
        check(!s.offer(f, 0));
        check(!r.push(parts[0].h, parts[0].data.data(), 0, out));
        r.reset(SESSION + 1);
        for (unsigned i = 0; i < 9; ++i)
            check(!r.push(parts[i].h, parts[i].data.data(), i * 8000, out));
        check(r.delivered == 0);
    }
    { // start and finish deadlines; same expired snapshot cannot reopen itself
        Sender s;
        s.reset(SESSION);
        check(s.offer(f, 0));
        Fragment v{};
        check(!s.next(12001, false, v.h, v.data.data()));
        check(s.expired == 1);
        s.reset(SESSION);
        check(s.offer(f, 0));
        check(s.next(0, false, v.h, v.data.data()));
        check(!s.next(80001, false, v.h, v.data.data()));
        Receiver r;
        r.reset(SESSION);
        check(!r.push(parts[0].h, parts[0].data.data(), 0, out));
        for (unsigned i = 1; i < 9; ++i)
            check(!r.push(parts[i].h, parts[i].data.data(), 80001 + i, out));
        check(r.expired == 1 && r.delivered == 0);
    }
    { // 10-bit tag and 16-bit full sequence wrap
        for (uint16_t start : {uint16_t(1023), uint16_t(65535)})
        {
            Receiver r;
            r.reset(SESSION);
            for (unsigned j = 0; j < 2; ++j)
            {
                auto f2 = frame(uint16_t(start + j), 100 + j * 100);
                auto v = fragments(f2);
                for (unsigned i = 0; i < 9; ++i)
                    if (r.push(v[i].h, v[i].data.data(), j * 100000 + i * 8000, out))
                        check(out == fcFrame(f2));
            }
            check(r.delivered == 2);
        }
    }
    { // urgent inactive frame replaces an unfinished active snapshot
        Sender s;
        s.reset(SESSION);
        check(s.offer(f, 0));
        Fragment v{};
        check(s.next(0, false, v.h, v.data.data()));
        check(s.offer(frame(14, 1400, true), 10000));
        check(s.next(10000, false, v.h, v.data.data()));
        check(v.data[0] == 14 && (v.h & 15) == 0);
    }
    { // SDK epoch/sequence reset requires a fresh, jointly negotiated session
        Sender s;
        Receiver r;
        s.reset(SESSION);
        r.reset(SESSION);
        Fragment v{};
        check(s.offer(f, 0));
        for (unsigned i = 0; i < 9; ++i)
        {
            check(s.next(i * 8000, false, v.h, v.data.data()));
            r.push(v.h, v.data.data(), i * 8000, out);
        }
        check(r.delivered == 1);
        auto nextEpoch = frame(0, 0, true);
        put16(nextEpoch, 7, 8);
        finish(nextEpoch);
        check(!s.offer(nextEpoch, 100000));
        s.reset(SESSION + 1);
        r.reset(SESSION + 1);
        check(s.offer(nextEpoch, 100000));
        for (unsigned i = 0; i < 9; ++i)
        {
            check(s.next(100000 + i * 8000, false, v.h, v.data.data()));
            if (r.push(v.h, v.data.data(), 100000 + i * 8000, out))
                check(out == fcFrame(nextEpoch));
        }
        check(r.delivered == 1);
    }
    std::printf(
        "PASS: %u checks; exact reference payload parity, all 9 single erasures, "
        "all 36 double erasures, 1000 reorderings, duplicates, corruption, "
        "deadlines, resets, wraps. Sender=%zu B Receiver=%zu B.\n",
        checks, sizeof(Sender), sizeof(Receiver));
}
