/* SPDX-License-Identifier: MIT
 *
 * cam_set.c —— camera-set 的白名单和换算。表格和出处见 cam_set.h。
 */
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cam_set.h"
#include "sg_err.h"

uint32_t cam_set_address(const char *name)
{
    static const struct { const char *name; uint32_t addr; } table[] = {
        { "ClConfiguration", CAM16K_CL_CONFIG }, { "DeviceTapGeometry", CAM16K_TAP_GEOMETRY },
        { "TestPattern", CAM16K_TEST_PATTERN }, { "ExposureTime", CAM16K_EXPOSURE_CLK },
        { "DigitalGain", CAM16K_DIGITAL_GAIN }, { "AnalogGain", CAM16K_ANALOG_GAIN },
    };
    size_t i;

    if (!name) return 0;
    for (i = 0; i < sizeof table / sizeof table[0]; ++i)
        if (!strcmp(name, table[i].name)) return table[i].addr;
    return 0;
}

static int parse_uint(const char *text, uint32_t *out)
{
    char *end;
    unsigned long v;

    if (!text || !*text || *text == '-') return -EINVAL;
    errno = 0;
    v = strtoul(text, &end, 10);
    if (errno || *end || v > 0xffffffffUL) return -EINVAL;
    *out = (uint32_t)v;
    return 0;
}

static int parse_double(const char *text, double *out)
{
    char *end;

    if (!text || !*text) return -EINVAL;
    errno = 0;
    *out = strtod(text, &end);
    if (errno || *end || !isfinite(*out)) return -EINVAL;
    return 0;
}

int cam_set_check(const char *name, const char *text, const struct cam_set_state *st,
                  uint32_t *raw, const char **why)
{
    uint32_t addr = cam_set_address(name), v;
    double d;
    const char *dummy;

    if (!why) why = &dummy;
    *why = NULL;
    if (!addr || !st || !raw) return -EINVAL;

    switch (addr) {
    case CAM16K_CL_CONFIG:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v != 0u && v != 3u) { *why = "ClConfiguration: 0 Base / 3 EightyBit"; return -EPERM; }
        break;
    case CAM16K_TAP_GEOMETRY:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v != 2u && v != 3u) { *why = "DeviceTapGeometry: 2 or 3"; return -EPERM; }
        if (st->cl_config != 0u || st->pixel_format != CAM16K_MONO8) {
            *why = "DeviceTapGeometry is locked unless the camera is Base + Mono8 (set ClConfiguration 0 first)";
            return -EPERM;
        }
        break;
    case CAM16K_TEST_PATTERN:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v > 5u || v == 4u) {
            *why = "TestPattern: 0 Off / 1 Black / 2 White / 3 GreyHorizontalRamp / 5 GreyHorizontalRampMoving";
            return -EPERM;
        }
        break;
    case CAM16K_EXPOSURE_CLK:
        if (parse_double(text, &d)) return -EINVAL;
        if (d <= 0.0 || d > 100000.0) { *why = "ExposureTime is in microseconds (0 < us <= 100000)"; return -EPERM; }
        v = (uint32_t)lround(d * CAM16K_SENSOR_MHZ);
        if (st->exposure_min_clk == 0u || st->exposure_max_clk < st->exposure_min_clk) {
            *why = "camera did not report a valid exposure range (0x01001014 / 0x01001018)";
            return -EPERM;
        }
        if (v < st->exposure_min_clk || v > st->exposure_max_clk) {
            *why = "ExposureTime outside the range the camera reports for the current line rate";
            return -EPERM;
        }
        break;
    case CAM16K_DIGITAL_GAIN:
        if (parse_double(text, &d)) return -EINVAL;
        if (d < 0.10 || d > 15.99) { *why = "DigitalGain: 0.10 .. 15.99 (times)"; return -EPERM; }
        v = (uint32_t)lround(d * 256.0);
        break;
    case CAM16K_ANALOG_GAIN:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v > 4u) { *why = "AnalogGain: 0..4 = 1.25 / 1.75 / 2.0 / 3.0 / 4.0 times"; return -EPERM; }
        if (st->binning_v == 1u || st->binning_v == 2u) {
            *why = "AnalogGain is locked by the camera while BinningV (0x01302040) is 1 or 2";
            return -EPERM;
        }
        break;
    default:
        return -EINVAL;
    }
    *raw = v;
    return 0;
}
