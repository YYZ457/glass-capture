/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SG3000_RX_UAPI_H
#define SG3000_RX_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define SG_ABI_VERSION 1
#define SG_DMA_CHUNK 65536u
struct sg_info {
    __u32 abi, vendor, device, domain, bus, devfn;
    __u32 engine_id, address_alignment, length_granularity, address_bits;
    __u32 max_transfer, poisoned;
};
struct sg_reg { __u32 offset, value; };
struct sg_dma {
    __aligned_u64 source;
    __aligned_u64 buffer;
    __u32 length, timeout_ms;
    __u32 status, completed;
};
#define SG_IOC_INFO _IOR('S', 0x30, struct sg_info)
#define SG_IOC_READ_REG _IOWR('S', 0x31, struct sg_reg)
#define SG_IOC_WRITE_REG _IOW('S', 0x32, struct sg_reg)
#define SG_IOC_DMA_READ _IOWR('S', 0x33, struct sg_dma)
#endif
