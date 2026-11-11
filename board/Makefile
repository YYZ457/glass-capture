# board/ 用户态程序的构建（驱动单独用 driver/module.sh）
#
#   make check                         在电脑上做语法/接口检查（不需要板子）
#   make test                          在电脑上跑假卡测试和单元测试
#   make CROSS_COMPILE=aarch64-linux-gnu-   交叉编译出板上程序到 out/
#
# 每个模块一行，新增文件时加进对应变量即可。

CC      = $(CROSS_COMPILE)gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
INC     = -I../include -Icapture -Iprocessing -Istream -Icamera_ctrl -Itransport -Iapps
OUT     = out

CAPTURE    = capture/sg_session.c capture/sg_mailbox.c capture/sg_preflight.c \
             capture/sg_block.c capture/sg_acq.c capture/sg_io_linux.c
PROCESSING = processing/sg_view.c processing/sg_stats.c processing/sg_candidates.c
STREAM     = stream/sg_pipeline.c stream/sg_linecheck.c
CAMERA     = camera_ctrl/cam_serial.c camera_ctrl/cam_config.c
TRANSPORT  = transport/tx_jsonl.c transport/tx_tcp.c
COMMON     = apps/pgm_write.c

LIB = $(CAPTURE) $(PROCESSING) $(STREAM) $(CAMERA) $(TRANSPORT) $(COMMON)

all: $(OUT)/sg_status $(OUT)/sg3000-capture $(OUT)/sg3000-stream

$(OUT)/sg_status: apps/sg_status.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(INC) -static $^ -lm -o $@
$(OUT)/sg3000-capture: apps/sg_capture_main.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(INC) -static $^ -lm -o $@
$(OUT)/sg3000-stream: apps/sg_stream_main.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(INC) -static -pthread $^ -lm -o $@
$(OUT):
	mkdir -p $@

# 电脑上跑测试（假卡 + 单元测试）。交叉编译时加 RUN=qemu-aarch64-static 在模拟器里跑。
TEST_CFLAGS = -std=c11 -O1 -g -Wall -Wextra -Werror -UNDEBUG
test: | $(OUT)
	$(CC) $(TEST_CFLAGS) $(INC) -static tests/fake_card_test.c $(LIB) -lm -o $(OUT)/fake_card_test
	$(CC) $(TEST_CFLAGS) $(INC) -static -pthread tests/unit_test.c $(LIB) -lm -o $(OUT)/unit_test
	$(RUN) $(OUT)/fake_card_test
	$(RUN) $(OUT)/unit_test

check:
	@for f in $(LIB) apps/*.c; do $(CC) $(CFLAGS) -D_GNU_SOURCE $(INC) -fsyntax-only $$f || exit 1; done
	@echo "all interfaces compile"

clean:
	rm -rf $(OUT)

.PHONY: all check clean test
