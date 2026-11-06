#include "../hls/preprocess_tile.h"
#include <algorithm>
#include <stdint.h>
#include <iostream>
#include <vector>
static uint32_t memory[65536];
const unsigned INPUT=16, CAL=30000, OUTPUT=48000;
unsigned image_w,image_h,stride;
unsigned raw_at(unsigned x,unsigned y) {
    unsigned p=y*stride+x;
    return (memory[INPUT+p/4]>>(8*(p%4)))&255;
}
unsigned correct(unsigned x,unsigned y,bool enabled,bool&valid) {
    unsigned raw=raw_at(x,y); valid=true;
    if(!enabled) return raw;
    unsigned dark=memory[CAL+x]&255,gain=memory[CAL+x]>>8;
    if(!gain){valid=false;return 0;}
    uint64_t signal=raw>dark?raw-dark:0;
    return std::min<unsigned>(255,unsigned((signal*gain+32768)/65536));
}
uint32_t expected(unsigned x,unsigned y,unsigned radius,unsigned pol,unsigned threshold,bool enabled) {
    uint64_t sum=0;bool valid=true;
    for(int dy=-int(radius);dy<=int(radius);dy++) for(int dx=-int(radius);dx<=int(radius);dx++) {
        unsigned xx=std::min(int(image_w)-1,std::max(0,int(x)+dx));
        unsigned yy=std::min(int(image_h)-1,std::max(0,int(y)+dy));
        bool v;sum+=correct(xx,yy,enabled,v);valid=valid&&v;
    }
    unsigned area=(2*radius+1)*(2*radius+1),bg=unsigned((sum+area/2)/area);
    bool v;unsigned center=correct(x,y,enabled,v);
    int diff=valid?int(center)-int(bg):0;
    bool mask=valid&&((pol!=2&&diff>=int(threshold))||(pol!=1&&diff<=-int(threshold)));
    return center|(bg<<8)|((uint32_t(diff)&511)<<16)|(uint32_t(valid)<<25)|(uint32_t(mask)<<26);
}
void setup(unsigned w,unsigned h) {
    image_w=w;image_h=h;stride=(w+3)&~3;
    std::fill(memory,memory+65536,0xdeadbeef);
    for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) {
        unsigned p=y*stride+x,shift=8*(p%4);
        unsigned value=(x*13+y*31+(x^y))&255;
        memory[INPUT+p/4]=(memory[INPUT+p/4]&~(255U<<shift))|(value<<shift);
    }
    for(unsigned x=0;x<w;x++) memory[CAL+x]=((65536U+(x%3)*16384)<<8)|(x%11);
    memory[CAL+std::min(255U,w-1)]=0;
}
bool check(unsigned x,unsigned y,unsigned w,unsigned h,unsigned r,unsigned pol,unsigned threshold,unsigned flags) {
    std::vector<uint32_t> before(memory,memory+65536),gold;
    for(unsigned yy=0;yy<h;yy++) for(unsigned xx=0;xx<w;xx++) gold.push_back(expected(x+xx,y+yy,r,pol,threshold,flags&1));
    unsigned status=preprocess_tile(memory,65536,INPUT,stride,image_w,image_h,x,y,w,h,r,CAL,OUTPUT,pol,threshold,flags);
    if(status) return false;
    for(unsigned i=0;i<65536;i++) {
        uint32_t want=(i>=OUTPUT&&i-OUTPUT<gold.size())?gold[i-OUTPUT]:before[i];
        if(memory[i]!=want){std::cerr<<"Mismatch at word "<<i<<"\n";return false;}
    }
    return true;
}
int main(){
    setup(16384,7);
    if(!check(0,0,16,3,2,0,10,1))return 1;
    if(!check(248,2,32,3,2,1,5,1))return 2;
    if(!check(16375,4,9,3,7,2,10,1))return 3;
    if(!check(127,0,256,7,7,0,20,1))return 4;
    if(!check(255,1,2,1,0,0,1,0))return 5;
    std::vector<uint32_t> before(memory,memory+65536);
    unsigned status=preprocess_tile(memory,65536,INPUT,stride,image_w,image_h,0,0,0,1,2,CAL,OUTPUT,0,10,1);
    if(status!=PREPROCESS_BAD_DIMENSION||!std::equal(before.begin(),before.end(),memory))return 6;
    status=preprocess_tile(memory,65536,INPUT,stride,image_w,image_h,0,0,16,1,2,CAL,INPUT,0,10,1);
    if(status!=PREPROCESS_OVERLAP||!std::equal(before.begin(),before.end(),memory))return 7;
    setup(272,78);
    if(!check(8,7,256,64,7,0,18,1))return 8;
    if(!check(0,0,3,2,1,2,5,0))return 9;
    std::cout<<"PASS preprocess_tile RTL cosim: 9 sequential jobs; 16K edges and full cache\n";
    return 0;
}
