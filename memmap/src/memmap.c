// SPDX-License-Identifier: GPL-2.0
/*
 * memmap.c - Memory-mapped block device driver for Linux 6.1
 *
 * Maps a reserved physical memory region (0x78000000, 256MB) as a block device.
 * Designed for QEMU environments where a memdisk.img is pre-loaded into memory.
 *
 * Usage:
 *   insmod memmap.ko
 *   # Device appears as /dev/memmap0
 *   mount /dev/memmap0 /mnt/point
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/pfn.h>
#include <linux/hdreg.h>

#define DRIVER_NAME     "memmap"
#define DRIVER_VERSION  "1.0"
#define DEVICE_NAME     "memmap"

/* Physical memory region: 0x78000000, 256MB */
#define MEMMAP_PHYS_BASE    0x78000000UL
#define MEMMAP_SIZE_MB      256
#define MEMMAP_SIZE         ((unsigned long)(MEMMAP_SIZE_MB) * 1024 * 1024)
#define MEMMAP_PHYS_END     (MEMMAP_PHYS_BASE + MEMMAP_SIZE)

/* Block device parameters */
#define MEMMAP_SECTOR_SIZE  512
#define MEMMAP_SECTORS      (MEMMAP_SIZE / MEMMAP_SECTOR_SIZE)
#define MEMMAP_MINORS       16
#define MEMMAP_QUEUE_DEPTH  64

/* Module parameters - allow override via insmod */
static ulong mem_base = MEMMAP_PHYS_BASE;
module_param(mem_base, ulong, 0444);
MODULE_PARM_DESC(mem_base, "Physical base address of memory region (default: 0x78000000)");

static ulong mem_size = MEMMAP_SIZE;
module_param(mem_size, ulong, 0444);
MODULE_PARM_DESC(mem_size, "Size of memory region in bytes (default: 256MB)");

static int major_num = 0;   /* 0 = dynamic allocation */
module_param(major_num, int, 0444);
MODULE_PARM_DESC(major_num, "Major device number (0 = auto-assign)");

/* Device structure */
struct memmap_dev {
    void __iomem        *virt_base;     /* Virtual address after ioremap */
    unsigned long        phys_base;     /* Physical base address */
    unsigned long        size;          /* Device size in bytes */
    unsigned long        nsectors;      /* Number of 512-byte sectors */

    struct blk_mq_tag_set tag_set;
    struct gendisk       *disk;
    struct request_queue *queue;

    spinlock_t           lock;
    int                  major;
};

static struct memmap_dev *g_dev = NULL;

/* ------------------------------------------------------------------ */
/*  Request processing                                                  */
/* ------------------------------------------------------------------ */

/**
 * memmap_transfer - Copy data between memory-mapped region and bio vector
 * @dev:    device structure
 * @sector: starting sector number
 * @nsect:  number of sectors
 * @buffer: kernel virtual address of I/O buffer
 * @write:  true = write to device, false = read from device
 */
static void memmap_transfer(struct memmap_dev *dev, sector_t sector,
                             unsigned long nsect, char *buffer, int write)
{
    unsigned long offset = sector * MEMMAP_SECTOR_SIZE;
    unsigned long nbytes = nsect  * MEMMAP_SECTOR_SIZE;

    if ((offset + nbytes) > dev->size) {
        pr_err(DRIVER_NAME ": transfer beyond end: off=%lu len=%lu size=%lu\n",
               offset, nbytes, dev->size);
        return;
    }

    if (write)
        memcpy_toio(dev->virt_base + offset, buffer, nbytes);
    else
        memcpy_fromio(buffer, dev->virt_base + offset, nbytes);
}

/**
 * memmap_handle_bio - Walk all segments of a bio and perform I/O
 */
static blk_status_t memmap_handle_bio(struct memmap_dev *dev, struct bio *bio)
{
    struct bio_vec bvec;
    struct bvec_iter iter;
    sector_t sector = bio->bi_iter.bi_sector;

    bio_for_each_segment(bvec, bio, iter) {
        char   *kaddr  = kmap_local_page(bvec.bv_page) + bvec.bv_offset;
        size_t  len    = bvec.bv_len;
        int     write  = op_is_write(bio_op(bio));

        if ((sector + (len >> 9)) > dev->nsectors) {
            kunmap_local(kaddr);
            return BLK_STS_IOERR;
        }

        memmap_transfer(dev, sector, len >> 9, kaddr, write);
        kunmap_local(kaddr);
        sector += len >> 9;
    }

    return BLK_STS_OK;
}

/**
 * memmap_queue_rq - blk-mq dispatch callback
 */
static blk_status_t memmap_queue_rq(struct blk_mq_hw_ctx *hctx,
                                     const struct blk_mq_queue_data *bd)
{
    struct request     *rq  = bd->rq;
    struct memmap_dev  *dev = hctx->queue->queuedata;
    struct bio         *bio;
    blk_status_t        ret = BLK_STS_OK;

    blk_mq_start_request(rq);

    /* Flush / discard: just succeed */
    if (req_op(rq) == REQ_OP_FLUSH  ||
        req_op(rq) == REQ_OP_DISCARD ||
        req_op(rq) == REQ_OP_SECURE_ERASE) {
        blk_mq_end_request(rq, BLK_STS_OK);
        return BLK_STS_OK;
    }

    /* Walk the bios in the request */
    __rq_for_each_bio(bio, rq) {
        ret = memmap_handle_bio(dev, bio);
        if (ret != BLK_STS_OK)
            break;
    }

    blk_mq_end_request(rq, ret);
    return ret;
}

static const struct blk_mq_ops memmap_mq_ops = {
    .queue_rq = memmap_queue_rq,
};

/* ------------------------------------------------------------------ */
/*  Block device operations                                             */
/* ------------------------------------------------------------------ */

static int memmap_open(struct block_device *bdev, fmode_t mode)
{
    pr_debug(DRIVER_NAME ": open\n");
    return 0;
}

static void memmap_release(struct gendisk *disk, fmode_t mode)
{
    pr_debug(DRIVER_NAME ": release\n");
}

static int memmap_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
    struct memmap_dev *dev = bdev->bd_disk->private_data;
    unsigned long size = dev->nsectors;

    /* Fake C/H/S geometry (compatible with fdisk) */
    geo->heads     = 4;
    geo->sectors   = 16;
    geo->cylinders = size / (geo->heads * geo->sectors);
    geo->start     = 0;
    return 0;
}

static const struct block_device_operations memmap_fops = {
    .owner      = THIS_MODULE,
    .open       = memmap_open,
    .release    = memmap_release,
    .getgeo     = memmap_getgeo,
};

/* ------------------------------------------------------------------ */
/*  Module init / exit                                                  */
/* ------------------------------------------------------------------ */

static int __init memmap_init(void)
{
    struct memmap_dev *dev;
    int ret;

    pr_info(DRIVER_NAME ": version " DRIVER_VERSION
            " - phys=0x%lx size=%lu MB\n",
            mem_base, mem_size / (1024 * 1024));

    /* Validate parameters */
    if (!mem_base || !mem_size || (mem_size & (MEMMAP_SECTOR_SIZE - 1))) {
        pr_err(DRIVER_NAME ": invalid parameters\n");
        return -EINVAL;
    }

    /* Allocate device structure */
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    spin_lock_init(&dev->lock);
    dev->phys_base = mem_base;
    dev->size      = mem_size;
    dev->nsectors  = mem_size / MEMMAP_SECTOR_SIZE;

    /* Reserve the I/O memory region */
    if (!request_mem_region(dev->phys_base, dev->size, DRIVER_NAME)) {
        pr_err(DRIVER_NAME ": cannot reserve memory region "
               "0x%lx-0x%lx\n", dev->phys_base,
               dev->phys_base + dev->size - 1);
        ret = -EBUSY;
        goto err_free_dev;
    }

    /* ioremap: use ioremap_cache to get cached (WB) mapping for speed */
    dev->virt_base = ioremap_cache(dev->phys_base, dev->size);
    if (!dev->virt_base) {
        pr_err(DRIVER_NAME ": ioremap failed\n");
        ret = -ENOMEM;
        goto err_release_region;
    }

    pr_info(DRIVER_NAME ": mapped phys=0x%lx -> virt=%p, %lu sectors\n",
            dev->phys_base, dev->virt_base, dev->nsectors);

    /* Register block device major number */
    dev->major = register_blkdev(major_num, DEVICE_NAME);
    if (dev->major < 0) {
        pr_err(DRIVER_NAME ": register_blkdev failed (%d)\n", dev->major);
        ret = dev->major;
        goto err_iounmap;
    }

    /* Configure blk-mq tag set */
    memset(&dev->tag_set, 0, sizeof(dev->tag_set));
    dev->tag_set.ops            = &memmap_mq_ops;
    dev->tag_set.nr_hw_queues   = 1;
    dev->tag_set.queue_depth    = MEMMAP_QUEUE_DEPTH;
    dev->tag_set.numa_node      = NUMA_NO_NODE;
    dev->tag_set.cmd_size       = 0;
    dev->tag_set.flags          = BLK_MQ_F_SHOULD_MERGE;
    dev->tag_set.driver_data    = dev;

    ret = blk_mq_alloc_tag_set(&dev->tag_set);
    if (ret) {
        pr_err(DRIVER_NAME ": blk_mq_alloc_tag_set failed (%d)\n", ret);
        goto err_unregister_blkdev;
    }

    /* Allocate gendisk */
    dev->disk = blk_mq_alloc_disk(&dev->tag_set, dev);
    if (IS_ERR(dev->disk)) {
        ret = PTR_ERR(dev->disk);
        pr_err(DRIVER_NAME ": blk_mq_alloc_disk failed (%d)\n", ret);
        goto err_free_tagset;
    }

    dev->queue                  = dev->disk->queue;
    dev->disk->major            = dev->major;
    dev->disk->first_minor      = 0;
    dev->disk->minors           = MEMMAP_MINORS;
    dev->disk->fops             = &memmap_fops;
    dev->disk->private_data     = dev;
    snprintf(dev->disk->disk_name, DISK_NAME_LEN, DEVICE_NAME "0");

    /* Set disk capacity (in 512-byte sectors) */
    set_capacity(dev->disk, dev->nsectors);

    /* Queue limits */
    blk_queue_logical_block_size(dev->queue, MEMMAP_SECTOR_SIZE);
    blk_queue_physical_block_size(dev->queue, MEMMAP_SECTOR_SIZE);
    blk_queue_max_hw_sectors(dev->queue, 1024);

    /* Add disk to the kernel */
    ret = add_disk(dev->disk);
    if (ret) {
        pr_err(DRIVER_NAME ": add_disk failed (%d)\n", ret);
        goto err_put_disk;
    }

    g_dev = dev;
    pr_info(DRIVER_NAME ": /dev/%s registered, major=%d, %lu MB\n",
            dev->disk->disk_name, dev->major,
            dev->size / (1024 * 1024));
    return 0;

err_put_disk:
    put_disk(dev->disk);
err_free_tagset:
    blk_mq_free_tag_set(&dev->tag_set);
err_unregister_blkdev:
    unregister_blkdev(dev->major, DEVICE_NAME);
err_iounmap:
    iounmap(dev->virt_base);
err_release_region:
    release_mem_region(dev->phys_base, dev->size);
err_free_dev:
    kfree(dev);
    return ret;
}

static void __exit memmap_exit(void)
{
    struct memmap_dev *dev = g_dev;

    if (!dev)
        return;

    del_gendisk(dev->disk);
    put_disk(dev->disk);
    blk_mq_free_tag_set(&dev->tag_set);
    unregister_blkdev(dev->major, DEVICE_NAME);
    iounmap(dev->virt_base);
    release_mem_region(dev->phys_base, dev->size);
    kfree(dev);
    g_dev = NULL;

    pr_info(DRIVER_NAME ": unloaded\n");
}

module_init(memmap_init);
module_exit(memmap_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Custom Driver");
MODULE_DESCRIPTION("Memory-mapped block device for QEMU reserved RAM (0x78000000, 256MB)");
MODULE_VERSION(DRIVER_VERSION);