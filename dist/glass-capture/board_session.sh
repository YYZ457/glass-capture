#!/bin/sh
# SPDX-License-Identifier: MIT
#
# board_session.sh —— 下次上板一条命令跑完 BRINGUP 第 5⅞ 节（测试图 + 调亮）和第 5⁹ 节（DMA 吞吐 + 15 kHz）。
#
# 用法（板上，先做过中控台「02 从 SD 加载程序」）：
#   sh /tmp/sg3000-board/board_session.sh [--seconds 30] [--exposures "50 90"] [--skip-15k]
# 或者中控台按钮「S 一键上板测试」（跑完自动把结果包取回电脑）。
#
# 前提：相机、采集卡上电；只接第一根 Camera Link 线；不需要网线。
#
# 会改的东西（全部断电即恢复，不写任何"保存"寄存器）：
#   驱动：rmmod 后按 cam_serial=1（以及 dma_fast=1）重新 insmod
#   相机：ClConfiguration=0、DeviceTapGeometry=3、TestPattern、ExposureTime、AnalogGain、LineRate（camera-set 白名单内）
#   采集卡：CameraWidth=16384（card-set 白名单内）
# 从不写：相机 UserSetSave；采集卡 DeviceReboot / ManualFPGAReload。
#
# 结果：/tmp/sg3000-session-<时间>/ 下的 session.log、各图缩略图；打包成同名 .tar.gz（几百 KB，能经串口取回）。
# 大图（每张 32 MB）留在板上 /tmp（内存盘），断电即丢。
set -u

B=${SG_BOARD_DIR:-/tmp/sg3000-board}
APP=$B/sg3000-capture
STREAM=$B/sg3000-stream
STATUS=$B/sg_status
KO=$B/sg3000_rx.ko
PCI=${SG_PCI_DIR:-/sys/bus/pci/devices/0000:01:00.0}
DEV=${SG_DEV_NODE:-/dev/sg3000_rx_0000:01:00.0}
PARAMS=${SG_PARAMS_DIR:-/sys/module/sg3000_rx/parameters}
RUN_SECONDS=30
EXPOSURES="50 90"
DO_15K=1

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) RUN_SECONDS=$2; shift 2 ;;
        --exposures) EXPOSURES=$2; shift 2 ;;
        --skip-15k) DO_15K=0; shift ;;
        *) echo "usage: $0 [--seconds N] [--exposures \"50 90\"] [--skip-15k]"; exit 2 ;;
    esac
done

STAMP=$(date +%Y%m%d-%H%M%S)
OUT=/tmp/sg3000-session-$STAMP
[ -e "$OUT" ] && OUT=$OUT-$$           # 同一秒跑两次也不混在一起（capture 不覆盖已有文件）
mkdir -p "$OUT" || exit 1
LOG=$OUT/session.log
FAILS=0
FAST=0

say() { echo "$*" | tee -a "$LOG"; }
# 跑一条命令：完整输出进日志，终端只显示 OK / FAIL
run() { echo "\$ $*" >> "$LOG"; "$@" >> "$LOG" 2>&1; rc=$?; echo "[rc=$rc]" >> "$LOG"; return $rc; }
check() {
    name=$1; shift
    if run "$@"; then say "  OK    $name"; return 0; fi
    say "  FAIL  $name   (details in session.log)"; FAILS=$((FAILS + 1)); return 1
}
# 日志里最后一行匹配 $1 的内容
last() { grep "$1" "$LOG" | tail -n 1; }

finish() {
    say ""
    say "== Summary: $FAILS step(s) failed; dma_fast tested: $FAST =="
    grep -E '^(Shot|Ramp|PGMFlat|DMAStats|CameraSet|CameraValue|dma_stats|CardSet|StreamSummary|LineRateHz|  FAIL|  SKIP|Refused)' "$LOG" \
        | sed 's#/tmp/sg3000-session-[0-9-]*/##' > "$OUT/summary.txt"
    # 终端只列最要紧的：失败项、测试图是否吻合、位统计、吞吐
    grep -E '^(  FAIL|  SKIP|StreamSummary|Shot|Ramp|Refused)' "$OUT/summary.txt" | sed 's/^/  /'
    name=$(basename "$OUT")
    ( cd /tmp && tar -cf - "$name/session.log" "$name/summary.txt" $(cd /tmp && ls "$name"/*.thumb.pgm 2>/dev/null) ) \
        | gzip -c > "$OUT.tar.gz"
    say "BoardSession saved: $OUT.tar.gz ($(wc -c < "$OUT.tar.gz") bytes)"
    exit "$1"
}

# 重新加载驱动；$1 = 额外参数（空或 dma_fast=1）
load_driver() {
    rmmod sg3000_rx >> "$LOG" 2>&1
    run insmod "$KO" cam_serial=1 $1 || return 1
    i=0
    while [ ! -e "$DEV" ] && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
    [ -e "$DEV" ] || return 1
    dmesg | tail -n 3 >> "$LOG"
    for p in "$PARAMS"/*; do echo "param $(basename "$p")=$(cat "$p")" >> "$LOG"; done
    return 0
}

# 采一帧并在板上统计；$1 = 名字
shot() {
    f=$OUT/$1.pgm
    check "capture $1" "$APP" capture --output "$f" --timeout-ms 10000 || return 1
    check "stats   $1" "$APP" pgm-stats "$f" --thumb "$OUT/$1.thumb.pgm"
    say "        $(last '^PGMStats' | sed 's#^PGMStats [^ ]* ##')"
    say "        $(last '^PGMBits')"
    echo "Shot $1: $(last '^PGMStats' | sed 's#^PGMStats [^ ]* ##') | $(last '^PGMBits' | sed 's/^PGMBits //')" >> "$LOG"
}
# 测试图那几张再记一行"和哪个公式吻合"
ramp_line() { echo "Ramp $1: $(last '^PGMRamp' | sed 's/^PGMRamp //')" >> "$LOG"; say "        $(last '^PGMRamp')"; }

# 连续采集 RUN_SECONDS 秒；$1 = 标签
stream_test() {
    [ -w "$PCI/dma_stats" ] && echo 0 > "$PCI/dma_stats"
    check "stream  $1 (${RUN_SECONDS} s)" "$STREAM" --source hardware --processor stats \
        --seconds "$RUN_SECONDS" --queue 4 --max-frame-mib 48
    summary=$(last '"event":"ended"' | sed -n 's/.*"received":"\([0-9]*\)".*"id_discontinuities":"\([0-9]*\)".*"elapsed_ms":"\([0-9]*\)".*"dma_chunk":\([0-9]*\).*"dma_bytes":"\([0-9]*\)".*"dma_ms":"\([0-9]*\)".*"wait_ms":"\([0-9]*\)".*/received=\1 discontinuities=\2 elapsed_ms=\3 chunk=\4 dma_bytes=\5 dma_ms=\6 wait_ms=\7/p')
    echo "StreamSummary $1 $summary" >> "$LOG"
    say "        $summary"
    if [ -r "$PCI/dma_stats" ]; then
        echo "dma_stats $1 $(cat "$PCI/dma_stats")" >> "$LOG"
        say "        dma_stats $(cat "$PCI/dma_stats")"
    fi
}

cam() { check "camera-set $1 $2" "$APP" camera-set "$1" "$2"; }

# ---- 0. 环境 ----------------------------------------------------------------
say "== glass-capture board session $STAMP =="
run uname -a
run sh -c "cd '$B' && sha256sum sg3000-capture sg3000-stream sg_status sg3000_rx.ko"
run "$APP" version

# ---- 1. 驱动（旧 DMA 模式）+ 体检 + 相机 -------------------------------------
say "-- 1. driver (coherent DMA), status, camera"
load_driver "" || { say "  FAIL  insmod / device node"; FAILS=$((FAILS + 1)); finish 1; }
check "sg_status" "$STATUS"
check "camera-probe" "$APP" camera-probe || finish 1

# ---- 2. B 方案：相机和卡都 Base 3 Tap，宽 16384 ------------------------------
say "-- 2. Base 3-tap, width 16384 (camera + card)"
cam ClConfiguration 0 || finish 1
cam DeviceTapGeometry 3 || finish 1
check "card-set CameraWidth 16384" "$APP" card-set CameraWidth 16384 || finish 1
if ! run "$STATUS"; then      # sg_status 退出码 1 = 预检没过
    say "  FAIL  preflight after switching to Base 3-tap"; FAILS=$((FAILS + 1)); finish 1
fi
grep -o 'LineRateHz *[0-9.]*' "$LOG" | tail -n 1 | sed 's/^/        /' | tee -a "$LOG"

# ---- 3. 测试图（第 5⅞ 节第 1 步）-------------------------------------------
say "-- 3. camera test patterns (coherent DMA)"
cam TestPattern 3 && shot ramp && ramp_line ramp
cam TestPattern 2 && shot white && say "        $(last '^PGMFlat')"
cam TestPattern 1 && shot black && say "        $(last '^PGMFlat')"
rm -f "$OUT/white.pgm" "$OUT/black.pgm"
cam TestPattern 0

# ---- 4. 调亮（第 5⅞ 节第 2 步）---------------------------------------------
say "-- 4. real scene: exposure / gain"
shot scene-default
for e in $EXPOSURES; do
    cam ExposureTime "$e" && shot "scene-exp$e"
done
cam AnalogGain 4 && shot scene-gain4

# ---- 5. DMA 吞吐：旧模式 ----------------------------------------------------
say "-- 5. throughput, coherent DMA, 10 kHz"
cam ExposureTime 50
stream_test coherent-10k

# ---- 6. DMA 吞吐：快速模式 --------------------------------------------------
say "-- 6. throughput, dma_fast=1, 10 kHz"
if load_driver dma_fast=1 && grep -q 'dma_fast=Y' "$LOG" && dmesg | tail -n 3 | grep -q 'fast cached'; then
    FAST=1
    cam TestPattern 3 && shot fast-ramp && ramp_line fast-ramp
    cam TestPattern 0
    stream_test fast-10k
else
    say "  SKIP  dma_fast (this sg3000_rx.ko has no dma_fast, or the 2 MiB buffer could not be allocated)"
    load_driver "" || finish 1
fi

# ---- 7. 15 kHz -------------------------------------------------------------
if [ "$DO_15K" = 1 ]; then
    say "-- 7. 15 kHz"
    if cam LineRate 15000; then
        run "$STATUS"
        grep -o 'LineRateHz *[0-9.]*' "$LOG" | tail -n 1 | sed 's/^/        /' | tee -a "$LOG"
        stream_test "$( [ $FAST = 1 ] && echo fast || echo coherent )-15k"
        shot scene-15k
    fi
fi

finish $([ $FAILS = 0 ] && echo 0 || echo 1)
