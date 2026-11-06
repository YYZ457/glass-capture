// SPDX-License-Identifier: MIT
#include "sg_image_stats.hpp"
void sg_image_stats(const unsigned char *pixels, uint64_t *result_words,
                    uint32_t width, uint32_t height, uint32_t stride,
                    uint32_t threshold, uint64_t sequence, uint64_t first_line)
{
#pragma HLS INTERFACE m_axi port=pixels offset=slave bundle=image_memory depth=16777216
#pragma HLS INTERFACE m_axi port=result_words offset=slave bundle=result_memory depth=10
#pragma HLS INTERFACE s_axilite port=pixels bundle=control
#pragma HLS INTERFACE s_axilite port=result_words bundle=control
#pragma HLS INTERFACE s_axilite port=width bundle=control
#pragma HLS INTERFACE s_axilite port=height bundle=control
#pragma HLS INTERFACE s_axilite port=stride bundle=control
#pragma HLS INTERFACE s_axilite port=threshold bundle=control
#pragma HLS INTERFACE s_axilite port=sequence bundle=control
#pragma HLS INTERFACE s_axilite port=first_line bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control
    uint64_t sum=0, above=0, count=0;
    uint32_t lo=255, hi=0, error=0;
    if(!width||!height||width>32768||height>32768||stride<width||threshold>255||
       (uint64_t)stride*height>16777216u) error=1;
    else {
        for(uint32_t y=0;y<height;++y) {
            for(uint32_t x=0;x<width;++x) {
#pragma HLS PIPELINE II=1
                unsigned value=pixels[(uint64_t)y*stride+x];
                sum+=value; above+=value>threshold; ++count;
                if(value<lo)lo=value;
                if(value>hi)hi=value;
            }
        }
    }
    result_words[0]=UINT64_C(0x5347495000000001);
    result_words[1]=sequence; result_words[2]=first_line;
    result_words[3]=count; result_words[4]=sum; result_words[5]=above;
    result_words[6]=lo; result_words[7]=hi; result_words[8]=error; result_words[9]=1;
}
