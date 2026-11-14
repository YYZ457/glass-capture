// SPDX-License-Identifier: GPL-2.0
/* Experimental SG3000 port-0 receiver. No IRQ, mmap or user-page DMA.
 * XDMA layout checked against pinned Xilinx dma_ip_drivers and supplied driver.
 * All DMA addresses come from Linux DMA APIs. Coherent storage is retained if
 * engine quiescence cannot be established; this intentionally requires reboot.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/iopoll.h>
#include <linux/sched/signal.h>
#include "sg3000_rx_uapi.h"

#define C2H 0x1000
#define SG_DESC 0x5000
#define ST_BUSY 1u
#define ST_ERRORS 0x00f80638u
#define WB_ERROR 0x80000000u
#define WB_COUNT 0x00ffffffu
#define CTRL_POLL 0x04000000u
#define CTRL_ERRORS ((1u << 3) | (1u << 4) | (0x1fu << 9) | (0x1fu << 19))
#define BUFFER_BYTES (SG_DMA_CHUNK + PAGE_SIZE)

struct sg_desc { __le32 control, bytes, src_lo, src_hi, dst_lo, dst_hi, next_lo, next_hi; };
struct sg_dev {
    struct pci_dev *pdev;
    void __iomem *bar0, *bar1;
    struct miscdevice misc;
    struct mutex lock;
    struct kref ref;
    atomic_t opened;
    struct sg_desc *desc;
    dma_addr_t desc_bus, data_bus, wb_bus;
    void *data;
    __le32 *wb;
    struct sg_info info;
    bool gone, quarantined;
    char name[48];
};

static u32 sg_status(struct sg_dev *d) { return readl(d->bar1 + C2H + 0x40); }
static bool sg_idle(struct sg_dev *d)
{
    u32 status;
    writel(0, d->bar1 + C2H + 4);
    readl(d->bar1 + C2H + 4); /* Flush posted stop, then require hardware idle. */
    return !read_poll_timeout(sg_status, status,
        status != U32_MAX && !(status & ST_BUSY), 100, 100000, false, d);
}

static void sg_quarantine(struct sg_dev *d)
{
    d->info.poisoned = 1;
    pci_clear_master(d->pdev);
    if (!d->quarantined) {
        d->quarantined = true;
        kref_get(&d->ref);
        /* An ioctl holds a live module reference, so this pins normal fault
         * cases. remove() may also run after module exit has already begun:
         * never resurrect that module, but still retain all DMA allocations. */
        if (try_module_get(THIS_MODULE))
            dev_crit(&d->pdev->dev, "DMA did not quiesce; retaining DMA storage and pinning module until reboot\n");
        else
            dev_crit(&d->pdev->dev, "DMA did not quiesce during removal; orphaning DMA storage until reboot\n");
    }
}

static void sg_free_dma(struct sg_dev *d)
{
    if (d->desc) dma_free_coherent(&d->pdev->dev, PAGE_SIZE, d->desc, d->desc_bus);
    if (d->wb) dma_free_coherent(&d->pdev->dev, PAGE_SIZE, d->wb, d->wb_bus);
    if (d->data) dma_free_coherent(&d->pdev->dev, BUFFER_BYTES, d->data, d->data_bus);
    d->desc = NULL; d->wb = NULL; d->data = NULL;
}

static void sg_release(struct kref *ref)
{
    struct sg_dev *d = container_of(ref, struct sg_dev, ref);
    sg_free_dma(d);
    pci_dev_put(d->pdev);
    kfree(d);
}

/* WHITELIST-BEGIN —— board/tests/driver_whitelist_test.c 会把这一段抽出来在电脑上编译测试 */

/* 相机串口（docs/interfaces/I7 第 4 节）。默认关闭：insmod sg3000_rx.ko cam_serial=1 才放开。
 * 关闭时这个驱动和旧版完全一样。 */
static bool cam_serial;

/* 串口邮箱 0x4110xx 内的偏移（-1 = 不是串口地址）；UART 寄存器 0x1000.. 同理。
 * 只放开端口 0：2026-11-14 实板上，单路卡（camera enum 0x30424630）碰端口 1 的
 * 0x4130xx / 0x110xx 会触发 nwl-pcie Slave error（synchronous external abort），整板要重启。
 * 将来换双路卡，再按 camera enum 加回端口 1。 */
static int sg_ser_reg(u32 off)
{
    if (off >= 0x411000 && off < 0x411030) return (int)(off - 0x411000);
    return -1;
}

static int sg_uart_reg(u32 off)
{
    if (off >= 0x1000 && off <= 0x100c) return (int)(off - 0x1000);
    return -1;
}

static bool sg_reg_allowed(u32 off, bool write)
{
    int r;

    if (off & 3) return false;
    if (cam_serial) {
        r = sg_ser_reg(off);
        if (r >= 0)
            return write ? (r == 0x04 || r == 0x08 || r == 0x10 || r == 0x14 || r == 0x18 || r == 0x28)
                         : (r == 0x04 || r == 0x1c || r == 0x20 || r == 0x24);
        r = sg_uart_reg(off);
        if (r >= 0)
            return write ? (r == 0x0 || r == 0x4 || r == 0xc) : r == 0xc;
    }
    if (write)
        return off == 0x412010 || off == 0x412014 || off == 0x202000 || off == 0x201000;
    if (off == 0x41200c || off == 0x41201c || off == 0x412020 || off == 0x412024)
        return true;
    return (off >= 0x202008 && off <= 0x20206c) || off == 0x202098 || off == 0x20209c;
}

/* 允许写的地址里，哪些值能写。前提：sg_reg_allowed(off, true) 已经通过。 */
static bool sg_reg_value_allowed(u32 off, u32 value)
{
    int r;

    if (off == 0x201000) return value == 2;
    if (off == 0x202000) return value == U32_MAX;
    if (off == 0x412014) return value == 28 || value == 32;
    r = sg_ser_reg(off);
    if (r == 0x04) return value == 0x04000000;
    if (r == 0x08 || r == 0x18 || r == 0x28) return value == 0xa5;
    if (r == 0x14) return value > 16 && value <= 16 + 512;
    if (sg_uart_reg(off) >= 0) return value <= 0xff;
    return true;
}
/* WHITELIST-END */

static int sg_dma_read(struct sg_dev *d, struct sg_dma *x)
{
    u32 status = 0, wb = 0, offset;
    unsigned long end;
    int ret = 0;
    dma_addr_t destination;
    if (!x->length || x->length > SG_DMA_CHUNK || x->source > U32_MAX ||
        x->source + x->length > (1ULL << 32) || !x->timeout_ms || x->timeout_ms > 2000)
        return -EINVAL;
    if (!access_ok(u64_to_user_ptr(x->buffer), x->length)) return -EFAULT;
    if (d->info.poisoned) return -EIO;
    if (!sg_idle(d)) { sg_quarantine(d); return -EIO; }

    /* MM incremental mode: source and destination low alignment bits match. */
    offset = ((u32)x->source - (u32)d->data_bus) & (d->info.address_alignment - 1);
    destination = d->data_bus + offset;
    memset(d->data + offset, 0, x->length);
    memset(d->desc, 0, sizeof(*d->desc));
    d->desc->control = cpu_to_le32(0xad4b0003); /* Observed Windows MM last descriptor. */
    d->desc->bytes = cpu_to_le32(x->length);
    d->desc->src_lo = cpu_to_le32((u32)x->source);
    d->desc->src_hi = 0;
    d->desc->dst_lo = cpu_to_le32(lower_32_bits(destination));
    d->desc->dst_hi = cpu_to_le32(upper_32_bits(destination));
    WRITE_ONCE(*d->wb, cpu_to_le32(0));
    dma_wmb();
    readl(d->bar1 + C2H + 0x44); /* Clear stale completion/error event status. */
    writel(0, d->bar1 + C2H + 0x90); /* Polling, no engine interrupt mask. */
    writel(lower_32_bits(d->wb_bus), d->bar1 + C2H + 0x88);
    writel(upper_32_bits(d->wb_bus), d->bar1 + C2H + 0x8c);
    writel(lower_32_bits(d->desc_bus), d->bar1 + SG_DESC + 0x80);
    writel(upper_32_bits(d->desc_bus), d->bar1 + SG_DESC + 0x84);
    writel(0, d->bar1 + SG_DESC + 0x88);
    writel(CTRL_ERRORS | CTRL_POLL | 1u, d->bar1 + C2H + 4);
    readl(d->bar1 + C2H + 0x40);
    end = jiffies + msecs_to_jiffies(x->timeout_ms);
    for (;;) {
        wb = le32_to_cpu(READ_ONCE(*d->wb));
        status = sg_status(d);
        if (status == U32_MAX || (status & ST_ERRORS) || (wb & WB_ERROR)) { ret = -EIO; break; }
        if ((wb & WB_COUNT) == 1 && !(status & ST_BUSY)) break;
        if ((wb & WB_COUNT) > 1) { ret = -EPROTO; break; }
        if (signal_pending(current)) { ret = -EINTR; break; }
        if (time_after_eq(jiffies, end)) { ret = -ETIMEDOUT; break; }
        usleep_range(50, 100);
    }
    x->status = status; x->completed = wb;
    if (!sg_idle(d)) { sg_quarantine(d); return -EIO; }
    status = sg_status(d);
    x->status = status;
    if (status == U32_MAX || (status & ST_ERRORS)) ret = -EIO;
    dma_rmb();
    if (!ret && copy_to_user(u64_to_user_ptr(x->buffer), d->data + offset, x->length)) ret = -EFAULT;
    return ret;
}

static long sg_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct sg_dev *d = file->private_data;
    struct sg_reg reg;
    struct sg_dma dma = {0};
    void __user *user = (void __user *)arg;
    int ret = 0;
    if (mutex_lock_interruptible(&d->lock)) return -EINTR;
    if (d->gone) { ret = -ENODEV; goto out; }
    if (cmd == SG_IOC_INFO) {
        if (copy_to_user(user, &d->info, sizeof(d->info))) ret = -EFAULT;
        goto out;
    }
    if (d->info.poisoned) { ret = -EIO; goto out; }
    switch (cmd) {
    case SG_IOC_READ_REG:
    case SG_IOC_WRITE_REG:
        if (copy_from_user(&reg, user, sizeof(reg))) { ret = -EFAULT; break; }
        if (!sg_reg_allowed(reg.offset, cmd == SG_IOC_WRITE_REG)) { ret = -EINVAL; break; }
        if (cmd == SG_IOC_READ_REG) {
            reg.value = readl(d->bar0 + reg.offset);
            if (copy_to_user(user, &reg, sizeof(reg))) ret = -EFAULT;
        } else {
            if (!sg_reg_value_allowed(reg.offset, reg.value)) { ret = -EINVAL; break; }
            writel(reg.value, d->bar0 + reg.offset);
        }
        break;
    case SG_IOC_DMA_READ:
        if (copy_from_user(&dma, user, sizeof(dma))) { ret = -EFAULT; break; }
        ret = sg_dma_read(d, &dma);
        if (copy_to_user(user, &dma, sizeof(dma))) ret = -EFAULT;
        break;
    default: ret = -ENOTTY;
    }
out:
    mutex_unlock(&d->lock);
    return ret;
}

static int sg_open(struct inode *inode, struct file *file)
{
    struct sg_dev *d = container_of(file->private_data, struct sg_dev, misc);
    int ret = 0;
    if (!capable(CAP_SYS_RAWIO)) return -EPERM;
    mutex_lock(&d->lock);
    if (d->gone) ret = -ENODEV;
    else if (d->info.poisoned) ret = -EIO;
    else if (atomic_cmpxchg(&d->opened, 0, 1)) ret = -EBUSY;
    else { kref_get(&d->ref); file->private_data = d; }
    mutex_unlock(&d->lock);
    return ret;
}
static int sg_close(struct inode *inode, struct file *file)
{
    struct sg_dev *d = file->private_data;
    atomic_set(&d->opened, 0);
    kref_put(&d->ref, sg_release);
    return 0;
}
static const struct file_operations sg_fops = {
    .owner = THIS_MODULE, .open = sg_open, .release = sg_close,
    .unlocked_ioctl = sg_ioctl, .compat_ioctl = sg_ioctl, .llseek = no_llseek,
};

static int sg_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct sg_dev *d;
    u32 ident, align;
    unsigned int channel, direction;
    int ret;
    if (pdev->hdr_type != PCI_HEADER_TYPE_NORMAL || pci_resource_len(pdev, 0) != 0x1000000 ||
        pci_resource_len(pdev, 1) != 0x10000 || !(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
        !(pci_resource_flags(pdev, 1) & IORESOURCE_MEM)) return -ENODEV;
    ret = pci_enable_device_mem(pdev); if (ret) return ret;
    ret = pci_request_regions(pdev, "sg3000_rx"); if (ret) goto disable;
    d = kzalloc(sizeof(*d), GFP_KERNEL); if (!d) { ret = -ENOMEM; goto regions; }
    d->pdev = pci_dev_get(pdev); mutex_init(&d->lock); kref_init(&d->ref);
    atomic_set(&d->opened, 0);
    d->bar0 = pci_iomap(pdev, 0, 0); d->bar1 = pci_iomap(pdev, 1, 0);
    if (!d->bar0 || !d->bar1) { ret = -ENOMEM; goto unmap; }
    if ((readl(d->bar1 + 0x2000) & 0xffff0000) != 0x1fc20000 ||
        (readl(d->bar1 + 0x3000) & 0xffff0000) != 0x1fc30000) { ret = -ENODEV; goto unmap; }
    /* Never take ownership while any implemented engine is already running. */
    for (direction = 0; direction < 2; direction++) {
        for (channel = 0; channel < 4; channel++) {
            void __iomem *engine = d->bar1 + direction * 0x1000 + channel * 0x100;
            u32 engine_id = readl(engine);
            if ((engine_id & 0xffff0000) != (0x1fc00000u | (direction << 16))) continue;
            if ((readl(engine + 0x40) & ST_BUSY) || (readl(engine + 4) & 1)) {
                ret = -EBUSY; goto unmap;
            }
        }
    }
    ident = readl(d->bar1 + C2H);
    if ((ident & 0xffff0000) != 0x1fc10000 || (ident & 0x8000) ||
        (sg_status(d) & ST_BUSY) || (readl(d->bar1 + C2H + 4) & 1)) { ret = -ENODEV; goto unmap; }
    align = readl(d->bar1 + C2H + 0x4c);
    d->info = (struct sg_info){ .abi = SG_ABI_VERSION, .vendor = pdev->vendor, .device = pdev->device,
        .domain = pci_domain_nr(pdev->bus), .bus = pdev->bus->number, .devfn = pdev->devfn,
        .engine_id = ident, .address_alignment = (align >> 16) & 255,
        .length_granularity = (align >> 8) & 255, .address_bits = align & 255, .max_transfer = SG_DMA_CHUNK };
    if (!d->info.address_alignment) d->info.address_alignment = 1;
    if (!d->info.length_granularity) d->info.length_granularity = 1;
    if (!d->info.address_bits) d->info.address_bits = 64;
    if (!is_power_of_2(d->info.address_alignment) || d->info.address_alignment > PAGE_SIZE ||
        d->info.address_bits < 32 || d->info.address_bits > 64) { ret = -ENODEV; goto unmap; }
    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (ret) ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
    if (ret) goto unmap;
    d->desc = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, &d->desc_bus, GFP_KERNEL);
    d->wb = dma_alloc_coherent(&pdev->dev, PAGE_SIZE, &d->wb_bus, GFP_KERNEL);
    d->data = dma_alloc_coherent(&pdev->dev, BUFFER_BYTES, &d->data_bus, GFP_KERNEL);
    if (!d->desc || !d->wb || !d->data) { ret = -ENOMEM; goto unmap; }
    pci_set_master(pdev); pci_intx(pdev, 0);
    snprintf(d->name, sizeof(d->name), "sg3000_rx_%s", pci_name(pdev));
    d->misc = (struct miscdevice){ .minor = MISC_DYNAMIC_MINOR, .name = d->name,
        .fops = &sg_fops, .parent = &pdev->dev, .mode = 0600 };
    ret = misc_register(&d->misc); if (ret) { pci_clear_master(pdev); goto unmap; }
    pci_set_drvdata(pdev, d);
    dev_info(&pdev->dev, "port0 C2H MM receiver ready at /dev/%s (experimental)\n", d->name);
    return 0;
unmap:
    if (d->bar1) pci_iounmap(pdev, d->bar1);
    if (d->bar0) pci_iounmap(pdev, d->bar0);
    kref_put(&d->ref, sg_release);
regions:
    pci_release_regions(pdev);
disable:
    pci_disable_device(pdev);
    return ret;
}

static void sg_remove(struct pci_dev *pdev)
{
    struct sg_dev *d = pci_get_drvdata(pdev);
    misc_deregister(&d->misc);
    mutex_lock(&d->lock);
    d->gone = true;
    if (!sg_idle(d)) sg_quarantine(d);
    pci_clear_master(pdev);
    /* Open descriptors retain only the object after removal, never DMA storage.
     * Quarantine deliberately retains storage while the device may still write. */
    if (!d->quarantined) sg_free_dma(d);
    pci_iounmap(pdev, d->bar0); pci_iounmap(pdev, d->bar1);
    mutex_unlock(&d->lock);
    pci_release_regions(pdev); pci_disable_device(pdev);
    kref_put(&d->ref, sg_release);
}
static const struct pci_device_id sg_ids[] = { { PCI_DEVICE(0x4c54, 0x3000) }, {0} };
MODULE_DEVICE_TABLE(pci, sg_ids);
static struct pci_driver sg_driver = { .name = "sg3000_rx", .id_table = sg_ids,
    .probe = sg_probe, .remove = sg_remove };
module_pci_driver(sg_driver);
module_param(cam_serial, bool, 0444);
MODULE_PARM_DESC(cam_serial, "Allow Camera Link serial mailbox and UART registers (default off)");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental SG3000 single-channel bounded C2H receiver");
MODULE_VERSION("0.1");
