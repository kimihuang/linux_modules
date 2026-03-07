// SPDX-License-Identifier: GPL-2.0-only
/*
 * Memmap Memory Driver
 *
 * This driver provides block device access to reserved memory regions.
 * It maps physical memory ranges as block devices for use as temporary storage.
 *
 * Usage:
 *   insmod memmap.ko memmap=256M\$0x78000000
 *   mount /dev/memblock0 /mnt/memdisk
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/hdreg.h>

#define DRIVER_NAME "memmap"
#define DRIVER_VERSION "1.0"
#define DRIVER_AUTHOR "Quantum Project"
#define DRIVER_DESC "Reserved memory block device driver"

#define MEMMAP_MAX_DEVS 8
#define KERNEL_SECTOR_SIZE 512

/* Device structure */
struct memmap_device {
    unsigned long size;           /* Device size in bytes */
    unsigned long sectors;        /* Number of sectors */
    void *data;                   /* Mapped memory area */
    phys_addr_t phys_addr;        /* Physical address */
    struct gendisk *gd;           /* Generic disk */
    struct blk_mq_tag_set tag_set; /* Block multiqueue tag set */
    spinlock_t lock;              /* Lock for operations */
    int index;                   /* Device index */
};

static struct memmap_device *devices[MEMMAP_MAX_DEVS];
static int num_devices;
static struct class *memmap_class;
static dev_t dev_base;

/* Module parameters */
static char *memmap_param = "";
module_param(memmap_param, charp, 0000);
MODULE_PARM_DESC(memmap_param,
    "Memory mapping specification: e.g., '256M$0x78000000' or '64M@0x80000000'");

/* Parse memory parameter: '256M\$0x78000000' or '64M@0x80000000' */
static int parse_mem_param(const char *param, unsigned long *size, phys_addr_t *addr)
{
    char size_str[32], addr_str[32];
    char sep;
    u64 val;
    char unit;

    if (sscanf(param, "%31[^@$]%c%31s", size_str, &sep, addr_str) != 3) {
        pr_err("Invalid parameter format: %s\n", param);
        pr_err("Expected format: '256M$0x78000000' or '64M@0x80000000'\n");
        return -EINVAL;
    }

    /* Parse size */
    if (sscanf(size_str, "%llu%c", &val, &unit) == 2) {
        switch (unit) {
            case 'M':
            case 'm':
                *size = val * 1024 * 1024;
                break;
            case 'G':
            case 'g':
                *size = val * 1024 * 1024 * 1024;
                break;
            case 'K':
            case 'k':
                *size = val * 1024;
                break;
            default:
                pr_err("Invalid size unit: %c\n", unit);
                return -EINVAL;
        }
    } else if (sscanf(size_str, "%llu", &val) == 1) {
        *size = val;
    } else {
        pr_err("Invalid size format: %s\n", size_str);
        return -EINVAL;
    }

    /* Parse address */
    if (sscanf(addr_str, "%lli", (long long *)addr) != 1) {
        pr_err("Invalid address format: %s\n", addr_str);
        return -EINVAL;
    }

    pr_info("Parsed: size=%lu bytes (0x%lx), addr=0x%pa\n", *size, *size, addr);
    return 0;
}

/* Block device request handler */
static blk_status_t memmap_queue_rq(struct blk_mq_hw_ctx *hctx,
                                   const struct blk_mq_queue_data *bd)
{
    struct request *req = bd->rq;
    struct memmap_device *dev = req->q->queuedata;
    blk_status_t status = BLK_STS_OK;
    struct bio_vec bvec;
    struct req_iterator iter;
    sector_t sector;

    blk_mq_start_request(req);

    sector = blk_rq_pos(req);

    spin_lock(&dev->lock);

    rq_for_each_segment(bvec, req, iter) {
        unsigned long offset = sector * KERNEL_SECTOR_SIZE;
        void *src, *dst;

        pr_debug("sector=%llu, offset=%lu, len=%u, dir=%s\n",
                 sector, offset, bvec.bv_len,
                 req_op(req) == REQ_OP_READ ? "read" : "write");

        /* Check bounds */
        if (offset + bvec.bv_len > dev->size) {
            pr_err("Access beyond device size: offset=%lu, len=%u, size=%lu\n",
                   offset, bvec.bv_len, dev->size);
            status = BLK_STS_IOERR;
            break;
        }

        if (req_op(req) == REQ_OP_READ) {
            src = dev->data + offset;
            dst = page_address(bvec.bv_page) + bvec.bv_offset;
            memcpy(dst, src, bvec.bv_len);
        } else {
            src = page_address(bvec.bv_page) + bvec.bv_offset;
            dst = dev->data + offset;
            memcpy(dst, src, bvec.bv_len);
        }

        sector += bvec.bv_len / KERNEL_SECTOR_SIZE;
    }

    spin_unlock(&dev->lock);

    blk_mq_end_request(req, status);
    return BLK_STS_OK;
}

/* Block multiqueue operations */
static struct blk_mq_ops memmap_mq_ops = {
    .queue_rq = memmap_queue_rq,
};

/* Get geometry (legacy block device interface) */
static int memmap_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
    geo->heads = 4;
    geo->sectors = 16;
    geo->cylinders = 1024;
    return 0;
}

static const struct block_device_operations memmap_fops = {
    .owner = THIS_MODULE,
    .getgeo = memmap_getgeo,
};

/* Create a memmap device */
static int memmap_device_create(unsigned long size, phys_addr_t phys_addr, int index)
{
    struct memmap_device *dev;
    int ret;

    pr_info("Creating memmap device %d: size=%lu, phys_addr=0x%pa\n",
            index, size, &phys_addr);

    if (index >= MEMMAP_MAX_DEVS) {
        pr_err("Maximum devices (%d) reached\n", MEMMAP_MAX_DEVS);
        return -ENOMEM;
    }

    dev = kzalloc(sizeof(struct memmap_device), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->size = size;
    dev->phys_addr = phys_addr;
    dev->sectors = size / SECTOR_SIZE;
    dev->index = index;
    spin_lock_init(&dev->lock);

    /* Map physical memory */
    pr_info("Remapping physical memory 0x%pa -> virtual\n", &phys_addr);
    dev->data = ioremap_cache(phys_addr, size);
    if (!dev->data) {
        pr_err("Failed to remap physical memory 0x%pa\n", &phys_addr);
        ret = -ENOMEM;
        goto err_free_dev;
    }

    /* Initialize block multiqueue */
    memset(&dev->tag_set, 0, sizeof(dev->tag_set));
    dev->tag_set.ops = &memmap_mq_ops;
    dev->tag_set.nr_hw_queues = 1;
    dev->tag_set.queue_depth = 128;
    dev->tag_set.numa_node = NUMA_NO_NODE;
    dev->tag_set.cmd_size = 0;
    dev->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
    dev->tag_set.driver_data = dev;

    ret = blk_mq_alloc_tag_set(&dev->tag_set);
    if (ret) {
        pr_err("Failed to allocate tag set\n");
        goto err_iounmap;
    }

    /* Allocate generic disk */
    dev->gd = blk_mq_alloc_disk(&dev->tag_set, dev);
    if (IS_ERR(dev->gd)) {
        ret = PTR_ERR(dev->gd);
        pr_err("Failed to allocate disk\n");
        goto err_free_tag_set;
    }

    dev->gd->major = MAJOR(dev_base);
    dev->gd->first_minor = index;
    dev->gd->minors = 1;
    dev->gd->fops = &memmap_fops;
    dev->gd->private_data = dev;
    dev->gd->queue = dev->gd->queue;
    snprintf(dev->gd->disk_name, 32, "memblock%d", index);
    set_capacity(dev->gd, dev->sectors);

    pr_info("Adding disk: %s, %lu sectors\n", dev->gd->disk_name, dev->sectors);

    ret = add_disk(dev->gd);
    if (ret) {
        pr_err("Failed to add disk\n");
        goto err_cleanup_disk;
    }

    devices[index] = dev;
    num_devices++;

    pr_info("Created memmap device %d: /dev/%s, size=%lu MB\n",
            index, dev->gd->disk_name, size / (1024 * 1024));

    return 0;

err_cleanup_disk:
    put_disk(dev->gd);
err_free_tag_set:
    blk_mq_free_tag_set(&dev->tag_set);
err_iounmap:
    iounmap(dev->data);
err_free_dev:
    kfree(dev);
    return ret;
}

/* Destroy a memmap device */
static void memmap_device_destroy(int index)
{
    struct memmap_device *dev = devices[index];

    if (!dev)
        return;

    pr_info("Destroying memmap device %d\n", index);

    del_gendisk(dev->gd);
    put_disk(dev->gd);
    blk_mq_free_tag_set(&dev->tag_set);
    iounmap(dev->data);
    kfree(dev);
    devices[index] = NULL;
    num_devices--;
}

/* Module initialization */
static int __init memmap_init(void)
{
    int ret;
    int index = 0;
    char *param, *next;
    unsigned long size;
    phys_addr_t addr;

    pr_info("%s: %s version %s\n", DRIVER_NAME, DRIVER_DESC, DRIVER_VERSION);

    if (strlen(memmap_param) == 0) {
        pr_warn("No memory mapping parameter specified\n");
        pr_warn("Usage: insmod %s.ko memmap='256M\\$0x78000000'\n", DRIVER_NAME);
        pr_info("Creating default test device (1MB)\n");
        size = 1024 * 1024;
        addr = 0;
        ret = memmap_device_create(size, addr, index++);
        if (ret)
            goto err;
    } else {
        /* Parse multiple memory regions */
        param = memmap_param;
        while (param && *param && index < MEMMAP_MAX_DEVS) {
            next = strchr(param, ',');
            if (next)
                *next = '\0';

            if (parse_mem_param(param, &size, &addr) == 0) {
                ret = memmap_device_create(size, addr, index);
                if (ret)
                    goto err;
                index++;
            }

            if (next) {
                *next = ',';
                param = next + 1;
            } else {
                break;
            }
        }
    }

    /* Create device class */
    memmap_class = class_create(THIS_MODULE, DRIVER_NAME);
    if (IS_ERR(memmap_class)) {
        ret = PTR_ERR(memmap_class);
        pr_err("Failed to create device class\n");
        goto err;
    }

    return 0;

err:
    while (index > 0) {
        memmap_device_destroy(--index);
    }
    return ret;
}

/* Module cleanup */
static void __exit memmap_exit(void)
{
    int i;

    pr_info("Unloading %s\n", DRIVER_NAME);

    for (i = 0; i < MEMMAP_MAX_DEVS; i++) {
        if (devices[i])
            memmap_device_destroy(i);
    }

    class_destroy(memmap_class);
}

module_init(memmap_init);
module_exit(memmap_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_ALIAS_BLOCKDEV_MAJOR(MEMMAP_MAJOR);
