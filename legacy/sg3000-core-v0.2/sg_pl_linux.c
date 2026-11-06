/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "sg_processing.h"
#include "sg_pl_uapi.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
struct pl { int fd; struct sgpl_caps caps; };
_Static_assert(sizeof(struct sgpl_caps) == 32, "PL caps ABI size");
_Static_assert(sizeof(struct sgpl_job) == 104, "PL job ABI size");
static int process(void *context, const struct sg_image_view *v, uint32_t timeout,
                   struct sg_processing_result *r)
{
    struct pl *p = context; struct sgpl_job job; int ret = sg_view_validate(v);
    if (ret || !r || !timeout || timeout > 60000) return ret ? ret : -EINVAL;
    if (v->width > p->caps.max_width || v->height > p->caps.max_height ||
        v->bytes > p->caps.max_bytes) return -EFBIG;
    memset(&job, 0, sizeof(job)); job.user_pixels = (uintptr_t)v->pixels;
    job.bytes = v->bytes; job.sequence = v->sequence; job.first_line = v->first_line;
    job.width = v->width; job.height = v->height; job.stride = v->stride;
    job.pixel_format = v->pixel_format; job.timeout_ms = timeout;
    if (ioctl(p->fd, SGPL_PROCESS, &job)) return -errno;
    if (job.status || job.error_flags || job.sequence != v->sequence ||
        job.first_line != v->first_line || job.decision > SG_FAIL ||
        job.pixel_count != (uint64_t)v->width * v->height || job.minimum > job.maximum ||
        job.maximum > 255 || job.sum > job.pixel_count * 255 ||
        job.above_threshold > job.pixel_count) return -EPROTO;
    memset(r, 0, sizeof(*r)); r->abi = SG_PROCESS_ABI;
    r->sequence = job.sequence; r->first_line = job.first_line;
    r->pixel_count = job.pixel_count; r->sum = job.sum;
    r->above_threshold = job.above_threshold; r->minimum = job.minimum;
    r->maximum = job.maximum; r->decision = job.decision; r->algorithm = job.algorithm;
    return 0;
}
static void close_pl(void *context)
{ struct pl *p = context; close(p->fd); free(p); }
int sg_pl_open(struct sg_processor *out, const char *device)
{
    struct pl *p; struct stat st; int ret;
    if (!out || !device) return -EINVAL;
    p = calloc(1, sizeof(*p)); if (!p) return -ENOMEM;
    p->fd = open(device, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (p->fd < 0) { ret = -errno; free(p); return ret; }
    if (fstat(p->fd, &st) || !S_ISCHR(st.st_mode)) { ret = -ENODEV; goto fail; }
    if (ioctl(p->fd, SGPL_GET_CAPS, &p->caps)) { ret = -errno; goto fail; }
    if (p->caps.magic != SGPL_MAGIC || p->caps.abi != SGPL_ABI ||
        !(p->caps.flags & SGPL_CAP_BOUNDED_PROCESS) ||
        p->caps.pixel_format != SG_PIXEL_MONO8 || !p->caps.max_bytes) {
        ret = -ENOTSUP; goto fail;
    }
    out->context = p; out->process = process; out->close = close_pl; return 0;
fail:
    close(p->fd); free(p); return ret;
}
