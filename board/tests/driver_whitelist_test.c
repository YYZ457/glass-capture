/* SPDX-License-Identifier: MIT
 *
 * driver_whitelist_test.c —— 在电脑上测驱动白名单（板子内核源码不在这里，.ko 编不了，
 * 但白名单逻辑可以单独编译）。Makefile 把 sg3000_rx.c 里 WHITELIST-BEGIN..END 抽成
 * out/whitelist.inc 再包含进来。运行：make -C board test
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define U32_MAX 0xffffffffu
#include "whitelist.inc"

static int ok(u32 off, u32 v) { return sg_reg_allowed(off, true) && sg_reg_value_allowed(off, v); }

int main(void)
{
    /* 默认（cam_serial=0）：和旧驱动一模一样 */
    assert(ok(0x412010, 123) && ok(0x412014, 28) && !ok(0x412014, 29));
    assert(ok(0x201000, 2) && !ok(0x201000, 1) && ok(0x202000, U32_MAX));
    assert(sg_reg_allowed(0x41201c, false) && sg_reg_allowed(0x20206c, false) && !sg_reg_allowed(0x202070, false));
    assert(!sg_reg_allowed(0x411010, true) && !sg_reg_allowed(0x41101c, false) && !sg_reg_allowed(0x100c, false));
    assert(!sg_reg_allowed(0x412011, false));

    cam_serial = true;
    assert(ok(0x411010, 0xdeadbeef));
    assert(ok(0x411014, 17) && ok(0x411014, 528) && !ok(0x411014, 16) && !ok(0x411014, 529));
    assert(ok(0x411004, 0x04000000) && !ok(0x411004, 1));
    assert(ok(0x411008, 0xa5) && ok(0x411018, 0xa5) && ok(0x411028, 0xa5) && !ok(0x411028, 0));
    assert(!sg_reg_allowed(0x41100c, true) && !sg_reg_allowed(0x411020, true) && !sg_reg_allowed(0x41102c, true));
    assert(sg_reg_allowed(0x41101c, false) && sg_reg_allowed(0x411024, false) && !sg_reg_allowed(0x411010, false));
    assert(ok(0x1000, 0xff) && ok(0x1004, 0) && ok(0x100c, 0x83) && !ok(0x1000, 0x100));
    assert(!sg_reg_allowed(0x1008, true) && !sg_reg_allowed(0x1010, true) && !sg_reg_allowed(0x1000, false));
    assert(sg_reg_allowed(0x100c, false));
    /* 端口 1 一律不放开：单路卡上碰它会 PCIe Slave error（2026-11-14 实板）*/
    assert(!sg_reg_allowed(0x413004, false) && !sg_reg_allowed(0x413010, true) && !sg_reg_allowed(0x41301c, false));
    assert(!sg_reg_allowed(0x11000, true) && !sg_reg_allowed(0x1100c, false) && !sg_reg_allowed(0x1100c, true));
    assert(!sg_reg_allowed(0x2000, true) && !sg_reg_allowed(0x12000, true));   /* XDMA 不在 BAR0，也不放开 */
    assert(ok(0x412014, 32) && !ok(0x412014, 40));                            /* 卡邮箱规则不变 */
    puts("driver whitelist ok");
    return 0;
}
