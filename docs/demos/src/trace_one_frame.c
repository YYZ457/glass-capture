/* trace_one_frame.c —— 演示用：在假卡（board/tests/fake_card_test.c）外面包一层日志，
 * 打印 sg_capture_once() 采一帧时的每一次寄存器读写和 DMA。
 * 只给 docs/demos/capture_protocol.md 用，不参与 board 构建。
 * 用法：trace_one_frame [--raw] [--bad-magic]
 * 假卡：块宽 16 像素、8 行，分 2 包、每包 64 字节；块号从 7 开始。 */
#define main fake_card_main
#include "../../../board/tests/fake_card_test.c"
#undef main

static const char *reg_name(uint32_t off)
{
    switch (off) {
#define N(x) case x: return #x;
    N(SG_MBX_TX_ROOM) N(SG_MBX_TX_DATA) N(SG_MBX_TX_LEN) N(SG_MBX_RX_OCCUPANCY)
    N(SG_MBX_RX_DATA) N(SG_MBX_RX_LEN) N(SG_ACQ_ARM) N(SG_HDR_ACK) N(SG_HDR_READY)
    N(SG_ACQ_STATE) N(SG_HDR_WIDTH) N(SG_HDR_LINES) N(SG_HDR_PIXFMT) N(SG_HDR_ZERO_A)
    N(SG_HDR_ZERO_B) N(SG_HDR_SEQ) N(SG_HDR_PAYLOAD_LEN) N(SG_HDR_DEST_OFFSET)
    N(SG_HDR_SRC_ADDR) N(SG_HDR_BLOCK_ID) N(SG_HDR_MAGIC) N(SG_HDR_MAX_LINES)
    N(SG_L_DDR_STATUS) N(SG_L_PIXEL_CLOCKS) N(SG_L_CL_CONFIG) N(SG_L_CAMERA_COLOR)
    N(SG_L_CAMERA_DEPTH) N(SG_L_ACQ_START) N(SG_L_ACQ_STOP) N(SG_L_CAMERA_WIDTH)
    N(SG_L_CAMERA_HEIGHT)
#undef N
    default: { static char b[32]; snprintf(b, sizeof b, "0x%06x(无名)", off); return b; }
    }
}
static int raw;   /* --raw：邮箱也逐个寄存器打印 */

/* 连续相同的操作合并成一行，免得邮箱逐字读写刷屏 */
static char last[96]; static unsigned rep, step;
static void flush(void) { if (rep) printf("%3u  %s\n", ++step, last); if (rep > 1) printf("       （上一行连续 %u 次）\n", rep); rep = 0; }
static void emit(const char *line) { if (rep && !strcmp(line, last)) { rep++; return; } flush(); snprintf(last, sizeof last, "%s", line); rep = 1; }

static uint32_t tx[8], rx[80]; static unsigned ntx, nrx, rxwords;
static int is_mbx(uint32_t off) { return off >= SG_MBX_BASE && off < SG_MBX_BASE + 0x30; }
static int t_read(void *c, uint32_t off, uint32_t *v)
{
    int r = f_read(c, off, v); char b[96];
    if (off == SG_MBX_RX_LEN) { rxwords = *v / 4; nrx = 0; }
    if (off == SG_MBX_RX_DATA && nrx < 80) {
        rx[nrx++] = *v;
        if (!raw && nrx == rxwords) {
            if (tx[4] == SG_MBX_OP_WRITE) snprintf(b, sizeof b, "邮箱  写 %-18s = %u   （回复 ACK #%u）", reg_name(tx[5]), tx[7], rx[2] >> 16);
            else snprintf(b, sizeof b, "邮箱  读 %-18s → %u   （回复 ACK #%u）", reg_name(tx[5]), rx[7], rx[2] >> 16);
            emit(b);
        }
    }
    if (!raw && is_mbx(off)) return r;
    if (off == SG_MBX_RX_DATA || off == SG_MBX_RX_OCCUPANCY) snprintf(b, sizeof b, "R %-20s", reg_name(off));
    else snprintf(b, sizeof b, "R %-20s = 0x%08x", reg_name(off), *v);
    emit(b); return r;
}
static int t_write(void *c, uint32_t off, uint32_t v)
{
    char b[96];
    if (off == SG_MBX_TX_DATA && ntx < 8) tx[ntx++] = v;
    if (!raw && is_mbx(off)) { if (off == SG_MBX_TX_LEN) ntx = 0; return f_write(c, off, v); }
    if (off == SG_MBX_TX_DATA) { emit("W SG_MBX_TX_DATA       （逐字写请求报文）"); }
    else if (off == SG_MBX_TX_LEN) {
        snprintf(b, sizeof b, "W SG_MBX_TX_LEN = %u   ── 邮箱请求：%s %s", v,
                 tx[4] == SG_MBX_OP_WRITE ? "写" : "读", reg_name(tx[5]));
        emit(b); ntx = 0;
    } else { snprintf(b, sizeof b, "W %-20s = 0x%08x", reg_name(off), v); emit(b); }
    return f_write(c, off, v);
}
static int t_dma(void *c, uint32_t src, void *buf, uint32_t len, uint32_t to)
{
    char b[96]; snprintf(b, sizeof b, "DMA 卡上 DDR 0x%04x → 内存，%u 字节", src, len); emit(b);
    return f_dma(c, src, buf, len, to);
}

int main(int argc, char **argv)
{
    int bad_magic = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--raw")) raw = 1;
        if (!strcmp(argv[i], "--bad-magic")) bad_magic = 1;   /* 假卡给出错误的块头魔数 */
    }
    struct sg_capture_io io = IO; struct sg_capture_session s;
    struct sg_capture_options o; struct sg_capture_result r;
    io.read_reg = t_read; io.write_reg = t_write; io.dma_read = t_dma;
    reset(); C.bad_magic = bad_magic; sg_capture_default_options(&o);
    sg_capture_session_init(&s, &io);
    int ret = sg_capture_once(&s, &o, &r);
    flush();
    printf("\nsg_capture_once = %d，帧 %ux%u，块号 %u，stop_error = %d\n",
           ret, r.frame.width, r.frame.height, r.frame.block_id, r.stop_error);
    if (ret) printf("primary_stage = \"%s\"，start_attempted = %d，stop_attempted = %d，帧内存 = %s\n",
                    r.primary_stage ? r.primary_stage : "-", r.start_attempted, r.stop_attempted, r.frame.data ? "未释放" : "已释放");
    sg_capture_frame_free(&r.frame);
    return ret != 0;
}
