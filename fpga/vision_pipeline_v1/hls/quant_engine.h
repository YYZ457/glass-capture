#ifndef VISION_PIPELINE_V1_QUANT_ENGINE_H
#define VISION_PIPELINE_V1_QUANT_ENGINE_H

#include <stdint.h>

enum QuantEngineOpcode {
    QE_CONV = 1, QE_MAXPOOL = 2, QE_REQUANT = 3, QE_ADD_SAT = 4
};
enum QuantEngineFlags {
    QE_DEPTHWISE = 1, QE_RELU = 2, QE_BIAS = 4
};
enum QuantEngineStatus {
    QE_OK = 0, QE_ERR_OPCODE = 1, QE_ERR_MEMORY = 2,
    QE_ERR_DIMENSION = 3, QE_ERR_PARAMETER = 4,
    QE_ERR_RANGE = 5, QE_ERR_OVERLAP = 6, QE_ERR_SCALE = 7
};

/* Offline, model-independent operator primitive; no model or board is assumed.
 *
 * One ap_ctrl_hs transaction, one 32-bit m_axi gmem master, AXI-Lite ctrl.
 * memory_words is in [1, 2^24]. Every offset is a 32-bit WORD index, never a
 * byte address. The caller must supply a real allocation of memory_words words;
 * software cannot discover the physical allocation size or bus validity.
 * The HLS m_axi depth=4096 annotation is only the RTL cosimulation allocation
 * hint, not a hardware capacity bound; dedicated cosim benches must use <=4096
 * words, while native simulation and hardware may use memory_words up to 2^24.
 * Null host pointers are rejected in C simulation; hardware cannot validate
 * its configured physical memory base address.
 * Each INT8 element occupies one word: only low 8 bits are read, two's
 * complement; INT8 writes are zero-extended low-byte bit patterns (0..255).
 * INT32 elements/bias use all 32 bits as two's-complement bit patterns.
 * Tensor layout is CHW. Dimensions: C in [1,64], H/W in [1,128]. Output H/W
 * must also be <=128. All used ranges are bounds checked using 64-bit sizes.
 * Output must not overlap ANY used read range (inputs, weights, bias, scales).
 * Read ranges may overlap one another. Every parameter, range, alias and scale
 * is checked before any write. On nonzero status memory is unchanged.
 *
 * CONV: INT8 input, INT8 weights, INT32 output. channels_out in [1,64]; kernel
 * 1 or 3, stride 1 or 2, pad 0 or 1; output H/W=floor((H/W+2*pad-k)/stride)+1.
 * Padding is mathematical zero; no quantization zero point is applied.
 * Dense weights OIHW. QE_DEPTHWISE means channels_out==channels_in, channel
 * multiplier 1, weights C1HW. QE_BIAS adds one INT32 bias per output channel.
 * Complete sum (all input channels/taps plus optional bias) is INT64, clipped
 * to INT32 once at the end. No accumulate-existing or implicit activation.
 * Allowed flags QE_DEPTHWISE|QE_BIAS. input2/multiplier/shift offsets unused.
 *
 * MAXPOOL: INT8 CHW 2x2, stride 2, output H/W=floor(H/W / 2); requires H/W>=2.
 * Odd final rows/columns are dropped. channels_out/kernel/stride/pad/flags=0.
 * All offsets except input/output are unused.
 *
 * REQUANT: INT32 CHW to INT8 CHW, same dimensions. Per-channel multiplier
 * word is unsigned in [0,INT32_MAX], shift word in [0,31]. Computes
 * round_away_on_tie(input * multiplier / 2^shift), saturates [-128,127],
 * then optional QE_RELU clamps negatives to zero. No signed right-shift or
 * intermediate INT32 clipping. Allowed flags QE_RELU.
 * channels_out/kernel/stride/pad=0; input2/weights/bias offsets unused.
 *
 * ADD_SAT: elementwise INT32 CHW input + input2, INT64 intermediate, final
 * saturation to INT32. channels_out/kernel/stride/pad/flags=0. All parameter
 * offsets except input/input2/output are unused.
 *
 * Unused offsets are ignored; unused non-offset scalar parameters must be 0.
 * No persistent data: repeated calls are independent. No performance guarantee
 * is implied; this scalar reference architecture prioritizes defined semantics.
 *
 * AXI-Lite byte map: control 0x00; memory base low/high 0x10/0x14;
 * memory_words 0x1c; opcode 0x24; input_offset 0x2c; input2_offset 0x34;
 * output_offset 0x3c; weights_offset 0x44; bias_offset 0x4c;
 * multiplier_offset 0x54; shift_offset 0x5c; channels_in 0x64;
 * height 0x6c; width 0x74; channels_out 0x7c; kernel 0x84; stride 0x8c;
 * pad 0x94; flags 0x9c; return status 0xa4. Verify generated driver/XML map
 * after HLS export; software integration must use that generated artifact.
 */
uint32_t quant_engine(
    uint32_t *memory, uint32_t memory_words, uint32_t opcode,
    uint32_t input_offset, uint32_t input2_offset, uint32_t output_offset,
    uint32_t weights_offset, uint32_t bias_offset,
    uint32_t multiplier_offset, uint32_t shift_offset,
    uint32_t channels_in, uint32_t height, uint32_t width,
    uint32_t channels_out, uint32_t kernel, uint32_t stride,
    uint32_t pad, uint32_t flags);

#endif
