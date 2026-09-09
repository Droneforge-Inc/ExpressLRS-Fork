/* Host probe for Betaflight's production CRSF parser. GPL-3.0-or-later.
 * Takes time_us byte pairs on stdin, writes time_us and 16 decoded raw channels.
 * No firmware process, model, or hardware is modified or started.
 */
#include "platform.h"
#include "common/time.h"
#include "rx/rx.h"
#include "rx/crsf.h"
#include <inttypes.h>
#include <stdio.h>
extern void crsfDataReceive(uint16_t c, void *data);
extern uint8_t crsfFrameStatus(rxRuntimeState_t *state);
extern uint32_t crsfChannelData[CRSF_MAX_CHANNEL];
static uint32_t clock_us;
uint32_t micros(void) {return clock_us;}
uint32_t microsISR(void) {return clock_us;}
int main(void) {
    rxRuntimeState_t state={0};uint64_t t;unsigned byte;unsigned frames=0;
    while(scanf("%" SCNu64 " %u",&t,&byte)==2) {
        if(byte>255)return 2;
        clock_us=(uint32_t)t;crsfDataReceive(byte,&state);
        if(crsfFrameStatus(&state)&RX_FRAME_COMPLETE) {
            printf("%" PRIu64,t);
            for(unsigned i=0;i<16;++i)printf(" %u",crsfChannelData[i]);
            putchar('\n');++frames;
        }
    }
    return frames?0:3;
}
