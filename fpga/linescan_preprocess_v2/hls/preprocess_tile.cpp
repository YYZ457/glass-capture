#include "preprocess_tile.h"

namespace {
const unsigned MAX_DIAMETER = 15;
const unsigned MAX_PADDED_WIDTH = 270;
const unsigned MAX_ROW_WORDS = 69; // ceil((270 + 3-byte alignment)/4)

bool overlaps(uint64_t a, uint64_t a_end, uint64_t b, uint64_t b_end) {
#pragma HLS INLINE
    return a < b_end && b < a_end;
}

uint32_t clamp_coordinate(int32_t value, uint32_t extent) {
#pragma HLS INLINE
    return value < 0 ? 0U :
           (static_cast<uint32_t>(value) >= extent ? extent - 1U :
            static_cast<uint32_t>(value));
}

uint16_t correct_pixel(uint32_t raw, uint32_t calibration, bool enabled) {
#pragma HLS INLINE
    if (!enabled) return static_cast<uint16_t>(0x100U | raw);
    const uint32_t gain = calibration >> 8;
    if (gain == 0) return 0;
    const uint32_t dark = calibration & 255U;
    const uint32_t delta = raw > dark ? raw - dark : 0;
    // Maximum product plus rounding fits unsigned 32 bits, even at gain 0xffffff.
    const uint32_t corrected = (delta * gain + 32768U) >> 16;
    return static_cast<uint16_t>(0x100U | (corrected > 255 ? 255 : corrected));
}
}

uint32_t preprocess_tile(
    uint32_t* memory, uint32_t memory_words, uint32_t input_offset,
    uint32_t stride_bytes, uint32_t image_width, uint32_t image_height,
    uint32_t core_x, uint32_t core_y, uint32_t core_width,
    uint32_t core_height, uint32_t radius, uint32_t calibration_offset,
    uint32_t output_offset, uint32_t polarity, uint32_t threshold,
    uint32_t flags) {
#pragma HLS INTERFACE m_axi port=memory offset=slave bundle=ddr depth=65536 max_read_burst_length=64 max_write_burst_length=64 max_widen_bitwidth=32 num_read_outstanding=2 num_write_outstanding=2
#pragma HLS INTERFACE s_axilite port=memory bundle=control offset=0x10
#pragma HLS INTERFACE s_axilite port=memory_words bundle=control offset=0x1c
#pragma HLS INTERFACE s_axilite port=input_offset bundle=control offset=0x24
#pragma HLS INTERFACE s_axilite port=stride_bytes bundle=control offset=0x2c
#pragma HLS INTERFACE s_axilite port=image_width bundle=control offset=0x34
#pragma HLS INTERFACE s_axilite port=image_height bundle=control offset=0x3c
#pragma HLS INTERFACE s_axilite port=core_x bundle=control offset=0x44
#pragma HLS INTERFACE s_axilite port=core_y bundle=control offset=0x4c
#pragma HLS INTERFACE s_axilite port=core_width bundle=control offset=0x54
#pragma HLS INTERFACE s_axilite port=core_height bundle=control offset=0x5c
#pragma HLS INTERFACE s_axilite port=radius bundle=control offset=0x64
#pragma HLS INTERFACE s_axilite port=calibration_offset bundle=control offset=0x6c
#pragma HLS INTERFACE s_axilite port=output_offset bundle=control offset=0x74
#pragma HLS INTERFACE s_axilite port=polarity bundle=control offset=0x7c
#pragma HLS INTERFACE s_axilite port=threshold bundle=control offset=0x84
#pragma HLS INTERFACE s_axilite port=flags bundle=control offset=0x8c
#pragma HLS INTERFACE s_axilite port=return bundle=control offset=0x94

    // Validate every scalar and full DDR range before loading or storing pixels.
    // All extent arithmetic is explicitly widened before multiplication/addition.
#ifndef __SYNTHESIS__
    if (memory == 0) return PREPROCESS_BAD_MEMORY;
#endif
    if (memory_words == 0 || memory_words > (1U << 29))
        return PREPROCESS_BAD_MEMORY;
    if (image_width == 0 || image_width > 16384 || image_height == 0 ||
        image_height > 65536 || core_width == 0 || core_width > 256 ||
        core_height == 0 || core_height > 64 ||
        uint64_t(core_x) + core_width > image_width ||
        uint64_t(core_y) + core_height > image_height ||
        stride_bytes < image_width || (stride_bytes & 3U) != 0)
        return PREPROCESS_BAD_DIMENSION;
    if (radius > 7 || polarity > 2 || threshold == 0 || threshold > 255 ||
        (flags & ~1U) != 0)
        return PREPROCESS_BAD_PARAMETER;

    const bool calibrated = (flags & 1U) != 0;
    const uint64_t input_end = uint64_t(input_offset) +
        uint64_t(stride_bytes / 4U) * image_height;
    const uint64_t calibration_end = uint64_t(calibration_offset) + image_width;
    const uint64_t output_end = uint64_t(output_offset) +
        uint64_t(core_width) * core_height;
    if (input_end > memory_words || output_end > memory_words ||
        (calibrated && calibration_end > memory_words))
        return PREPROCESS_BAD_RANGE;
    if (overlaps(output_offset, output_end, input_offset, input_end) ||
        (calibrated && overlaps(output_offset, output_end,
                                 calibration_offset, calibration_end)))
        return PREPROCESS_OVERLAP;

    uint16_t line_buffer[MAX_DIAMETER][MAX_PADDED_WIDTH];
#pragma HLS ARRAY_PARTITION variable=line_buffer complete dim=1
#pragma HLS BIND_STORAGE variable=line_buffer type=ram_2p impl=bram
    uint32_t row_words[MAX_ROW_WORDS];
#pragma HLS BIND_STORAGE variable=row_words type=ram_2p impl=bram
    uint32_t column_calibration[MAX_PADDED_WIDTH];
#pragma HLS BIND_STORAGE variable=column_calibration type=ram_2p impl=bram
    uint32_t output_row_cache[256];
#pragma HLS BIND_STORAGE variable=output_row_cache type=ram_2p impl=bram

    const uint32_t diameter = 2U * radius + 1U;
    const uint32_t padded_width = core_width + 2U * radius;
    const uint32_t padded_height = core_height + 2U * radius;
    const uint32_t first_x = clamp_coordinate(int32_t(core_x) - int32_t(radius), image_width);
    const uint32_t last_x = clamp_coordinate(int32_t(core_x + core_width + radius - 1U), image_width);
    const uint32_t first_word = first_x / 4U;
    const uint32_t words_per_row = last_x / 4U - first_word + 1U;
    const uint32_t area = diameter * diameter;

    if (calibrated) {
        load_calibration: for (uint32_t x = 0; x <= last_x - first_x; ++x) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=270
            column_calibration[x] = memory[uint64_t(calibration_offset) + first_x + x];
        }
    }

    uint32_t current_bank = 0;
    padded_rows: for (uint32_t py = 0; py < padded_height; ++py) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=78
        const uint32_t global_y = clamp_coordinate(int32_t(core_y + py) - int32_t(radius), image_height);
        const uint64_t row_address = uint64_t(input_offset) +
            uint64_t(global_y) * (stride_bytes / 4U) + first_word;
        // A contiguous bounded burst reads at most 69 packed DDR words per row.
        fetch_packed_row: for (uint32_t word = 0; word < words_per_row; ++word) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=69
            row_words[word] = memory[row_address + word];
        }
        cache_corrected_row: for (uint32_t px = 0; px < padded_width; ++px) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=270
            const uint32_t gx = clamp_coordinate(int32_t(core_x + px) - int32_t(radius), image_width);
            const uint32_t packed = row_words[gx / 4U - first_word];
            const uint32_t raw = (packed >> ((gx & 3U) * 8U)) & 255U;
            const uint32_t cal = calibrated ? column_calibration[gx - first_x] : 0U;
            line_buffer[current_bank][px] = correct_pixel(raw, cal, calibrated);
        }
        if (py + 1U >= diameter) {
            uint16_t sum_history[MAX_DIAMETER] = {};
            uint8_t bad_history[MAX_DIAMETER] = {};
#pragma HLS ARRAY_PARTITION variable=sum_history complete dim=1
#pragma HLS ARRAY_PARTITION variable=bad_history complete dim=1
            uint32_t horizontal_sum = 0;
            uint32_t horizontal_bad = 0;
            const uint32_t center_bank = current_bank >= radius ?
                current_bank - radius : current_bank + diameter - radius;
            const uint32_t output_row = py + 1U - diameter;
            emit_row: for (uint32_t px = 0; px < padded_width; ++px) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=270
                uint32_t vertical_sum = 0;
                uint32_t vertical_bad = 0;
                // Fixed bank indexes become parallel reads from 15 separate BRAMs.
                vertical_banks: for (unsigned bank = 0; bank < MAX_DIAMETER; ++bank) {
#pragma HLS UNROLL
                    if (bank < diameter) {
                        const uint16_t sample = line_buffer[bank][px];
                        vertical_sum += sample & 255U;
                        vertical_bad += (sample & 0x100U) == 0 ? 1U : 0U;
                    }
                }
                horizontal_sum = horizontal_sum + vertical_sum - sum_history[diameter - 1U];
                horizontal_bad = horizontal_bad + vertical_bad - bad_history[diameter - 1U];
                shift_history: for (unsigned h = MAX_DIAMETER - 1; h > 0; --h) {
#pragma HLS UNROLL
                    sum_history[h] = sum_history[h - 1];
                    bad_history[h] = bad_history[h - 1];
                }
                sum_history[0] = static_cast<uint16_t>(vertical_sum);
                bad_history[0] = static_cast<uint8_t>(vertical_bad);
                if (px + 1U >= diameter) {
                    const uint32_t corrected = line_buffer[center_bank][px - radius] & 255U;
                    // Round exactly once after the full 2D sum, never after a row mean.
                    const uint32_t background = (horizontal_sum + area / 2U) / area;
                    const bool valid = horizontal_bad == 0;
                    const int32_t difference = valid ? int32_t(corrected) - int32_t(background) : 0;
                    const bool pass = valid &&
                        ((polarity != 2 && difference >= int32_t(threshold)) ||
                         (polarity != 1 && difference <= -int32_t(threshold)));
                    const uint32_t result = corrected | (background << 8) |
                        ((uint32_t(difference) & 0x1ffU) << 16) |
                        (uint32_t(valid) << 25) | (uint32_t(pass) << 26);
                    output_row_cache[px + 1U - diameter] = result;
                }
            }
            // Isolate conditional window warm-up from the external write loop.
            // This contiguous loop permits real AXI bursts instead of one
            // external write transaction per output pixel.
            const uint64_t destination = uint64_t(output_offset) + uint64_t(output_row) * core_width;
            store_core_row: for (uint32_t x = 0; x < core_width; ++x) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=1 max=256
                memory[destination + x] = output_row_cache[x];
            }
        }
        current_bank = current_bank + 1U == diameter ? 0U : current_bank + 1U;
    }
    return PREPROCESS_OK;
}
