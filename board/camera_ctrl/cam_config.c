/* SPDX-License-Identifier: MIT
 *
 * cam_config.c —— cam_settings ⇄ 相机寄存器。
 * 只调 cam_serial 的 read32/write32；cam_serial 写好之前，这里每个函数都会返回 -ENOSYS。
 *
 * ❓ 地址全部来自 8K 型号 XML（cam_regs.h 里标 [参考8K]），拿到 16K 的 XML 后核对。
 * ❓ User Set 槽位号（1..3）和"写 1 触发保存"也是按 GenICam 惯例写的，待核对。
 * 怎么验证：cam_set 之后 cam_get 读回一致；打开测试图案后跑 verify/V1_testpattern.py。
 */
#include "cam_config.h"
#include "cam_regs.h"
#include "cam_serial.h"
#include "sg_err.h"

#define SENSOR_CLOCK_HZ     80000000u   /* 曝光、行周期的单位：80 MHz 时钟数 */
#define CLK_PER_US          80u
#define CAM_TIMEOUT_MS      1000u

#define MAX_WIDTH           16384u
#define MIN_LINE_RATE_HZ    100u        /* 保守范围，待手册确认 */
#define MAX_LINE_RATE_HZ    200000u
#define EXPOSURE_TIMED      1u          /* 注意：Timed 是 1，不是 0 */
#define MAX_TEST_PATTERN    6
#define USERSET_SAVE_CMD    1u

static int rd(struct sg_capture_session *s, uint32_t addr, uint32_t *v)
{
    return cam_gencp_read32(s, addr, v, CAM_TIMEOUT_MS);
}

static int wr(struct sg_capture_session *s, uint32_t addr, uint32_t v)
{
    return cam_gencp_write32(s, addr, v, CAM_TIMEOUT_MS);
}

/* 换算（四舍五入） */
static uint32_t rate_to_period_clk(uint32_t hz) { return (SENSOR_CLOCK_HZ + hz / 2) / hz; }
static uint32_t period_clk_to_rate(uint32_t clk) { return clk ? (SENSOR_CLOCK_HZ + clk / 2) / clk : 0; }

int cam_settings_check(const struct cam_settings *in)
{
    uint64_t exposure_clk;

    if (in == NULL) {
        return -EINVAL;
    }
    if (in->width == 0 || in->width > MAX_WIDTH) {
        return -EINVAL;
    }
    if (in->line_rate_hz < MIN_LINE_RATE_HZ || in->line_rate_hz > MAX_LINE_RATE_HZ) {
        return -EINVAL;
    }
    if (in->trigger != CAM_TRIG_FREE_RUN && in->trigger != CAM_TRIG_EXTERNAL) {
        return -EINVAL;
    }
    if (in->test_pattern < 0 || in->test_pattern > MAX_TEST_PATTERN) {
        return -EINVAL;
    }
    /* 曝光必须放得进一个行周期 */
    exposure_clk = (uint64_t)in->exposure_us * CLK_PER_US;
    if (in->exposure_us == 0 || exposure_clk > rate_to_period_clk(in->line_rate_hz)) {
        return -EINVAL;
    }
    return 0;
}

int cam_get(struct sg_capture_session *s, struct cam_settings *out)
{
    uint32_t width, period, exposure, trig, pattern;
    int ret;

    if (s == NULL || out == NULL) {
        return -EINVAL;
    }
    if ((ret = rd(s, CAM_REG_WIDTH, &width)) ||
        (ret = rd(s, CAM_REG_LINE_PERIOD_CLK, &period)) ||
        (ret = rd(s, CAM_REG_EXPOSURE_CLK, &exposure)) ||
        (ret = rd(s, CAM_REG_LINE_TRIG_MODE, &trig)) ||
        (ret = rd(s, CAM_REG_TEST_PATTERN, &pattern))) {
        return ret;
    }
    out->width        = width;
    out->line_rate_hz = period_clk_to_rate(period);
    out->exposure_us  = (exposure + CLK_PER_US / 2) / CLK_PER_US;
    out->trigger      = trig ? CAM_TRIG_EXTERNAL : CAM_TRIG_FREE_RUN;
    out->test_pattern = (int)pattern;
    return 0;
}

int cam_set(struct sg_capture_session *s, const struct cam_settings *in)
{
    uint32_t new_period, old_period, exposure_clk;
    int ret;

    if (s == NULL) {
        return -EINVAL;
    }
    ret = cam_settings_check(in);
    if (ret) {
        return ret;
    }
    new_period   = rate_to_period_clk(in->line_rate_hz);
    exposure_clk = in->exposure_us * CLK_PER_US;

    ret = rd(s, CAM_REG_LINE_PERIOD_CLK, &old_period);
    if (ret) {
        return ret;
    }

    if ((ret = wr(s, CAM_REG_TEST_PATTERN, (uint32_t)in->test_pattern)) ||
        (ret = wr(s, CAM_REG_LINE_TRIG_MODE, in->trigger == CAM_TRIG_EXTERNAL)) ||
        (ret = wr(s, CAM_REG_EXPOSURE_MODE, EXPOSURE_TIMED))) {
        return ret;
    }

    /* 曝光和行周期的先后：任何时刻都要满足"曝光 ≤ 行周期"。
     * 周期变长（降行频）先改周期；周期变短先改曝光。 */
    if (new_period >= old_period) {
        if ((ret = wr(s, CAM_REG_LINE_PERIOD_CLK, new_period)) ||
            (ret = wr(s, CAM_REG_EXPOSURE_CLK, exposure_clk))) {
            return ret;
        }
    } else {
        if ((ret = wr(s, CAM_REG_EXPOSURE_CLK, exposure_clk)) ||
            (ret = wr(s, CAM_REG_LINE_PERIOD_CLK, new_period))) {
            return ret;
        }
    }

    return wr(s, CAM_REG_WIDTH, in->width);
}

int cam_save_user_set(struct sg_capture_session *s, unsigned slot)
{
    int ret;

    if (s == NULL || slot < 1 || slot > 3) {
        return -EINVAL;
    }
    if ((ret = wr(s, CAM_REG_USERSET_SELECTOR, slot)) ||
        (ret = wr(s, CAM_REG_USERSET_SAVE, USERSET_SAVE_CMD))) {
        return ret;
    }
    return wr(s, CAM_REG_USERSET_DEFAULT, slot);
}
