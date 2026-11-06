#include "quant_engine.h"
// CPU-only stable C ABI. Never add this wrapper as a second HLS top.
extern "C"
#ifdef _WIN32
__declspec(dllexport)
#endif
uint32_t vision_quant_engine(uint32_t *m, uint32_t n, uint32_t op,
    uint32_t in, uint32_t in2, uint32_t out, uint32_t w, uint32_t b,
    uint32_t mult, uint32_t shift, uint32_t ci, uint32_t h, uint32_t width,
    uint32_t co, uint32_t k, uint32_t stride, uint32_t pad, uint32_t flags) {
    return quant_engine(m,n,op,in,in2,out,w,b,mult,shift,ci,h,width,co,k,stride,pad,flags);
}
