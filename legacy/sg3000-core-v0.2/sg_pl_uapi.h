/* SPDX-License-Identifier: MIT
 * PROPOSED host/PL-driver ABI v1. No physical MMIO addresses are assigned here.
 * A separate Linux driver must implement this before the PL backend can run.
 */
#ifndef SG_PL_UAPI_H
#define SG_PL_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define SGPL_MAGIC 0x53474950u
#define SGPL_ABI 1u
#define SGPL_CAP_BOUNDED_PROCESS 1u
struct sgpl_caps {
    __u32 magic, abi, flags, pixel_format;
    __u32 max_width, max_height, max_bytes, reserved;
};
struct sgpl_job {
    __aligned_u64 user_pixels; /* CPU pointer for copy_from_user, NEVER a DMA address */
    __aligned_u64 bytes, sequence, first_line;
    __u32 width, height, stride, pixel_format;
    __u32 timeout_ms, status, algorithm, decision;
    __aligned_u64 pixel_count, sum, above_threshold;
    __u32 minimum, maximum, error_flags, reserved;
};
#define SGPL_GET_CAPS _IOR('P', 0x60, struct sgpl_caps)
#define SGPL_PROCESS _IOWR('P', 0x61, struct sgpl_job)
/* PROCESS must be synchronous and bounded: on every return no device can still
 * reference user_pixels. Driver uses its own DMA buffers + correct cache sync,
 * stops/quarantines buffers on timeout, and never trusts user-supplied bus addresses.
 * Unimplemented drivers MUST NOT advertise SGPL_CAP_BOUNDED_PROCESS.
 */
#endif
