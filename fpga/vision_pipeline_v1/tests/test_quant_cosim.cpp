// Compact RTL co-simulation test: match m_axi depth=4096 words.
#include "../hls/quant_engine.h"
#include <stdint.h>
#include <iostream>
static uint32_t m[4096];
int main() {
    for(unsigned i=0;i<4096;i++) m[i]=0xdeadbeef;
    for(unsigned i=0;i<16;i++) m[i]=uint32_t(int(i)-8)&255;
    m[32]=2;
    // Single input/output channel 1x1 convolution, 4x4 shape.
    unsigned status=quant_engine(m,4096,1,0,0,64,32,0,0,0,1,4,4,1,1,1,0,0);
    if(status) return 1;
    for(unsigned i=0;i<16;i++) if(m[64+i]!=uint32_t((int(i)-8)*2)) return 2;
    m[128]=1; m[144]=1;
    status=quant_engine(m,4096,3,64,0,160,0,0,128,144,1,4,4,0,0,0,0,0);
    if(status) return 3;
    for(unsigned i=0;i<16;i++) if(m[160+i]!=(uint32_t(int(i)-8)&255)) return 4;
    status=quant_engine(m,4096,2,160,0,192,0,0,0,0,1,4,4,0,0,0,0,0);
    const int expected[4]={-3,-1,5,7};
    if(status) return 5;
    for(unsigned i=0;i<4;i++) if(m[192+i]!=(uint32_t(expected[i])&255)) return 6;
    // Fresh repeated invocation, stale done would give wrong destination.
    status=quant_engine(m,4096,4,64,64,224,0,0,0,0,1,4,4,0,0,0,0,0);
    if(status) return 7;
    for(unsigned i=0;i<16;i++) if(m[224+i]!=uint32_t((int(i)-8)*4)) return 8;
    // Rejection and late-channel validation must not alter output.
    status=quant_engine(m,4096,4,64,64,64,0,0,0,0,1,4,4,0,0,0,0,0);
    if(status!=QE_ERR_OVERLAP) return 9;
    for(unsigned i=0;i<16;i++) if(m[64+i]!=uint32_t((int(i)-8)*2)) return 10;
    m[144]=32;
    status=quant_engine(m,4096,3,64,0,256,0,0,128,144,1,4,4,0,0,0,0,0);
    if(status!=QE_ERR_SCALE) return 11;
    for(unsigned i=0;i<16;i++) if(m[256+i]!=0xdeadbeef) return 12;
    // Depthwise3x3: one pixel, zero padding, all weights1 => centre only.
    m[0]=uint32_t(-7)&255;
    for(unsigned i=0;i<9;i++) m[32+i]=1;
    status=quant_engine(m,4096,1,0,0,288,32,0,0,0,1,1,1,1,3,1,1,QE_DEPTHWISE);
    if(status || m[288]!=uint32_t(-7)) return 13;
    std::cout<<"PASS quant_engine RTL smoke: 7 sequential jobs\n";
    return 0;
}
