#include "quant_engine.h"

namespace {
const int64_t I32_MIN_VALUE = -2147483647LL - 1;
const int64_t I32_MAX_VALUE = 2147483647LL;

int64_t read_i32(uint32_t bits) {
    return bits < 0x80000000U ? int64_t(bits) : int64_t(bits) - 4294967296LL;
}
int32_t read_i8(uint32_t bits) {
    uint32_t b = bits & 255U;
    return b < 128U ? int32_t(b) : int32_t(b) - 256;
}
uint32_t clip_i32(int64_t value) {
    if (value > I32_MAX_VALUE) value = I32_MAX_VALUE;
    if (value < I32_MIN_VALUE) value = I32_MIN_VALUE;
    return uint32_t(value);
}
bool valid_range(uint32_t offset, uint64_t count, uint32_t words) {
    return uint64_t(offset) + count <= uint64_t(words);
}
bool overlap(uint32_t a, uint64_t na, uint32_t b, uint64_t nb) {
    return uint64_t(a) < uint64_t(b) + nb && uint64_t(b) < uint64_t(a) + na;
}
}

uint32_t quant_engine(
    uint32_t *memory, uint32_t memory_words, uint32_t opcode,
    uint32_t input_offset, uint32_t input2_offset, uint32_t output_offset,
    uint32_t weights_offset, uint32_t bias_offset,
    uint32_t multiplier_offset, uint32_t shift_offset,
    uint32_t channels_in, uint32_t height, uint32_t width,
    uint32_t channels_out, uint32_t kernel, uint32_t stride,
    uint32_t pad, uint32_t flags) {
#pragma HLS INTERFACE m_axi port=memory offset=slave bundle=gmem depth=4096 max_read_burst_length=16 max_write_burst_length=16 num_read_outstanding=2 num_write_outstanding=2 max_widen_bitwidth=32
#pragma HLS INTERFACE s_axilite port=memory bundle=ctrl offset=0x10
#pragma HLS INTERFACE s_axilite port=memory_words bundle=ctrl offset=0x1c
#pragma HLS INTERFACE s_axilite port=opcode bundle=ctrl offset=0x24
#pragma HLS INTERFACE s_axilite port=input_offset bundle=ctrl offset=0x2c
#pragma HLS INTERFACE s_axilite port=input2_offset bundle=ctrl offset=0x34
#pragma HLS INTERFACE s_axilite port=output_offset bundle=ctrl offset=0x3c
#pragma HLS INTERFACE s_axilite port=weights_offset bundle=ctrl offset=0x44
#pragma HLS INTERFACE s_axilite port=bias_offset bundle=ctrl offset=0x4c
#pragma HLS INTERFACE s_axilite port=multiplier_offset bundle=ctrl offset=0x54
#pragma HLS INTERFACE s_axilite port=shift_offset bundle=ctrl offset=0x5c
#pragma HLS INTERFACE s_axilite port=channels_in bundle=ctrl offset=0x64
#pragma HLS INTERFACE s_axilite port=height bundle=ctrl offset=0x6c
#pragma HLS INTERFACE s_axilite port=width bundle=ctrl offset=0x74
#pragma HLS INTERFACE s_axilite port=channels_out bundle=ctrl offset=0x7c
#pragma HLS INTERFACE s_axilite port=kernel bundle=ctrl offset=0x84
#pragma HLS INTERFACE s_axilite port=stride bundle=ctrl offset=0x8c
#pragma HLS INTERFACE s_axilite port=pad bundle=ctrl offset=0x94
#pragma HLS INTERFACE s_axilite port=flags bundle=ctrl offset=0x9c
#pragma HLS INTERFACE s_axilite port=return bundle=ctrl offset=0xa4

    if (opcode < QE_CONV || opcode > QE_ADD_SAT) return QE_ERR_OPCODE;
    // An m_axi pointer denotes a physical AXI base register. A host C pointer
    // check has no useful hardware meaning: address zero can be a mapped bus
    // address, and the master cannot establish whether any address is mapped.
#ifndef __SYNTHESIS__
    if (memory == 0) return QE_ERR_MEMORY;
#endif
    if (memory_words == 0 || memory_words > (1U << 24))
        return QE_ERR_MEMORY;
    if (channels_in == 0 || channels_in > 64 || height == 0 || height > 128 ||
        width == 0 || width > 128) return QE_ERR_DIMENSION;

    uint32_t out_h = height, out_w = width, out_c = channels_in;
    uint64_t weights_n = 0;
    bool use_bias = false;
    if (opcode == QE_CONV) {
        if (channels_out == 0 || channels_out > 64) return QE_ERR_DIMENSION;
        if ((kernel != 1 && kernel != 3) || (stride != 1 && stride != 2) ||
            pad > 1 || (flags & ~(QE_DEPTHWISE | QE_BIAS)) != 0)
            return QE_ERR_PARAMETER;
        if ((flags & QE_DEPTHWISE) && channels_out != channels_in)
            return QE_ERR_DIMENSION;
        if (height + 2 * pad < kernel || width + 2 * pad < kernel)
            return QE_ERR_DIMENSION;
        out_h = (height + 2 * pad - kernel) / stride + 1;
        out_w = (width + 2 * pad - kernel) / stride + 1;
        out_c = channels_out;
        if (out_h > 128 || out_w > 128) return QE_ERR_DIMENSION;
        weights_n = uint64_t(out_c) * ((flags & QE_DEPTHWISE) ? 1 : channels_in)
                    * kernel * kernel;
        use_bias = (flags & QE_BIAS) != 0;
    } else {
        uint32_t allowed_flags = opcode == QE_REQUANT ? QE_RELU : 0;
        if (channels_out || kernel || stride || pad || (flags & ~allowed_flags))
            return QE_ERR_PARAMETER;
        if (opcode == QE_MAXPOOL) {
            if (height < 2 || width < 2) return QE_ERR_DIMENSION;
            out_h = height / 2;
            out_w = width / 2;
        }
    }
    uint64_t in_n = uint64_t(channels_in) * height * width;
    uint64_t out_n = uint64_t(out_c) * out_h * out_w;
    if (!valid_range(input_offset, in_n, memory_words) ||
        !valid_range(output_offset, out_n, memory_words)) return QE_ERR_RANGE;
    if (overlap(input_offset, in_n, output_offset, out_n)) return QE_ERR_OVERLAP;
    if (opcode == QE_CONV) {
        if (!valid_range(weights_offset, weights_n, memory_words) ||
            (use_bias && !valid_range(bias_offset, out_c, memory_words)))
            return QE_ERR_RANGE;
        if (overlap(output_offset, out_n, weights_offset, weights_n) ||
            (use_bias && overlap(output_offset, out_n, bias_offset, out_c)))
            return QE_ERR_OVERLAP;
    }
    if (opcode == QE_ADD_SAT) {
        if (!valid_range(input2_offset, in_n, memory_words)) return QE_ERR_RANGE;
        if (overlap(output_offset, out_n, input2_offset, in_n)) return QE_ERR_OVERLAP;
    }
    uint32_t multipliers[64], shifts[64];
    if (opcode == QE_REQUANT) {
        if (!valid_range(multiplier_offset, channels_in, memory_words) ||
            !valid_range(shift_offset, channels_in, memory_words)) return QE_ERR_RANGE;
        if (overlap(output_offset, out_n, multiplier_offset, channels_in) ||
            overlap(output_offset, out_n, shift_offset, channels_in))
            return QE_ERR_OVERLAP;
        // Validate all scales first, including channels whose data comes later.
        for (uint32_t c = 0; c < channels_in; ++c) {
            multipliers[c] = memory[multiplier_offset + c];
            shifts[c] = memory[shift_offset + c];
            if (multipliers[c] > 2147483647U || shifts[c] > 31)
                return QE_ERR_SCALE;
        }
    }

    if (opcode == QE_CONV) {
        const bool depthwise = (flags & QE_DEPTHWISE) != 0;
        for (uint32_t oc = 0; oc < out_c; ++oc) {
            for (uint32_t oy = 0; oy < out_h; ++oy) {
                for (uint32_t ox = 0; ox < out_w; ++ox) {
                    int64_t sum = use_bias ? read_i32(memory[bias_offset + oc]) : 0;
                    uint32_t count_c = depthwise ? 1 : channels_in;
                    for (uint32_t kc = 0; kc < count_c; ++kc) {
                        uint32_t ic = depthwise ? oc : kc;
                        for (uint32_t ky = 0; ky < kernel; ++ky) {
                            for (uint32_t kx = 0; kx < kernel; ++kx) {
                                int32_t iy = int32_t(oy * stride + ky) - int32_t(pad);
                                int32_t ix = int32_t(ox * stride + kx) - int32_t(pad);
                                if (iy >= 0 && ix >= 0 && iy < int32_t(height) &&
                                    ix < int32_t(width)) {
                                    uint32_t in_index = (ic * height + uint32_t(iy)) * width + uint32_t(ix);
                                    uint32_t w_index = ((oc * count_c + kc) * kernel + ky) * kernel + kx;
                                    sum += int64_t(read_i8(memory[input_offset + in_index])) *
                                           read_i8(memory[weights_offset + w_index]);
                                }
                            }
                        }
                    }
                    memory[output_offset + (oc * out_h + oy) * out_w + ox] = clip_i32(sum);
                }
            }
        }
    } else if (opcode == QE_MAXPOOL) {
        for (uint32_t c = 0; c < channels_in; ++c) {
            for (uint32_t y = 0; y < out_h; ++y) {
                for (uint32_t x = 0; x < out_w; ++x) {
                    // Spell the fixed 2x2 window explicitly. The 2022.1 Windows
                    // LLVM front end crashes optimizing nested tap loops here.
                    uint32_t base = input_offset + (c * height + 2 * y) * width + 2 * x;
                    int32_t best = read_i8(memory[base]);
                    int32_t v01 = read_i8(memory[base + 1]);
                    int32_t v10 = read_i8(memory[base + width]);
                    int32_t v11 = read_i8(memory[base + width + 1]);
                    if (v01 > best) best = v01;
                    if (v10 > best) best = v10;
                    if (v11 > best) best = v11;
                    memory[output_offset + (c * out_h + y) * out_w + x] = uint32_t(best) & 255U;
                }
            }
        }
    } else if (opcode == QE_REQUANT) {
        uint32_t plane = height * width;
        for (uint32_t c = 0; c < channels_in; ++c) {
            for (uint32_t i = 0; i < plane; ++i) {
#pragma HLS PIPELINE
                int64_t product = read_i32(memory[input_offset + c * plane + i]) *
                                  int64_t(multipliers[c]);
                uint64_t magnitude = product < 0 ? uint64_t(-product) : uint64_t(product);
                uint32_t shift = shifts[c];
                uint64_t rounded = shift == 0 ? magnitude :
                    (magnitude + (uint64_t(1) << (shift - 1))) >> shift;
                int64_t result = product < 0 ? -int64_t(rounded) : int64_t(rounded);
                if (result < -128) result = -128;
                if (result > 127) result = 127;
                if ((flags & QE_RELU) && result < 0) result = 0;
                memory[output_offset + c * plane + i] = uint32_t(result) & 255U;
            }
        }
    } else {
        for (uint32_t i = 0; i < uint32_t(in_n); ++i) {
#pragma HLS PIPELINE
            int64_t sum = read_i32(memory[input_offset + i]) + read_i32(memory[input2_offset + i]);
            memory[output_offset + i] = clip_i32(sum);
        }
    }
    return QE_OK;
}
