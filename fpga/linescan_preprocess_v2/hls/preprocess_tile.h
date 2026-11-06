#ifndef LINESCAN_PREPROCESS_TILE_H
#define LINESCAN_PREPROCESS_TILE_H

#include <stdint.h>

enum PreprocessTileStatus {
    PREPROCESS_OK = 0,
    PREPROCESS_BAD_MEMORY = 1,
    PREPROCESS_BAD_DIMENSION = 2,
    PREPROCESS_BAD_PARAMETER = 3,
    PREPROCESS_BAD_RANGE = 4,
    PREPROCESS_OVERLAP = 5
};

// All offsets are uint32 word indexes into the same DDR allocation.
// The input occupies stride_bytes * image_height bytes, including row padding.
// With calibration disabled calibration_offset is ignored. With it enabled,
// image_width words are required. Output must overlap neither occupied region.
// On any nonzero status memory is unchanged. The caller supplies at least
// memory_words actual words; depth=65536 is only an RTL cosimulation hint.
uint32_t preprocess_tile(
    uint32_t* memory, uint32_t memory_words, uint32_t input_offset,
    uint32_t stride_bytes, uint32_t image_width, uint32_t image_height,
    uint32_t core_x, uint32_t core_y, uint32_t core_width,
    uint32_t core_height, uint32_t radius, uint32_t calibration_offset,
    uint32_t output_offset, uint32_t polarity, uint32_t threshold,
    uint32_t flags);

#endif
