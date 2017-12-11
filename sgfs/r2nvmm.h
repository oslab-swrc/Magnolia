/*
 *
 * BRIEF DESCRIPTION
 *
 * Definitions for the R2NVMM filesystem.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#ifndef _LINUX_R2NVMM_H
#define _LINUX_R2NVMM_H

#include <linux/fs.h>
//#include <linux/dax.h>
//#include <linux/magic.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/crc16.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/rcupdate.h>
#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/radix-tree.h>
#include <linux/version.h>
#include <linux/kthread.h>
#include <linux/buffer_head.h>
#include <linux/uio.h>
#include <asm/tlbflush.h>
#include <linux/version.h>
#include <linux/pfn_t.h>
#include <linux/mempolicy.h>

//#define	R2NVMM_SUPER_MAGIC	0xE0170601	/* R2NVMM */
#define	R2NVMM_SUPER_MAGIC	0x0601	/* R2NVMM */

#define PAGE_SHIFT_2M 21

#define PAGE_SHIFT_2M 21
#define PAGE_SHIFT_1G 30

/*
 * Mount flags
 */
#define R2NVMM_MOUNT_PROTECT 0x000001            /* wprotect CR0.WP */
#define R2NVMM_MOUNT_XATTR_USER 0x000002         /* Extended user attributes */
#define R2NVMM_MOUNT_POSIX_ACL 0x000004          /* POSIX Access Control Lists */
#define R2NVMM_MOUNT_DAX 0x000008                /* Direct Access */
#define R2NVMM_MOUNT_ERRORS_CONT 0x000010        /* Continue on errors */
#define R2NVMM_MOUNT_ERRORS_RO 0x000020          /* Remount fs ro on errors */
#define R2NVMM_MOUNT_ERRORS_PANIC 0x000040       /* Panic on errors */
#define R2NVMM_MOUNT_HUGEMMAP 0x000080           /* Huge mappings with mmap */
#define R2NVMM_MOUNT_HUGEIOREMAP 0x000100        /* Huge mappings with ioremap */
#define R2NVMM_MOUNT_FORMAT      0x000200        /* was FS formatted on mount? */
#define R2NVMM_MOUNT_MOUNTING    0x000400        /* FS currently being mounted */

/*
 * Maximal count of links to a file
 */
#define R2NVMM_LINK_MAX          32000

#define R2NVMM_DEF_BLOCK_SIZE_4K 4096

#define R2NVMM_INODE_SIZE 128    /* must be power of two */
#define R2NVMM_INODE_BITS   7

#define R2NVMM_NAME_LEN 255

/* R2NVMM supported data blocks */
#define R2NVMM_BLOCK_TYPE_4K     0
#define R2NVMM_BLOCK_TYPE_2M     1
#define R2NVMM_BLOCK_TYPE_1G     2
#define R2NVMM_BLOCK_TYPE_MAX    3

#define META_BLK_SHIFT 9

/*
 * Play with this knob to change the default block type.
 * By changing the R2NVMM_DEFAULT_BLOCK_TYPE to 2M or 1G,
 * we should get pretty good coverage in testing.
 */
#define R2NVMM_DEFAULT_BLOCK_TYPE R2NVMM_BLOCK_TYPE_4K

#define r2nvmm_dbg(s, args ...)		pr_debug(s, ## args)
#define r2nvmm_err(s, args ...)		r2nvmm_error_msg(sb, s, ## args)
#define r2nvmm_warn(s, args ...)	pr_warning(s, ## args)
#define r2nvmm_info(s, args ...)	pr_info(s, ## args)


#define r2nvmm_set_bit                   __test_and_set_bit_le
#define r2nvmm_clear_bit                 __test_and_clear_bit_le
#define r2nvmm_find_next_zero_bit                find_next_zero_bit_le

#define R2NVMM_SB_SIZE 1024	/* must be power of two */
//#define R2NVMM_SB_SIZE 512       /* must be power of two */
#define R2NVMM_NAME_LEN 255

/* The root inode follows immediately after the redundant super block */
#define R2NVMM_ROOT_INO		(1)
#define R2NVMM_BLOCKNODE_INO	(2)
#define R2NVMM_INODELIST_INO	(3)
#define R2NVMM_INODELIST1_INO	(4)
#define R2NVMM_INODETABLE_INO	(5)

#define R2NVMM_ROOT_INO_START	(R2NVMM_SB_SIZE * 2)
#define R2NVMM_NORMAL_INODE_START	(16)

#define R2NVMM_SB_STATIC_SIZE(ps) ((u64)&ps->s_start_dynamic - (u64)ps)

/* the above fast mount fields take total 32 bytes in the super block */
#define R2NVMM_FAST_MOUNT_FIELD_SIZE  (36)


/* ======================= Write ordering ========================= */

#define CACHELINE_SIZE  (64)
#define CACHELINE_MASK  (~(CACHELINE_SIZE - 1))
#define CACHELINE_ALIGN(addr) (((addr)+CACHELINE_SIZE-1) & CACHELINE_MASK)

#define X86_FEATURE_PCOMMIT	( 9*32+22) /* PCOMMIT instruction */
#define X86_FEATURE_CLFLUSHOPT	( 9*32+23) /* CLFLUSHOPT instruction */
#define X86_FEATURE_CLWB	( 9*32+24) /* CLWB instruction */


struct r2nvmm_falloc {
	wait_queue_head_t *waitq; /* faults into hole wait for punch to end */
	pgoff_t start;		/* start of range currently being fallocated */
	pgoff_t next;		/* the next page offset to be fallocated */
	pgoff_t nr_falloced;	/* how many new pages have been fallocated */
	pgoff_t nr_unswapped;	/* how often writepage refused to swap out */
};

struct free_list {
	spinlock_t s_lock;
	struct rb_root	block_free_tree;
	struct r2nvmm_range_node *first_node;
	unsigned long	block_start;
	unsigned long	block_end;
	unsigned long	num_free_blocks;
	unsigned long	num_blocknode;

	/* Statistics */
	unsigned long	alloc_log_count;
	unsigned long	alloc_data_count;
	unsigned long	free_log_count;
	unsigned long	free_data_count;
	unsigned long	alloc_log_pages;
	unsigned long	alloc_data_pages;
	unsigned long	freed_log_pages;
	unsigned long	freed_data_pages;

	u64		padding[8];	/* Cache line break */
};

/*
 * Structure of an inode in R2NVMM.
 * Keep the inode size to within 120 bytes: We use the last eight bytes
 * as inode table tail pointer.
 */
struct r2nvmm_inode {
	/* first 48 bytes */
	__le16	i_rsvd;		/* reserved. used to be checksum */
	u8	valid;		/* Is this inode valid? */
	u8	i_blk_type;	/* data block size this inode uses */
	__le32	i_flags;	/* Inode flags */
	__le64	i_size;		/* Size of data in bytes */
	__le32	i_ctime;	/* Inode modification time */
	__le32	i_mtime;	/* Inode b-tree Modification time */
	__le32	i_atime;	/* Access time */
	__le16	i_mode;		/* File mode */
	__le16	i_links_count;	/* Links count */

	/*
	 * Blocks count. This field is updated in-place;
	 * We just make sure it is consistent upon clean umount,
	 * and it is recovered in DFS recovery if power failure occurs.
	 */
	__le64	i_blocks;
	__le64	i_xattr;	/* Extended attribute block */

	/* second 48 bytes */
	__le32	i_uid;		/* Owner Uid */
	__le32	i_gid;		/* Group Id */
	__le32	i_generation;	/* File version (for NFS) */
	__le32	padding;
	__le64	r2nvmm_ino;	/* r2nvmm inode number */

	__le64	log_head;	/* Log head pointer */
	__le64	log_tail;	/* Log tail pointer */

	struct {
		__le32 rdev;	/* major/minor # */
	} dev;			/* device inode */

	/* Leave 8 bytes for inode table tail pointer */
} __attribute((__packed__));


struct simple_xattrs {
	struct list_head head;
	spinlock_t lock;
};

struct simple_xattr {
	struct list_head list;
	char *name;
	size_t size;
	char value[0];
};

struct r2nvmm_inode_info {
	spinlock_t		lock;
	unsigned int		seals;		/* r2nvmm seals */
	unsigned long		flags;
	unsigned long		alloced;	/* data pages alloced to file */
	unsigned long		swapped;	/* subtotal assigned to swap */
	struct list_head        shrinklist;     /* shrinkable hpage inodes */
	struct list_head	swaplist;	/* chain of maybes on swap */
	struct shared_policy	policy;		/* NUMA memory alloc policy */
//	struct simple_xattrs	xattrs;		/* list of xattrs */
	struct inode vfs_inode;
};

/*
 * Structure of the super block in R2NVMM
 * The fields are partitioned into static and dynamic fields. The static fields
 * never change after file system creation. This was primarily done because
 * r2nvmm_get_block() returns NULL if the block offset is 0 (helps in catching
 * bugs). So if we modify any field using journaling (for consistency), we
 * will have to modify s_sum which is at offset 0. So journaling code fails.
 * This (static+dynamic fields) is a temporary solution and can be avoided
 * once the file system becomes stable and r2nvmm_get_block() returns correct
 * pointers even for offset 0.
 */
struct r2nvmm_super_block {
	/* static fields. they never change after file system creation.
	 * checksum only validates up to s_start_dynamic field below */
	__le16		s_sum;              /* checksum of this sb */
	__le16		s_padding16;
	__le32		s_magic;            /* magic signature */
	__le32		s_padding32;
	__le32		s_blocksize;        /* blocksize in bytes */
	__le64		s_size;             /* total size of fs in bytes */
	char		s_volume_name[16];  /* volume name */

	__le64		s_start_dynamic;

	/* all the dynamic fields should go here */
	/* s_mtime and s_wtime should be together and their order should not be
	 * changed. we use an 8 byte write to update both of them atomically */
	__le32		s_mtime;            /* mount time */
	__le32		s_wtime;            /* write time */
	/* fields for fast mount support. Always keep them together */
	__le64		s_num_free_blocks;
} __attribute((__packed__));
#if 0
struct r2nvmm_mount_opts{
	/* Mount options */
	kuid_t		uid;    /* Mount uid for root directory */
	kgid_t		gid;    /* Mount gid for root directory */
	umode_t		mode;   /* Mount mode for root directory */

	unsigned long	bpi;
	unsigned long	num_inodes;
	unsigned long	blocksize;
	unsigned long	initsize;
	unsigned long	s_mount_opt;
};
#endif

/*
 * R2NVMM super-block data in memory
 */
struct r2nvmm_sb_info {
	struct super_block *sb;
	unsigned long max_blocks;   /* How many blocks are allowed */
	unsigned long max_inodes;   /* How many inodes are allowed */
	unsigned long free_inodes;  /* How many are left for allocation */
	spinlock_t stat_lock;	    /* Serialize r2nvmm_sb_info changes */
	umode_t mode;		    /* Mount mode for root directory */
	unsigned char huge;	    /* Whether to try for hugepages */
	kuid_t uid;		    /* Mount uid for root directory */
	kgid_t gid;		    /* Mount gid for root directory */
	struct mempolicy *mpol;     /* default memory policy for mappings */
	spinlock_t shrinklist_lock;   /* Protects shrinklist */
	struct list_head shrinklist;  /* List of shinkable inodes */
	unsigned long shrinklist_len; /* Length of shrinklist */
	int nodes;

	/* ZEROED page for cache page initialized */
	void *zeroed_page;

	/* Per-NODE inode map */
	struct inode_map	*inode_maps;

	/* Decide new inode map id */
	unsigned long map_id;
	/* Per-NODE free block list */
	struct free_list *free_lists;

	/* Shared free block list */
	struct free_list *shared_free_list;
#if 0
//	unsigned long	num_blocks;
//	struct r2nvmm_mount_opts mount_opts;
//	struct percpu_counter used_blocks;  /* How many are allocated */
	struct block_device *s_bdev;

	/*
	 * base physical and virtual address of R2NVMM (which is also
	 * the pointer to the super block)
	 */
	phys_addr_t	phys_addr;
	void		*virt_addr;

	/*
	 * Backing store option:
	 * 1 = no load, 2 = no store,
	 * else do both
	 */
	unsigned int	r2nvmm_backing_option;

//	atomic_t	next_generation;

	/* inode tracking */
	unsigned long	s_inodes_used_count;
	unsigned long	reserved_blocks;
	struct proc_dir_entry *s_proc;

	struct mutex 	s_lock;	/* protects the SB's buffer-head */

	unsigned long per_list_blocks;
#endif
};

static inline struct r2nvmm_inode_info *R2NVMM_I(struct inode *inode)
{
	return container_of(inode, struct r2nvmm_inode_info, vfs_inode);
}

#define RESERVED_BLOCKS 3

struct inode_map {
	struct mutex inode_table_mutex;
	struct rb_root	inode_inuse_tree;
	unsigned long	num_range_node_inode;
	struct r2nvmm_range_node *first_inode_range;
	int allocated;
	int freed;
};




static inline bool arch_has_pcommit(void)
{
	return static_cpu_has(X86_FEATURE_PCOMMIT);
}

static inline bool arch_has_clwb(void)
{
	return static_cpu_has(X86_FEATURE_CLWB);
}
//#####################################
//#####################################
//#####################################
#if 0
extern int support_clwb;
extern int support_pcommit;

#define _mm_clflush(addr)\
	asm volatile("clflush %0" : "+m" (*(volatile char *)(addr)))
#define _mm_clflushopt(addr)\
	asm volatile(".byte 0x66; clflush %0" : "+m" (*(volatile char *)(addr)))
#define _mm_clwb(addr)\
	asm volatile(".byte 0x66; xsaveopt %0" : "+m" (*(volatile char *)(addr)))
#define _mm_pcommit()\
	asm volatile(".byte 0x66, 0x0f, 0xae, 0xf8")

/* Provides ordering from all previous clflush too */
static inline void PERSISTENT_MARK(void)
{
	/* TODO: Fix me. */
}

static inline void PERSISTENT_BARRIER(void)
{
	asm volatile ("sfence\n" : : );
	if (support_pcommit) {
		/* Do nothing */
	}
}

static inline void r2nvmm_flush_buffer(void *buf, uint32_t len, bool fence)
{
	uint32_t i;
	len = len + ((unsigned long)(buf) & (CACHELINE_SIZE - 1));
	if (support_clwb) {
		for (i = 0; i < len; i += CACHELINE_SIZE)
			_mm_clwb(buf + i);
	} else {
		for (i = 0; i < len; i += CACHELINE_SIZE)
			_mm_clflush(buf + i);
	}
	/* Do a fence only if asked. We often don't need to do a fence
	 * immediately after clflush because even if we get context switched
	 * between clflush and subsequent fence, the context switch operation
	 * provides implicit fence. */
	if (fence)
		PERSISTENT_BARRIER();
}
#endif

/* Function Prototypes */
/*********************************/
/* super.c */
extern const struct super_operations r2nvmm_ops;


/* file.c */
extern const struct file_operations r2nvmm_file_operations;
extern const struct inode_operations r2nvmm_file_inode_operations;


/* inode.c */
extern const struct inode_operations r2nvmm_dir_inode_operations; 
extern const struct address_space_operations r2nvmm_aop;
struct inode *r2nvmm_get_inode(struct super_block *sb, const struct inode *dir,
	 umode_t mode, dev_t dev);




int r2nvmm_fill_super(struct super_block *sb, void *data, int silent);
int r2nvmm_readpage(struct file *file, struct page *page);
int r2nvmm_write_begin(struct file *file, struct address_space *mapping, 
		loff_t pos, unsigned len, unsigned flags, 
		struct page **pagep, void **fsdata); 
int r2nvmm_write_end(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned len, unsigned copied,
		struct page *page, void *fsdata); 

extern struct dentry *simple_lookup(struct inode *, struct dentry *, unsigned int flags);
extern int simple_link(struct dentry *old_dentry, struct inode *dir, 
		struct dentry *dentry);
extern int simple_unlink(struct inode *dir, struct dentry *dentry);
extern struct dentry *simple_lookup(struct inode *dir, struct dentry *dentry,
	       	unsigned int flags);
extern int simple_rename(struct inode *old_dir, struct dentry *old_dentry,  
		struct inode *new_dir, struct dentry *new_dentry, unsigned int flags);
extern int simple_empty(struct dentry *);
extern void inode_nohighmem(struct inode *inode);
extern const struct inode_operations page_symlink_inode_operations;
extern const struct vm_operations_struct generic_file_vm_ops;

/* inode.c*/
int r2nvmm_init_inode_inuse_list(struct super_block *sb);
int r2nvmm_init_inode_table(struct super_block *sb);

#endif /* _LINUX_R2NVMM_H */
