#pragma once

#include <stdint.h>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <algorithm>
#if defined(TARGET_SITL)
#include <stdexcept>
#endif

#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>

#define DEVICE_NAME "testing"
#define RADIO_SX128X 1

typedef uint8_t byte;

#define HEX 16
#define DEC 10

class Stream
{
public:
    Stream() {}
    virtual ~Stream() {}

    // Stream methods
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual void flush() = 0;

    // Print methods
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t *s, size_t l) = 0;

#if defined(TARGET_SITL)
    size_t readBytes(uint8_t *buffer, size_t count) {
        size_t n = 0;
        while (n < count) {
            int value = read();
            if (value < 0) break;
            buffer[n++] = static_cast<uint8_t>(value);
        }
        return n;
    }
#endif

    int print(const char *s) {return 0;}
    int print(uint8_t s) {return 0;}
    int print(uint8_t s, int radix) {return 0;}
    int println() {return 0;}
    int println(const char *s) {return 0;}
    int println(uint8_t s) {return 0;}
    int println(uint8_t s, int radix) {return 0;}
};

class HardwareSerial: public Stream {
public:
    // Stream methods
    int available() {return 0;}
    int read() {return -1;}
    int peek() {return 0;}
    void flush() {}
    void end() {}
    void begin(int baud) {}
    void enableHalfDuplexRx() {}
    int availableForWrite() {return 256;}

    // Print methods
    size_t write(uint8_t c) {return 1;}
    size_t write(const uint8_t *s, size_t l) {return l;}

    int print(const char *s) {return 0;}
    int print(uint8_t s) {return 0;}
    int print(uint8_t s, int radix) {return 0;}
    int println() {return 0;}
    int println(const char *s) {return 0;}
    int println(uint8_t s) {return 0;}
    int println(uint8_t s, int radix) {return 0;}
};

static HardwareSerial Serial;
static Stream *SerialLogger = &Serial;

inline void interrupts() {}
inline void noInterrupts() {}

#if defined(TARGET_SITL)
extern uint64_t sitl_time_us;
inline unsigned long micros() { return sitl_time_us; }
inline unsigned long millis() { return sitl_time_us / 1000; }
inline void delay(int32_t) { throw std::runtime_error("Blocking delay is unsupported in ELRS protocol SITL"); }
using std::min;
using std::max;
inline long map(long x, long a, long b, long c, long d) {
    return (x-a)*(d-c)/(b-a)+c;
}
#else
inline unsigned long micros() {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec*(uint64_t)1000000+tv.tv_usec;
}

inline void delay(int32_t time) {
    usleep(time);
}
inline unsigned long millis() { return 0; }
#endif

#define bit(x) (1 << (x))
#if defined(TARGET_SITL)
inline void delayMicroseconds(int) { throw std::runtime_error("Blocking delay is unsupported in ELRS protocol SITL"); }
#else
inline void delayMicroseconds(int delay) { }
#endif
inline char *itoa(int32_t value, char *str, int base) { sprintf(str, "%d", value); return str; }
inline char *utoa(uint32_t value, char *str, int base) { sprintf(str, "%u", value); return str; }
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))

#ifdef _WIN32
#define random rand
#define srandom srand
#endif
