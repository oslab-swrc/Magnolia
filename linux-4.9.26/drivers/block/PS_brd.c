/*
 * Persistent Ram backed block device driver.
 *
 * Copyright (C) 2017 Yongseob. Yi
 * Copyright (C) 2007 Nick Piggin
 * Copyright (C) 2007 Novell Inc.
 *
 * Parts derived from drivers/block/rd.c, drivers/block/brd.c, 
 * drivers/nvdimm/pmem.c and drivers/block/loop.c, 
 * copyright of their respective owners.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/pfn_t.h>
#include <linux/types.h>

#include <linux/memblock.h>
//#include <linux/vmalloc.h>
#include <asm/uaccess.h>

#define SECTOR_SHIFT		9
#define PAGE_SECTORS_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define PAGE_SECTORS		(1 << PAGE_SECTORS_SHIFT)

extern struct memblock memblock;
extern void *vmalloc_PS(unsigned long size, int node, gfp_t flags);
/*
 * Each block persistent ramdisk device has a radix_tree PS_brd_pages of pages
 * that stores the pages containing the block device's contents.
 * A persisnt PS_brd page's ->index is its offset in PAGE_SIZE units.
 * This is similar to, but in no way connected with, the kernel's pagecache 
 * or buffer cache (which sit above our block device).
 */
struct PS_brd_device {
	int		PS_brd_number;

	struct request_queue	*PS_brd_queue;
	struct gendisk		*PS_brd_disk;
	struct list_head	PS_brd_list;
	/* drivers/nvdimm/pmem.h :pmem_device */
	phys_addr_t		phys_addr;
	phys_addr_t		data_offset;
	u64			pfn_flags;
	void			*virt_addr;
	size_t			size;
	u32			pfn_pad;

	/*
	 * Backing store of pages and lock to protect it. This is the contents
	 * of the block device.
	 */
	spinlock_t		PS_brd_lock;
	struct radix_tree_root	PS_brd_pages;
};

/*
 * Look up and return a brd's page for a given sector.
 */
static DEFINE_MUTEX(PS_brd_mutex);
static struct page *PS_brd_lookup_page(struct PS_brd_device *PS_brd, sector_t sector)
{
	pgoff_t idx;
	struct page *page;

	/*
	 * The page lifetime is protected by the fact that we have opened the
	 * device node -- brd pages will never be deleted under us, so we
	 * don't need any further locking or refcounting.
	 *
	 * This is strictly true for the radix-tree nodes as well (ie. we
	 * don't actually need the rcu_read_lock()), however that is not a
	 * documented feature of the radix-tree API so it is better to be
	 * safe here (we don't have total exclusion from radix tree updates
	 * here, only deletes).
	 */
	rcu_read_lock();
	idx = sector >> PAGE_SECTORS_SHIFT; /* sector to page index */
	page = radix_tree_lookup(&PS_brd->PS_brd_pages, idx);
	rcu_read_unlock();

	BUG_ON(page && page->index != idx);

	return page;
}

/*
 * Look up and return a PS_brd's page for a given sector.
 * If one does not exist, allocate an empty page, and insert that. Then
 * return it.
 */
static struct page *PS_brd_insert_page(struct PS_brd_device *PS_brd, sector_t sector)
{
	pgoff_t idx;
	struct page *page;
	gfp_t gfp_flags;

	page = PS_brd_lookup_page(PS_brd, sector);
	if (page)
		return page;

	/*
	 * Must use NOIO because we don't want to recurse back into the
	 * block or filesystem layers from page reclaim.
	 * vfree
	 * Can support DAX and highmem, because our ->direct_access
	 * routine for DAX must return memory that is always addressable.
	 * If DAX was reworked to use pfns and kmap throughout, this
	 * restriction might be able to be lifted.
	 */
	gfp_flags = GFP_NOIO | __GFP_ZERO;
#ifdef CONFIG_ZONE_PSTORAGE
//	gfp_flags |= __GFP_PSTORAGE | __GFP_THISNODE;
	gfp_flags |= __GFP_PSTORAGE;
#endif
//	page = alloc_page(gfp_flags);
	page = vmalloc_PS(memblock.pstorage.total_size, NUMA_NO_NODE, gfp_flags);
	if (!page)
		return NULL;

	if (radix_tree_preload(GFP_NOIO)) {
		__free_page(page);
		return NULL;
	}

	spin_lock(&PS_brd->PS_brd_lock);
	idx = sector >> PAGE_SECTORS_SHIFT;
	page->index = idx;
	if (radix_tree_insert(&PS_brd->PS_brd_pages, idx, page)) {
		__free_page(page);
		page = radix_tree_lookup(&PS_brd->PS_brd_pages, idx);
		BUG_ON(!page);
		BUG_ON(page->index != idx);
	}
	spin_unlock(&PS_brd->PS_brd_lock);

	radix_tree_preload_end();

	return page;
}

static void PS_brd_free_page(struct PS_brd_device *PS_brd, sector_t sector)
{
	struct page *page;
	pgoff_t idx;

	spin_lock(&PS_brd->PS_brd_lock);
	idx = sector >> PAGE_SECTORS_SHIFT;
	page = radix_tree_delete(&PS_brd->PS_brd_pages, idx);
	spin_unlock(&PS_brd->PS_brd_lock);
	if (page)
		__free_page(page);
}

static void PS_brd_zero_page(struct PS_brd_device *PS_brd, sector_t sector)
{
	struct page *page;

	page = PS_brd_lookup_page(PS_brd, sector);
	if (page)
		clear_highpage(page);
}

/*
 * Free all backing store pages and radix tree. This must only be called when
 * there are no other users of the device.
 */
#define FREE_BATCH 16
static void PS_brd_free_pages(struct PS_brd_device *PS_brd)
{
	unsigned long pos = 0;
	struct page *pages[FREE_BATCH];
	int nr_pages;

	do {
		int i;

		nr_pages = radix_tree_gang_lookup(&PS_brd->PS_brd_pages,
				(void **)pages, pos, FREE_BATCH);

		for (i = 0; i < nr_pages; i++) {
			void *ret;

			BUG_ON(pages[i]->index < pos);
			pos = pages[i]->index;
			ret = radix_tree_delete(&PS_brd->PS_brd_pages, pos);
			BUG_ON(!ret || ret != pages[i]);
			__free_page(pages[i]);
		}

		pos++;

		/*
		 * This assumes radix_tree_gang_lookup always returns as
		 * many pages as possible. If the radix-tree code changes,
		 * so will this have to.
		 */
	} while (nr_pages == FREE_BATCH);
}

/*
 * copy_to_PS_brd_setup must be called before copy_to_PS_brd. It may sleep.
 */
static int copy_to_PS_brd_setup(struct PS_brd_device *PS_brd, sector_t sector, size_t n)
{
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	if (!PS_brd_insert_page(PS_brd, sector))
		return -ENOSPC;
	if (copy < n) {
		sector += copy >> SECTOR_SHIFT;
		if (!PS_brd_insert_page(PS_brd, sector))
			return -ENOSPC;
	}
	return 0;
}

static void discard_from_PS_brd(struct PS_brd_device *PS_brd,
		sector_t sector, size_t n)
{
	while (n >= PAGE_SIZE) {
		/*
		 * Don't want to actually discard pages here because
		 * re-allocating the pages can result in writeback
		 * deadlocks under heavy load.
		 */
		if (0)
			PS_brd_free_page(PS_brd, sector);
		else
			PS_brd_zero_page(PS_brd, sector);
		sector += PAGE_SIZE >> SECTOR_SHIFT;
		n -= PAGE_SIZE;
	}
}

/*
 * Copy n bytes from src to the brd starting at sector. Does not sleep.
 */
static void copy_to_PS_brd(struct PS_brd_device *PS_brd, const void *src,
		sector_t sector, size_t n)
{
	struct page *page;
	void *dst;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = PS_brd_lookup_page(PS_brd, sector);
	BUG_ON(!page);

	dst = kmap_atomic(page);
	memcpy(dst + offset, src, copy);
	kunmap_atomic(dst);

	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = PS_brd_lookup_page(PS_brd, sector);
		BUG_ON(!page);

		dst = kmap_atomic(page);
		memcpy(dst, src, copy);
		kunmap_atomic(dst);
	}
}

/*
 * Copy n bytes to dst from the brd starting at sector. Does not sleep.
 */
static void copy_from_PS_brd(void *dst, struct PS_brd_device *PS_brd,
		sector_t sector, size_t n)
{
	struct page *page;
	void *src;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = PS_brd_lookup_page(PS_brd, sector);
	if (page) {
		src = kmap_atomic(page);
		memcpy(dst, src + offset, copy);
		kunmap_atomic(src);
	} else
		memset(dst, 0, copy);

	if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = PS_brd_lookup_page(PS_brd, sector);
		if (page) {
			src = kmap_atomic(page);
			memcpy(dst, src, copy);
			kunmap_atomic(src);
		} else
			memset(dst, 0, copy);
	}
}

/*
 * Process a single bvec of a bio.
 */
static int PS_brd_do_bvec(struct PS_brd_device *PS_brd, struct page *page,
		unsigned int len, unsigned int off, bool is_write,
		sector_t sector)
{
	void *mem;
	int err = 0;

	if (is_write) {
		err = copy_to_PS_brd_setup(PS_brd, sector, len);
		if (err)
			goto out;
	}

	mem = kmap_atomic(page);
	if (!is_write) {
		copy_from_PS_brd(mem + off, PS_brd, sector, len);
		flush_dcache_page(page);
	} else {
		flush_dcache_page(page);
		copy_to_PS_brd(PS_brd, mem + off, sector, len);
	}
	kunmap_atomic(mem);

out:
	return err;
}

static blk_qc_t PS_brd_make_request(struct request_queue *q, struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev;
	struct PS_brd_device *PS_brd = bdev->bd_disk->private_data;
	struct bio_vec bvec;
	sector_t sector;
	struct bvec_iter iter;

	sector = bio->bi_iter.bi_sector;
	if (bio_end_sector(bio) > get_capacity(bdev->bd_disk))
		goto io_error;

	if (unlikely(bio_op(bio) == REQ_OP_DISCARD)) {
		if (sector & ((PAGE_SIZE >> SECTOR_SHIFT) - 1) ||
				bio->bi_iter.bi_size & ~PAGE_MASK)
			goto io_error;
		discard_from_PS_brd(PS_brd, sector, bio->bi_iter.bi_size);
		goto out;
	}

	bio_for_each_segment(bvec, bio, iter) {
		unsigned int len = bvec.bv_len;
		int err;

		err = PS_brd_do_bvec(PS_brd, bvec.bv_page, len, bvec.bv_offset,
				op_is_write(bio_op(bio)), sector);
		if (err)
			goto io_error;
		sector += len >> SECTOR_SHIFT;
	}

out:
	bio_endio(bio);
	return BLK_QC_T_NONE;
io_error:
	bio_io_error(bio);
	return BLK_QC_T_NONE;
}

static int PS_brd_rw_page(struct block_device *PS_bdev, sector_t sector,
		struct page *page, bool is_write)
{
	struct PS_brd_device *PS_brd = PS_bdev->bd_disk->private_data;
	int err = PS_brd_do_bvec(PS_brd, page, PAGE_SIZE, 0, is_write, sector);
	page_endio(page, is_write, err);
	return err;
}

#ifdef CONFIG_BLK_DEV_PS_RAM_DAX
static long PS_brd_direct_access(struct block_device *bdev, sector_t sector,
		void **kaddr, pfn_t *pfn, long size)
{
	struct PS_brd_device *PS_brd = bdev->bd_disk->private_data;
	struct page *page;

	if (!PS_brd)
		return -ENODEV;
	page = PS_brd_insert_page(PS_brd, sector);
	if (!page)
		return -ENOSPC;
//	*kaddr = page_address(page);
//	*pfn = page_to_pfn_t(page);
	*kaddr = page_address(page);
	//*pfn = memblock.pstorage.regions.base;;
	*pfn = page_to_pfn_t(page);
	
	return memblock.pstorage.total_size;
	return PAGE_SIZE;
}
#else
#define PS_brd_direct_access NULL
#endif

static int PS_brd_ioctl(struct block_device *bdev, fmode_t mode,
		unsigned int cmd, unsigned long arg)
{
	int error;
	struct PS_brd_device *PS_brd = bdev->bd_disk->private_data;

	if (cmd != BLKFLSBUF)
		return -ENOTTY;

	/*
	 * ram device BLKFLSBUF has special semantics, we want to actually
	 * release and destroy the ramdisk data.
	 */
	mutex_lock(&PS_brd_mutex);
	mutex_lock(&bdev->bd_mutex);
	error = -EBUSY;
	if (bdev->bd_openers <= 1) {
		/*
		 * Kill the cache first, so it isn't written back to the
		 * device.
		 *
		 * Another thread might instantiate more buffercache here,
		 * but there is not much we can do to close that race.
		 */
		kill_bdev(bdev);
		PS_brd_free_pages(PS_brd);
		error = 0;
	}
	mutex_unlock(&bdev->bd_mutex);
	mutex_unlock(&PS_brd_mutex);

	return error;
}

static const struct block_device_operations PS_brd_fops = {
	.owner =		THIS_MODULE,
	.rw_page =		PS_brd_rw_page,
	.ioctl =		PS_brd_ioctl,
	.direct_access =	PS_brd_direct_access,
};

/*
 * And now the modules code and kernel interface.
 */
static int PS_rd_nr = CONFIG_BLK_DEV_PS_RAM_COUNT;
module_param(PS_rd_nr, int, S_IRUGO);
MODULE_PARM_DESC(PS_rd_nr, "Maximum number of PS_brd devices");

int PS_rd_size = CONFIG_BLK_DEV_PS_RAM_SIZE;
//phys_addr_t PS_rd_size = memblock.pstorage.total_size/1024 ;/*convert to kbyte*/
//int PS_rd_size= memblock.pstorage.total_size/1024 ;/*convert to kbyte*/
module_param(PS_rd_size, int, S_IRUGO);
MODULE_PARM_DESC(PS_rd_size, "Size of total  PRAM/PMEM disk in kbytes.");

static int max_part = 1;
module_param(max_part, int, S_IRUGO);
MODULE_PARM_DESC(max_part, "Num Minors to reserve between devices");

MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(PS_RAMDISK_MAJOR);
MODULE_ALIAS("PS_rd");

#ifndef MODULE
/* Legacy boot options - nonmodular */
static int __init PS_ramdisk_size(char *str)
{
	PS_rd_size = simple_strtol(str, NULL, 0);
	return 1;
}
__setup("PS_ramdisk_size=", PS_ramdisk_size);
#endif

/*
 * The device scheme is derived from loop.c. Keep them in synch where possible
 * (should share code eventually).
 */
static LIST_HEAD(PS_brd_devices);
static DEFINE_MUTEX(PS_brd_devices_mutex);

static struct PS_brd_device *PS_brd_alloc(int i)
{
	struct PS_brd_device *PS_brd;
	struct gendisk *disk;

	PS_rd_size= memblock.pstorage.total_size/1024 ;/*convert to kbyte*/
	PS_brd = kzalloc(sizeof(*PS_brd), GFP_KERNEL | __GFP_PSTORAGE | __GFP_THISNODE);
	//PS_brd = kzalloc(sizeof(*PS_brd), GFP_KERNEL);
	if (!PS_brd)
		goto out;
	PS_brd->PS_brd_number		= i;
	spin_lock_init(&PS_brd->PS_brd_lock);
	INIT_RADIX_TREE(&PS_brd->PS_brd_pages, GFP_ATOMIC);

	PS_brd->PS_brd_queue = blk_alloc_queue(GFP_KERNEL);
	if (!PS_brd->PS_brd_queue)
		goto out_free_dev;

	blk_queue_make_request(PS_brd->PS_brd_queue, PS_brd_make_request);
	blk_queue_max_hw_sectors(PS_brd->PS_brd_queue, 1024);
	blk_queue_bounce_limit(PS_brd->PS_brd_queue, BLK_BOUNCE_ANY);

	/* This is so fdisk will align partitions on 4k, because of
	 * direct_access API needing 4k alignment, returning a PFN
	 * (This is only a problem on very small devices <= 4M,
	 *  otherwise fdisk will align on 1M. Regardless this call
	 *  is harmless)
	 */
	blk_queue_physical_block_size(PS_brd->PS_brd_queue, PAGE_SIZE);

	PS_brd->PS_brd_queue->limits.discard_granularity = PAGE_SIZE;
	blk_queue_max_discard_sectors(PS_brd->PS_brd_queue, UINT_MAX);
	PS_brd->PS_brd_queue->limits.discard_zeroes_data = 1;
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, PS_brd->PS_brd_queue);
#ifdef CONFIG_BLK_DEV_PS_RAM_DAX
	queue_flag_set_unlocked(QUEUE_FLAG_DAX, PS_brd->PS_brd_queue);
#endif
	disk = PS_brd->PS_brd_disk = alloc_disk(max_part);
	if (!disk)
		goto out_free_queue;
	disk->major		= PS_RAMDISK_MAJOR;
	disk->first_minor	= i * max_part;
	disk->fops		= &PS_brd_fops;
	disk->private_data	= PS_brd;
	disk->queue		= PS_brd->PS_brd_queue;
	disk->flags		= GENHD_FL_EXT_DEVT;
	sprintf(disk->disk_name, "PS_ram%d", i);
	set_capacity(disk, PS_rd_size * 2 );/*512-byte sectors*/

	return PS_brd;

out_free_queue:
	blk_cleanup_queue(PS_brd->PS_brd_queue);
out_free_dev:
	kfree(PS_brd);
out:
	return NULL;
}

static void PS_brd_free(struct PS_brd_device *PS_brd)
{
	put_disk(PS_brd->PS_brd_disk);
	blk_cleanup_queue(PS_brd->PS_brd_queue);
	PS_brd_free_pages(PS_brd);
	kfree(PS_brd);
}

static struct PS_brd_device *PS_brd_init_one(int i, bool *new)
{
	struct PS_brd_device *PS_brd;

	*new = false;
	list_for_each_entry(PS_brd, &PS_brd_devices, PS_brd_list) {
		if (PS_brd->PS_brd_number == i)
			goto out;
	}

	PS_brd = PS_brd_alloc(i);
	if (PS_brd) {
		add_disk(PS_brd->PS_brd_disk);
		list_add_tail(&PS_brd->PS_brd_list, &PS_brd_devices);
	}
	*new = true;
out:
	return PS_brd;
}

static void PS_brd_del_one(struct PS_brd_device *PS_brd)
{
	list_del(&PS_brd->PS_brd_list);
	del_gendisk(PS_brd->PS_brd_disk);
	PS_brd_free(PS_brd);
}

static struct kobject *PS_brd_probe(dev_t dev, int *part, void *data)
{
	struct PS_brd_device *PS_brd;
	struct kobject *kobj;
	bool new;

	mutex_lock(&PS_brd_devices_mutex);
	PS_brd = PS_brd_init_one(MINOR(dev) / max_part, &new);
	kobj = PS_brd ? get_disk(PS_brd->PS_brd_disk) : NULL;
	mutex_unlock(&PS_brd_devices_mutex);

	if (new)
		*part = 0;

	return kobj;
}

static int __init PS_brd_init(void)
{
	struct PS_brd_device *PS_brd, *next;
	int i;

	/*
	 * PS_brd module now has a feature to instantiate underlying device
	 * structure on-demand, provided that there is an access dev node.
	 *
	 * (1) if PS_rd_nr is specified, create that many upfront. else
	 *     it defaults to CONFIG_BLK_DEV_RAM_COUNT
	 * (2) User can further extend brd devices by create dev node themselves
	 *     and have kernel automatically instantiate actual device
	 *     on-demand. Example:
	 *		mknod /path/devnod_name b 1 X	# 1 is the rd major
	 *		fdisk -l /path/devnod_name
	 *	If (X / max_part) was not already created it will be created
	 *	dynamically.
	 */

	if (register_blkdev(PS_RAMDISK_MAJOR, "PS_ramdisk"))
		return -EIO;

	if (unlikely(!max_part))
		max_part = 1;

	for (i = 0; i < PS_rd_nr; i++) {
		PS_brd = PS_brd_alloc(i);
		if (!PS_brd)
			goto out_free;
		list_add_tail(&PS_brd->PS_brd_list, &PS_brd_devices);
	}

	/* point of no return */

	list_for_each_entry(PS_brd, &PS_brd_devices, PS_brd_list)
		add_disk(PS_brd->PS_brd_disk);

	blk_register_region(MKDEV(PS_RAMDISK_MAJOR, 0), 1UL << MINORBITS,
			THIS_MODULE, PS_brd_probe, NULL, NULL);

	pr_info("PS_brd: module loaded\n");
	pr_info("pstorage zone total size : [ %#018Lx ]\n",
			(u64)memblock.pstorage.total_size);
	return 0;

out_free:
	list_for_each_entry_safe(PS_brd, next, &PS_brd_devices, PS_brd_list) {
		list_del(&PS_brd->PS_brd_list);
		PS_brd_free(PS_brd);
	}
	unregister_blkdev(PS_RAMDISK_MAJOR, "PS_ramdisk");

	pr_info("PS_brd: module NOT loaded !!!\n");
	return -ENOMEM;
}

static void __exit PS_brd_exit(void)
{
	struct PS_brd_device *PS_brd, *next;

	list_for_each_entry_safe(PS_brd, next, &PS_brd_devices, PS_brd_list)
		PS_brd_del_one(PS_brd);

	blk_unregister_region(MKDEV(PS_RAMDISK_MAJOR, 0), 1UL << MINORBITS);
	unregister_blkdev(PS_RAMDISK_MAJOR, "PS_ramdisk");

	pr_info("PS_brd: module unloaded\n");
}

module_init(PS_brd_init);
module_exit(PS_brd_exit);

