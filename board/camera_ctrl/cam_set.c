/* SPDX-License-Identifier: MIT
 *
 * cam_set.c —— camera-set 的白名单和换算。表格和出处见 cam_set.h。
 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
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
        { "LineRate", CAM16K_LINE_PERIOD_CLK },
        { "DigitalShift", CAM16K_DIGITAL_SHIFT }, { "TDIStage", CAM16K_TDI_STAGE }, { "TDIMode", CAM16K_TDI_MODE },
        { "BlackLevel", CAM16K_BLACK_LEVEL }, { "FFCEnable", CAM16K_FFC_ENABLE },
        { "FFCUserSetSelector", CAM16K_FFC_USER_SET }, { "FFCSampleFrames", CAM16K_FFC_SAMPLE_FRAMES },
        { "FFCCalTargetMode", CAM16K_FFC_TARGET_MODE }, { "FFCCalTargetValue", CAM16K_FFC_TARGET_VALUE },
        { "FFCCalFPN", CAM16K_FFC_CAL_FPN }, { "FFCCalPRNU", CAM16K_FFC_CAL_PRNU },
        { "FFCUserSetLoad", CAM16K_FFC_USER_SET_LOAD },
    };
    size_t i;

    if (!name) return 0;
    for (i = 0; i < sizeof table / sizeof table[0]; ++i)
        if (!strcmp(name, table[i].name)) return table[i].addr;
    return 0;
}

const uint32_t cam_set_state_addr[CAM_SET_STATE_COUNT] = {
    CAM16K_CL_CONFIG, CAM16K_PIXEL_FORMAT, CAM16K_BINNING_V, CAM16K_EXPOSURE_MIN_CLK, CAM16K_EXPOSURE_MAX_CLK,
    CAM16K_LINE_PERIOD_MIN, CAM16K_LINE_PERIOD_MAX, CAM16K_LINE_TRIGGER_MODE,
    CAM16K_FFC_FACTORY, CAM16K_FFC_USER_SET, CAM16K_REVERSE_X, CAM16K_FFC_TARGET_MODE,
    CAM16K_BLACK_LEVEL_MIN, CAM16K_BLACK_LEVEL_MAX,
};

int cam_set_is_command(const char *name)
{
    uint32_t addr = cam_set_address(name);

    return addr == CAM16K_FFC_CAL_FPN || addr == CAM16K_FFC_CAL_PRNU || addr == CAM16K_FFC_USER_SET_LOAD;
}

int cam_set_ffc_cal_locked(const struct cam_set_state *st)
{
    return (st->ffc_factory == 0u && st->ffc_user_set == 0u) || st->reverse_x == 1u;
}

static int parse_int(const char *text, int32_t *out)
{
    char *end;
    long v;

    if (!text || !*text) return -EINVAL;
    errno = 0;
    v = strtol(text, &end, 10);
    if (errno || *end || v < INT32_MIN || v > INT32_MAX) return -EINVAL;
    *out = (int32_t)v;
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
    case CAM16K_LINE_PERIOD_CLK:
        if (parse_double(text, &d)) return -EINVAL;
        if (d < 100.0 || d > 200000.0) { *why = "LineRate is in Hz (100 .. 200000)"; return -EPERM; }
        if (st->line_trigger_mode != 0u) {
            *why = "LineRate is locked while LineTriggerMode (0x01001004) is on";
            return -EPERM;
        }
        v = (uint32_t)lround(CAM16K_SENSOR_MHZ * 1e6 / d);
        if (st->line_period_min_clk == 0u || st->line_period_max_clk < st->line_period_min_clk) {
            *why = "camera did not report a valid line-period range (0x01001020 / 0x01001024)";
            return -EPERM;
        }
        if (v < st->line_period_min_clk || v > st->line_period_max_clk) {
            *why = "LineRate outside what the camera allows for the current width / CL mode";
            return -EPERM;
        }
        break;
    case CAM16K_DIGITAL_SHIFT:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v > 4u) { *why = "DigitalShift: 0..4 = x1 / x2 / x4 / x8 / x16"; return -EPERM; }
        break;
    case CAM16K_TDI_STAGE:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v > 1u) { *why = "TDIStage: 0 = 1 stage / 1 = 2 stages"; return -EPERM; }
        break;
    case CAM16K_TDI_MODE:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v > 1u) { *why = "TDIMode: 0 Sum / 1 Average"; return -EPERM; }
        break;
    case CAM16K_BLACK_LEVEL: {
        int32_t level;

        if (parse_int(text, &level)) return -EINVAL;
        if (st->black_max < st->black_min) {
            *why = "camera did not report a valid black level range (0x0120c058 / 0x0120c05c)";
            return -EPERM;
        }
        if (level < st->black_min || level > st->black_max) {
            *why = "BlackLevel outside the range the camera reports";
            return -EPERM;
        }
        v = (uint32_t)level;
        break;
    }
    case CAM16K_FFC_ENABLE:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v > 1u) { *why = "FFCEnable: 0 off / 1 on"; return -EPERM; }
        break;
    case CAM16K_FFC_USER_SET:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v > 3u) { *why = "FFCUserSetSelector: 0 Default / 1..3 UserSet"; return -EPERM; }
        break;
    case CAM16K_FFC_SAMPLE_FRAMES:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v != 1024u && v != 2048u) { *why = "FFCSampleFrames: 1024 or 2048"; return -EPERM; }
        break;
    case CAM16K_FFC_TARGET_MODE:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v > 2u) { *why = "FFCCalTargetMode: 0 MAX / 1 Average / 2 Target"; return -EPERM; }
        break;
    case CAM16K_FFC_TARGET_VALUE:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v > 250u) { *why = "FFCCalTargetValue: 0..250 (grey level)"; return -EPERM; }
        if (st->ffc_target_mode != 2u) {
            *why = "FFCCalTargetValue is locked unless FFCCalTargetMode is 2 (Target)";
            return -EPERM;
        }
        v *= 16u;
        break;
    case CAM16K_FFC_CAL_FPN:
    case CAM16K_FFC_CAL_PRNU:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v != 1u) { *why = "this is a command: the only value is 1"; return -EPERM; }
        if (cam_set_ffc_cal_locked(st)) {
            *why = "FFC calibration is locked: set FFCUserSetSelector to 1..3 first (and ReverseX must be off)";
            return -EPERM;
        }
        break;
    case CAM16K_FFC_USER_SET_LOAD:
        if (parse_uint(text, &v)) return -EINVAL;
        if (v != 1u) { *why = "this is a command: the only value is 1"; return -EPERM; }
        break;
    default:
        return -EINVAL;
    }
    *raw = v;
    return 0;
}
