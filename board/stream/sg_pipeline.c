/* SPDX-License-Identifier: MIT
 *
 * sg_pipeline.c —— 双线程流水线：
 *   主线程：source->next 取块 → 放进有界队列
 *   处理线程（consume）：从队列取块 → processor->process → sink->publish → 释放块
 * 只管排队、线程、计数、停止，不知道数据从哪来、怎么处理、往哪发。
 *
 * 迁移来源：legacy sg_pipeline.c（逻辑、检查顺序、错误码照搬）
 * 规则：
 *   - 队列满不丢块：直接以 -ENOBUFS 结束（处理跟不上 = 系统不合格，必须让人知道）
 *   - 任何一方出错都置 abort，两边线程都会尽快退出
 *   - 先 stop 采集，再等处理线程收尾；stop 只调一次
 * 怎么验证：sg3000-stream --source synthetic 跑一段，received = processed
 * 状态：🟡 合成数据测过，没上板
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sg_pipeline.h"
#include "sg_err.h"

#define MAX_QUEUE_DEPTH   16u
#define MAX_TOTAL_BYTES   (UINT64_C(512) * 1024 * 1024)   /* 队列 + 正在处理的 + 正在接收的 */

/* 队列里的一格：块本身（拥有内存）+ 给处理器看的视图（借用） */
struct slot {
    struct sg_capture_frame frame;
    struct sg_image_view view;
};

/* 环形队列：head 出，tail 进，count 个有效 */
struct queue {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    struct slot *slots;
    unsigned depth, head, tail, count;
    int done, abort;

    struct sg_processor *processor;
    struct sg_result_sink *sink;
    struct sg_pipeline_stats *stats;
    uint32_t timeout;
};

uint64_t sg_monotonic_ns(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        abort();
    }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

/* 处理结果自己必须"对得上号"，否则算协议错误。 */
static int result_valid(const struct sg_processing_result *r, const struct sg_image_view *v)
{
    return r->abi == SG_PROCESS_ABI &&
           r->sequence == v->sequence &&
           r->first_line == v->first_line &&
           r->error_flags == 0 &&
           r->decision <= SG_FAIL;
}

/* ==== 处理线程 ========================================================= */

static void *consume(void *context)
{
    struct queue *q = context;

    for (;;) {
        struct slot item;
        struct sg_processing_result result;
        int ret;

        /* ① 等到队列里有东西，或者被通知结束 */
        pthread_mutex_lock(&q->mutex);
        while (!q->count && !q->done && !q->abort) {
            pthread_cond_wait(&q->ready, &q->mutex);
        }
        if (q->abort || (!q->count && q->done)) {
            pthread_mutex_unlock(&q->mutex);
            break;
        }
        item = q->slots[q->head];
        memset(&q->slots[q->head], 0, sizeof(item));   /* 所有权移到 item */
        q->head = (q->head + 1u) % q->depth;
        q->count--;
        pthread_mutex_unlock(&q->mutex);

        /* ② 处理（不持锁，主线程可以同时往队列里放） */
        memset(&result, 0, sizeof(result));
        ret = q->processor->process(q->processor->context, &item.view, q->timeout, &result);
        if (!ret && !result_valid(&result, &item.view)) {
            ret = -EPROTO;
        }

        /* ③ 发布结果 */
        if (ret) {
            pthread_mutex_lock(&q->mutex);
            q->stats->processing_error = ret;
            q->abort = 1;
            pthread_mutex_unlock(&q->mutex);
        } else {
            ret = q->sink->publish(q->sink->context, &item.view, &result, q->timeout);
            pthread_mutex_lock(&q->mutex);
            if (ret) {
                q->stats->output_error = ret;
                q->abort = 1;
            } else {
                q->stats->processed++;
            }
            pthread_mutex_unlock(&q->mutex);
        }

        sg_capture_frame_free(&item.frame);
        if (ret) {
            break;
        }
    }
    return NULL;
}

/* ==== 主线程 =========================================================== */

static int options_valid(const struct sg_source *src, const struct sg_processor *proc,
                         const struct sg_result_sink *sink, const struct sg_pipeline_options *o,
                         const struct sg_pipeline_stats *st)
{
    if (!src || !src->next || !src->stop || !proc || !proc->process ||
        !sink || !sink->publish || !o || !st) {
        return 0;
    }
    return o->queue_depth != 0 && o->queue_depth <= MAX_QUEUE_DEPTH &&
           o->max_frame_bytes != 0 && o->max_frame_bytes <= SG_CAPTURE_MAX_BYTES &&
           o->max_frame_bytes * (o->queue_depth + 2u) <= MAX_TOTAL_BYTES &&
           o->processing_timeout_ms != 0 && o->processing_timeout_ms <= 60000 &&
           o->duration_ms <= UINT64_C(86400000);
}

/* 刚收到的块是否像样 */
static int frame_valid(const struct sg_capture_frame *f, const struct sg_pipeline_options *o,
                       const struct sg_pipeline_stats *st)
{
    return f->data != NULL && f->width != 0 && f->height != 0 &&
           (uint64_t)f->width * f->height == f->length &&
           f->length <= o->max_frame_bytes &&
           st->received_lines <= UINT64_MAX - f->height;
}

int sg_pipeline_run(struct sg_source *src, struct sg_processor *proc, struct sg_result_sink *sink,
                    const struct sg_pipeline_options *o, struct sg_pipeline_stats *st)
{
    struct queue q;
    pthread_t thread;
    uint32_t previous_id = 0;
    uint64_t deadline;
    unsigned i;
    int ret = 0, rc;

    if (!options_valid(src, proc, sink, o, st)) {
        return -EINVAL;
    }
    memset(st, 0, sizeof(*st));
    memset(&q, 0, sizeof(q));
    q.depth     = o->queue_depth;
    q.processor = proc;
    q.sink      = sink;
    q.stats     = st;
    q.timeout   = o->processing_timeout_ms;

    /* ---- 准备队列和锁；失败也要 stop 一次源（约定：run 总会调一次 stop） ---- */
    q.slots = calloc(q.depth, sizeof(*q.slots));
    if (q.slots == NULL) {
        st->stop_error = src->stop(src->context);
        return -ENOMEM;
    }
    rc = pthread_mutex_init(&q.mutex, NULL);
    if (rc) {
        free(q.slots);
        st->stop_error = src->stop(src->context);
        return -rc;
    }
    rc = pthread_cond_init(&q.ready, NULL);
    if (rc) {
        pthread_mutex_destroy(&q.mutex);
        free(q.slots);
        st->stop_error = src->stop(src->context);
        return -rc;
    }

    st->started_ns = sg_monotonic_ns();
    deadline = o->duration_ms ? st->started_ns + o->duration_ms * UINT64_C(1000000) : UINT64_MAX;

    rc = pthread_create(&thread, NULL, consume, &q);
    if (rc) {
        ret = -rc;
        st->stop_error = src->stop(src->context);
        goto cleanup;
    }

    /* ---- 主循环：取块 → 检查 → 入队 ---- */
    for (;;) {
        struct slot item;
        int aborted;

        pthread_mutex_lock(&q.mutex);
        aborted = q.abort;
        pthread_mutex_unlock(&q.mutex);
        if (aborted) {
            break;
        }
        if (o->cancel && *o->cancel) {
            st->cancelled = 1;
            break;
        }
        if ((o->max_blocks && st->received >= o->max_blocks) || sg_monotonic_ns() >= deadline) {
            break;
        }

        memset(&item, 0, sizeof(item));
        ret = src->next(src->context, &item.frame);
        if (ret) {
            sg_capture_frame_free(&item.frame);
            if (ret == -ECANCELED && o->cancel && *o->cancel) {
                st->cancelled = 1;      /* Ctrl+C 导致的取消不算错 */
                ret = 0;
            } else {
                st->source_error = ret;
            }
            break;
        }
        if (!frame_valid(&item.frame, o, st)) {
            sg_capture_frame_free(&item.frame);
            ret = st->source_error = -EPROTO;
            break;
        }

        item.view.pixels        = item.frame.data;
        item.view.sequence      = st->received + 1u;
        item.view.first_line    = st->received_lines;
        item.view.timestamp_ns  = sg_monotonic_ns();
        item.view.bytes         = item.frame.length;
        item.view.width         = item.frame.width;
        item.view.height        = item.frame.height;
        item.view.stride        = item.frame.width;
        item.view.pixel_format  = SG_PIXEL_MONO8;
        item.view.card_block_id = item.frame.block_id;
        item.view.flags = o->synthetic ? SG_META_SYNTHETIC : SG_META_CONTINUITY_UNVERIFIED;

        /* 块号不连续：默认只计数并打标志；strict 模式直接结束 */
        if (st->received && item.frame.block_id != (uint32_t)(previous_id + 1u)) {
            st->id_discontinuities++;
            item.view.flags |= SG_META_ID_DISCONTINUITY;
            if (o->strict_block_ids) {
                sg_capture_frame_free(&item.frame);
                ret = st->source_error = -EPROTO;
                break;
            }
        }
        previous_id = item.frame.block_id;
        st->received++;
        st->bytes += item.frame.length;
        st->received_lines += item.frame.height;

        /* 入队；满了就结束，不丢块 */
        pthread_mutex_lock(&q.mutex);
        if (q.abort || q.count == q.depth) {
            if (!q.abort) {
                st->queue_full_events++;
                ret = st->source_error = -ENOBUFS;
            }
            q.abort = 1;
            pthread_mutex_unlock(&q.mutex);
            sg_capture_frame_free(&item.frame);
            break;
        }
        q.slots[q.tail] = item;
        q.tail = (q.tail + 1u) % q.depth;
        q.count++;
        if (q.count > st->queue_peak) {
            st->queue_peak = q.count;
        }
        pthread_cond_signal(&q.ready);
        pthread_mutex_unlock(&q.mutex);
    }

    /* ---- 收尾：先停采集，再让处理线程把队列里剩下的处理完（出错则直接放弃） ---- */
    st->stop_error = src->stop(src->context);
    pthread_mutex_lock(&q.mutex);
    q.done = 1;
    if (ret || st->stop_error) {
        q.abort = 1;
    }
    pthread_cond_broadcast(&q.ready);
    pthread_mutex_unlock(&q.mutex);
    pthread_join(thread, NULL);

    if (!ret) {
        ret = st->processing_error ? st->processing_error : st->output_error;
    }
    if (!ret) {
        ret = st->stop_error;
    }

cleanup:
    for (i = 0; i < q.depth; ++i) {
        sg_capture_frame_free(&q.slots[i].frame);
    }
    free(q.slots);
    pthread_cond_destroy(&q.ready);
    pthread_mutex_destroy(&q.mutex);
    st->finished_ns = sg_monotonic_ns();
    return ret;
}
