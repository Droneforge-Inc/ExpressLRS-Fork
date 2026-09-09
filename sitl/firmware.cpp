#include "firmware.h"
#include "targets.h"
#include "common.h"
#include "OTA.h"
#include "FHSS.h"
#include "options.h"
#include <algorithm>
#include <iterator>
#include <string>
uint64_t sitl_time_us=0;
firmware_options_t firmwareOptions{};
extern const char device_name[]="DF ELRS SITL";
extern const char version[]="3.0.0 SITL";
extern const char hardware_version[]="0.0.0 host";
#if TARGET_RX
#include "rx-serial/SerialCRSF.h"
#include "telemetry.h"
Telemetry telemetry;
// Unsupported hardware-management requests fail explicitly, never touch hardware.
void reset_into_bootloader() { throw std::runtime_error("bootloader unsupported in SITL"); }
void EnterBindingModeSafely() { throw std::runtime_error("binding unsupported in pre-synchronized SITL"); }
void UpdateModelMatch(uint8_t) { throw std::runtime_error("model-match changes unsupported in SITL"); }
class CaptureStream : public Stream {
public:
    sitl::Bytes bytes;
    int available() override {return 0;}
    int read() override {return -1;}
    int peek() override {return -1;}
    void flush() override {}
    size_t write(uint8_t b) override {bytes.push_back(b);return 1;}
    size_t write(const uint8_t *p,size_t n) override {bytes.insert(bytes.end(),p,p+n);return n;}
};
static CaptureStream serial;
static SerialCRSF crsf(serial,serial);
#endif
namespace sitl {
uint32_t Firmware::frequency(uint64_t slot) const {
    auto index=(slot/ExpressLRS_currAirRate_Modparams->FHSShopInterval)%FHSSgetSequenceCount();
    return FHSSconfig->freq_start+FHSSsequence[index]*freq_spread/FREQ_SPREAD_SCALE;
}
Bytes Firmware::drain() {
    Bytes out;size_t count=0;for(const auto &b:uart) {if(b.time>now || count==400)break;++count;}
    put(out,count,2);
    for(size_t i=0;i<count;++i) {auto b=uart.front();uart.pop_front();put(out,b.time,8);put(out,b.value,1);}
    return out;
}
Bytes Firmware::handle(uint16_t op,uint64_t time,const Bytes &payload) {
    if(time<now || time>UINT64_MAX/2)throw std::runtime_error("time must be monotonic and <= 2^63-1 us");
    Reader r{payload};Bytes out;
    if(op==Hello) {
        r.end();
        std::string text="ELRS protocol SITL v1; role=";
#if TARGET_TX
        text+="tx";
#else
        text+="rx";
#endif
        text+="; revision=" SITL_REVISION "; pre-synchronized; no RF PHY or embedded main loop";
        return Bytes(text.begin(),text.end());
    }
    if(op==Configure) {
        if(configured || time!=0)throw std::runtime_error("configure once at time zero; restart for a clean reset");
        auto uid=r.take(6);auto rate=r.get(1);auto mode=r.get(1);auto b=r.get(4);r.end();
        if(rate>=RATE_MAX || mode>2 || b<9600 || b>3000000)throw std::runtime_error("invalid configuration");
        auto mod=get_elrs_airRateConfig(rate);
        if(mod->numOfSends!=1 || (mod->PayloadLength==8 && mode==2))
            throw std::runtime_error("DVDA and 8-byte 12ch mode are outside this proof");
        std::copy(uid.begin(),uid.end(),UID);baud=b;
        ExpressLRS_currAirRate_Modparams=mod;ExpressLRS_currAirRate_RFperfParams=get_elrs_RFperfParams(rate);
        // RC-only proof: no reserved telemetry/sync slots. This is NOT the complete link scheduler.
        ExpressLRS_currTlmDenom=1;
        OtaUpdateCrcInitFromUid();OtaUpdateSerializers(static_cast<OtaSwitchMode_e>(mode),mod->PayloadLength);
        FHSSrandomiseFHSSsequence(uidMacSeedGet());
        std::fill(std::begin(ChannelData),std::end(ChannelData),CRSF_CHANNEL_VALUE_MID);
        ChannelData[4]=CRSF_CHANNEL_VALUE_1000;
        CRSF::LinkStatistics.uplink_TX_Power=1;
        CRSF::LinkStatistics.uplink_RSSI_1=60;CRSF::LinkStatistics.uplink_Link_quality=100;
        configured=true;
        put(out,mod->interval,4);put(out,ExpressLRS_currAirRate_RFperfParams->TOA,4);
        put(out,mod->PayloadLength,1);put(out,mod->FHSShopInterval,1);return out;
    }
    if(op==Quit){r.end();return out;}
    if(!configured)throw std::runtime_error("configure first");
#if TARGET_TX
    if(op==Transmit) {
        uint32_t channels[16];for(auto &c:channels){c=r.get(2);if(c>1984)throw std::runtime_error("channel outside 0..1984");}r.end();
        uint64_t interval=ExpressLRS_currAirRate_Modparams->interval;
        if(time%interval)throw std::runtime_error("TX time must be on the configured RF slot grid");
        uint64_t slot=time/interval;
        if(haveSlot && slot<=lastSlot)throw std::runtime_error("RF slot already transmitted");
        OtaNonce=slot%256;OTA_Packet_s packet{};
        OtaPackChannelData(&packet,channels,false,ExpressLRS_currTlmDenom);OtaGeneratePacketCrc(&packet);
        now=sitl_time_us=time;lastSlot=slot;haveSlot=true;
        put(out,slot,8);put(out,frequency(slot),4);put(out,ExpressLRS_currAirRate_Modparams->PayloadLength,1);
        auto bytes=reinterpret_cast<const uint8_t*>(&packet);out.insert(out.end(),bytes,bytes+ExpressLRS_currAirRate_Modparams->PayloadLength);
        return out;
    }
#else
    if(op==Receive) {
        auto slot=r.get(8);auto freq=r.get(4);auto len=r.get(1);auto bytes=r.take(len);r.end();
        uint64_t interval=ExpressLRS_currAirRate_Modparams->interval;
        if(len!=ExpressLRS_currAirRate_Modparams->PayloadLength || slot>UINT64_MAX/interval-1)
            throw std::runtime_error("invalid OTA length/slot");
        if(time<slot*interval+ExpressLRS_currAirRate_RFperfParams->TOA)
            throw std::runtime_error("RX cannot precede completion of radio airtime");
        if(haveSlot && slot<=lastSlot)throw std::runtime_error("duplicate/out-of-order radio slot");
        if(uart.size()>4000)throw std::runtime_error("UART queue full; advance and drain before receiving more");
        OTA_Packet_s packet{};std::copy(bytes.begin(),bytes.end(),reinterpret_cast<uint8_t*>(&packet));
        OtaNonce=slot%256;
        bool accepted=freq==frequency(slot) && packet.std.type==PACKET_TYPE_RCDATA && OtaValidatePacketCrc(&packet);
        now=sitl_time_us=time;lastSlot=slot;haveSlot=true;
        if(accepted) {
            OtaUnpackChannelData(&packet,ChannelData,ExpressLRS_currTlmDenom);
            serial.bytes.clear();crsf.sendRCFrame(true,false,ChannelData);
            uint64_t start=std::max(now,uartEnd);
            for(size_t i=0;i<serial.bytes.size();++i)
                uart.push_back({start+((i+1)*10000000ULL+baud-1)/baud,serial.bytes[i]});
            uartEnd=uart.back().time;
        }
        put(out,accepted,1);return out;
    }
    if(op==Advance) {r.end();now=sitl_time_us=time;return drain();}
    if(op==TelemetryIn) {
        // Complete bounded CRSF frames only; validate before passing to firmware parser.
        if(payload.size()<4 || payload.size()>64 || payload[1]+2U!=payload.size() ||
           crsf_crc.calc(payload.data()+2,payload.size()-3)!=payload.back())
            throw std::runtime_error("invalid CRSF telemetry frame");
        now=sitl_time_us=time;
        for(auto b:payload)telemetry.RXhandleUARTin(b);
        uint8_t len=0;uint8_t data[64]{};
        if(telemetry.GetNextPayload(&len,data))out.assign(data,data+len);
        return out; // Parsed FC telemetry, not yet transmitted across an RF downlink.
    }
#endif
    throw std::runtime_error("unsupported operation for this role");
}
}
