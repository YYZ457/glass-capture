/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "sg_pipeline.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct slot { struct sg_capture_frame frame; struct sg_image_view view; };
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
    if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static void *consume(void *context)
{
    struct queue *q = context;
    for (;;) {
        struct slot item; struct sg_processing_result result; int ret;
        pthread_mutex_lock(&q->mutex);
        while (!q->count && !q->done && !q->abort) pthread_cond_wait(&q->ready, &q->mutex);
        if (q->abort || (!q->count && q->done)) { pthread_mutex_unlock(&q->mutex); break; }
        item = q->slots[q->head]; memset(&q->slots[q->head], 0, sizeof(item));
        q->head = (q->head + 1u) % q->depth; --q->count;
        pthread_mutex_unlock(&q->mutex);
        memset(&result, 0, sizeof(result));
        ret = q->processor->process(q->processor->context, &item.view, q->timeout, &result);
        if (!ret && (result.abi != SG_PROCESS_ABI || result.sequence != item.view.sequence ||
            result.first_line != item.view.first_line || result.error_flags ||
            result.decision > SG_FAIL)) ret = -EPROTO;
        if (ret) {
            pthread_mutex_lock(&q->mutex); q->stats->processing_error = ret;
            q->abort = 1; pthread_mutex_unlock(&q->mutex);
        } else {
            ret = q->sink->publish(q->sink->context, &item.view, &result, q->timeout);
            pthread_mutex_lock(&q->mutex);
            if (ret) { q->stats->output_error = ret; q->abort = 1; }
            else ++q->stats->processed;
            pthread_mutex_unlock(&q->mutex);
        }
        sg_capture_frame_free(&item.frame);
        if (ret) break;
    }
    return NULL;
}
int sg_pipeline_run(struct sg_source *source, struct sg_processor *processor,
                    struct sg_result_sink *sink, const struct sg_pipeline_options *o,
                    struct sg_pipeline_stats *stats)
{
    struct queue q; pthread_t thread; uint32_t previous = 0; int ret = 0, thread_rc;
    uint64_t deadline;
    if (!source || !source->next || !source->stop || !processor || !processor->process ||
        !sink || !sink->publish || !o || !stats || !o->queue_depth || o->queue_depth > 16 ||
        !o->max_frame_bytes || o->max_frame_bytes > SG_CAPTURE_MAX_BYTES ||
        o->max_frame_bytes * (o->queue_depth + 2u) > UINT64_C(512)*1024*1024 ||
        !o->processing_timeout_ms || o->processing_timeout_ms > 60000 ||
        o->duration_ms > UINT64_C(86400000)) return -EINVAL;
    memset(stats, 0, sizeof(*stats)); memset(&q, 0, sizeof(q));
    q.depth = o->queue_depth; q.processor = processor; q.sink = sink; q.stats = stats;
    q.timeout = o->processing_timeout_ms;
    q.slots = calloc(q.depth, sizeof(*q.slots));
    if (!q.slots) { stats->stop_error = source->stop(source->context); return -ENOMEM; }
    thread_rc = pthread_mutex_init(&q.mutex, NULL);
    if (thread_rc) { free(q.slots); stats->stop_error = source->stop(source->context); return -thread_rc; }
    thread_rc = pthread_cond_init(&q.ready, NULL);
    if (thread_rc) { pthread_mutex_destroy(&q.mutex); free(q.slots); stats->stop_error=source->stop(source->context); return -thread_rc; }
    stats->started_ns = sg_monotonic_ns();
    deadline = o->duration_ms ? stats->started_ns + o->duration_ms * UINT64_C(1000000) : UINT64_MAX;
    thread_rc = pthread_create(&thread, NULL, consume, &q);
    if (thread_rc) { ret = -thread_rc; stats->stop_error=source->stop(source->context); goto cleanup; }
    for (;;) {
        struct slot item; int stopped;
        pthread_mutex_lock(&q.mutex); stopped = q.abort; pthread_mutex_unlock(&q.mutex);
        if (stopped) break;
        if (o->cancel && *o->cancel) { stats->cancelled=1; break; }
        if ((o->max_blocks && stats->received >= o->max_blocks) || sg_monotonic_ns() >= deadline) break;
        memset(&item, 0, sizeof(item));
        ret = source->next(source->context, &item.frame);
        if (ret) {
            sg_capture_frame_free(&item.frame);
            if (ret == -ECANCELED && o->cancel && *o->cancel) { stats->cancelled=1; ret=0; }
            else stats->source_error = ret;
            break;
        }
        if (!item.frame.data || !item.frame.width || !item.frame.height ||
            (uint64_t)item.frame.width * item.frame.height != item.frame.length ||
            item.frame.length > o->max_frame_bytes ||
            stats->received_lines > UINT64_MAX - item.frame.height) {
            sg_capture_frame_free(&item.frame); ret = stats->source_error = -EPROTO; break;
        }
        item.view = (struct sg_image_view){item.frame.data, stats->received + 1u,
            stats->received_lines, sg_monotonic_ns(), item.frame.length,
            item.frame.width, item.frame.height, item.frame.width, SG_PIXEL_MONO8,
            item.frame.block_id, o->synthetic ? SG_META_SYNTHETIC : SG_META_CONTINUITY_UNVERIFIED};
        if (stats->received && item.frame.block_id != (uint32_t)(previous + 1u)) {
            ++stats->id_discontinuities; item.view.flags |= SG_META_ID_DISCONTINUITY;
            if (o->strict_block_ids) {
                sg_capture_frame_free(&item.frame); ret=stats->source_error=-EPROTO; break;
            }
        }
        previous = item.frame.block_id;
        ++stats->received; stats->bytes += item.frame.length;
        stats->received_lines += item.frame.height;
        pthread_mutex_lock(&q.mutex);
        if (q.abort || q.count == q.depth) {
            if (!q.abort) { ++stats->queue_full_events; ret=stats->source_error=-ENOBUFS; }
            q.abort=1; pthread_mutex_unlock(&q.mutex); sg_capture_frame_free(&item.frame); break;
        }
        q.slots[q.tail]=item; q.tail=(q.tail+1u)%q.depth; ++q.count;
        if (q.count > stats->queue_peak) stats->queue_peak=q.count;
        pthread_cond_signal(&q.ready); pthread_mutex_unlock(&q.mutex);
    }
    /* Stop acquisition BEFORE waiting for remaining processing; no repeated START. */
    stats->stop_error=source->stop(source->context);
    pthread_mutex_lock(&q.mutex); q.done=1;
    if (ret || stats->stop_error) q.abort=1;
    pthread_cond_broadcast(&q.ready); pthread_mutex_unlock(&q.mutex);
    pthread_join(thread, NULL);
    if (!ret) ret=stats->processing_error ? stats->processing_error : stats->output_error;
    if (!ret) ret=stats->stop_error;
cleanup:
    for (unsigned i=0; i<q.depth; ++i) sg_capture_frame_free(&q.slots[i].frame);
    free(q.slots); pthread_cond_destroy(&q.ready); pthread_mutex_destroy(&q.mutex);
    stats->finished_ns=sg_monotonic_ns(); return ret;
}
