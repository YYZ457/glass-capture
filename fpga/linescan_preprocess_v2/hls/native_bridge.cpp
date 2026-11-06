#include "preprocess_tile.h"
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
uint32_t vision_preprocess_tile(uint32_t *m,uint32_t words,uint32_t input,
    uint32_t stride,uint32_t w,uint32_t h,uint32_t x,uint32_t y,
    uint32_t cw,uint32_t ch,uint32_t r,uint32_t calibration,uint32_t output,
    uint32_t polarity,uint32_t threshold,uint32_t flags) {
    return preprocess_tile(m,words,input,stride,w,h,x,y,cw,ch,r,calibration,output,polarity,threshold,flags);
}
