#!/bin/sh
# 在 board/ 目录下运行：编译 docs/demos/src/trace_one_frame.c 到 out/trace_one_frame
set -e
mkdir -p out
gcc -std=c11 -O1 -w -I../include -Icapture -Iprocessing -Istream -Icamera_ctrl -Itransport -Iapps \
    ../docs/demos/src/trace_one_frame.c \
    $(sed -n 's/^\(CAPTURE\|PROCESSING\|STREAM\|CAMERA\|TRANSPORT\|COMMON\) *= *//p; /\\$/{n; s/^ *//p}' Makefile | tr -d '\\') \
    -lm -lpthread -o out/trace_one_frame
