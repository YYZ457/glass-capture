/* SPDX-License-Identifier: MIT
 *
 * sg_screen.h —— 第一级"快筛"：每个像素只和本列的一对阈值比较，找出命中的行内连续段（run）。
 *
 * 为什么要有它（docs/design/detection_plan.md 第 4、5 节）：
 *   15 kHz × 16384 像素 = 246 MB/s，四个 A53 核每个像素总共只有十几个时钟周期。
 *   sg_candidates 对每个像素做浮点减法 + 连通域标号，一个核跟不上。
 *   干净玻璃上 99.99% 以上的像素什么都没有，所以先用最便宜的办法把它们筛掉：
 *     暗点：像素 <  lo[x]        亮点：像素 >  hi[x]
 *   两张表每列一个字节，由背景和噪声标定得到（lo = 背景 - 余量，hi = 背景 + 余量）。
 *   ARM64 上用 NEON 一次比 16 个像素；一段 64 个像素里没有命中就整段跳过。
 *   命中的 run 很少，后面的连通域、特征、分类只处理这些 run。
 *
 * 约定：run 是半开区间 [x0, x1)，同一个 run 里极性相同（暗和亮相邻时分成两个 run）；
 *       输出按光栅顺序（从上到下、从左到右）。lo[x] = 0 表示这一列不报暗点，hi[x] = 255 表示不报亮点。
 * 纯函数，不碰硬件；通用 C 版本和 NEON 版本结果必须相同（单元测试在 x86 和 qemu-aarch64 上各跑一遍）。
 *
 * 状态：🟡 设计原型。目前只有基准测试（sg3000-capture bench）在用，还没接进 sg3000-stream。
 */
#ifndef SG_SCREEN_H
#define SG_SCREEN_H
#include <stdint.h>

struct sg_run {
    uint32_t y;
    uint16_t x0, x1;        /* [x0, x1) */
    uint8_t bright;         /* 0 = 暗点（< lo），1 = 亮点（> hi） */
};

struct sg_screen_stats {
    uint64_t dark_pixels, bright_pixels;   /* 命中的像素数（截断后仍然继续数） */
    uint64_t runs;                         /* run 总数（可能大于写出的个数） */
};

#define SG_SCREEN_TRUNCATED 1
#define SG_SCREEN_MAX_WIDTH 65535u

/* 筛一块。runs[0..*count) 写出前 capacity 个 run；更多的只计数，返回 SG_SCREEN_TRUNCATED。
 * 返回 0、SG_SCREEN_TRUNCATED 或 -EINVAL（参数不对）。 */
int sg_screen(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride,
              const uint8_t *lo, const uint8_t *hi,
              struct sg_run *runs, uint32_t capacity, uint32_t *count, struct sg_screen_stats *stats);

/* 正在用哪种实现："neon" 或 "generic"（基准测试打印用）。 */
const char *sg_screen_backend(void);

#endif
