// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
// Memory interface reference kernel. Does not assign physical addresses.
// result_words[0..9]: magic/ABI, sequence, first_line, count, sum, above,
// min, max, error_flags, algorithm. Unknown decision is handled by the driver.
void sg_image_stats(const unsigned char *pixels, uint64_t *result_words,
                    uint32_t width, uint32_t height, uint32_t stride,
                    uint32_t threshold, uint64_t sequence, uint64_t first_line);
