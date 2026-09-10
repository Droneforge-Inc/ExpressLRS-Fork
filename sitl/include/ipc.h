#pragma once
#include <cstdint>
#include <csignal>
#include <stdexcept>
#include <vector>
namespace sitl {
using Bytes = std::vector<uint8_t>;
extern volatile std::sig_atomic_t stop_requested;
constexpr uint32_t magic = 0x4553494c; // ESIL
constexpr uint16_t version = 1;
constexpr size_t max_payload = 4096;
enum Op : uint16_t { Hello=1, Configure=2, Transmit=3, Receive=4, Advance=5, TelemetryIn=6, Quit=7,
    EnableDownlink=8, QueueTelemetry=9, TransmitTelemetry=10, ReceiveTelemetry=11 };
struct Message { uint16_t op; uint32_t seq; uint64_t time; Bytes data; };
struct Reader {
    const Bytes &data; size_t pos=0;
    uint64_t get(size_t bytes);
    Bytes take(size_t bytes);
    void end() const;
};
void put(Bytes &data, uint64_t value, size_t bytes);
Bytes encode(const Message &m);
bool read_message(int fd, Message &m);
void write_all(int fd, const Bytes &data);
}
