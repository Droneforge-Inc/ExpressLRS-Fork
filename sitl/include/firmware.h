#pragma once
#include "ipc.h"
#include <deque>
#include <limits>
namespace sitl {
struct UartByte { uint64_t time;uint8_t value; };
class Firmware {
public:
    Bytes handle(uint16_t op,uint64_t time,const Bytes &payload);
private:
    bool configured=false,haveSlot=false;
    bool downlink=false,haveTelemetrySlot=false;
    uint64_t lastTelemetrySlot=0;
    uint64_t now=0,lastSlot=0,uartEnd=0;
    uint32_t baud=420000;
    uint32_t frequency(uint64_t slot) const;
    Bytes drain();
    std::deque<UartByte> uart;
};
}
