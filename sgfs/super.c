#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/memblock.h>
#include <linux/statfs.h>
//#include <linux/random.h>
#include <linux/mempolicy.h>
#include "r2nvmm.h"

#define R2NVMM_DEFAULT_MODE	0755

extern struct memblock memblock;
const struct super_operations r2nvmm_sops;

static struct kmem_cache *r2nvmm_inode_cachep;
//static struct kmem_cache *r2nvmm_range_node_cachep;

struct file_system_type r2nvmm_fs_type;
struct vfsmount *r2nvmm_mnt;
const char *proc_dirname = "fs/r2nvmm"; 
struct proc_dir_entry *r2nvmm_proc_root;

static inline struct r2nvmm_sb_info *R2NVMM_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

void r2nvmm_error_msg(struct super_block *sb, const char *fmt, ...)
{
	va_list args;

	printk("r2nvmm error: ");
	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
#if 0
	if (test_opt(sb, ERRORS_PANIC))
		panic("r2nvmm: panic from previous error\n");
	if (test_opt(sb, ERRORS_RO)) {
		printk(KERN_CRIT "r2nvmm err: remounting filesystem read-only"(;
		sb->s_flags |= MS_RDONLY;
	}
#endif
}
#if 0
struct dentry *r2nvmm_get_parent(struct dentry *child)
{
	return ERR_PTR(-ESTALE);
}

int r2nvmm_encode_fh(struct inode *inode, __u32 *fh, int *len, struct inode *parent)
{
	if (*len < 3) {
		*len = 3;
		return FIELD_INVALID;
	}

	if (inode_unhashed(inode)) {
		DEFINE_SPINLOCK(lock);
		spin_lock(&lock);
		if (inode_unhashed(inode))
			__insert_inode_hash(inode, inode->i_no + inode->i_generation);
		spin_unlock(&lock);
	}

	fh[0] = inode->i_generation;
	fh[1] = inode->i_ino;
	fh[2] = ((__u64)inode->i_no) >> 32;

	*len=3;
	return 1;
}

struct dentry *r2nvmm_fh_to_dentry(struct super_block *sb, struct fid *fid,
		int fh_len, int fh_type)
{
	struct inode *inode;
	struct dentry *dentry = NULL;
	u64 inum;

	if (fh_len < 3)
		return NULL;

	inum = fid->raw[2];
	inum = ( inum << 32 ) | fid->raw[1];

	inode = ilookup5(sb, (unsigned long)(inum + fid->raw[0]),
			r2nvmm_match, fid->raw);

	if (inode) {
		dentry = d_find_alias(inode);
		iput(inode);
	}

	return dentry;
}

const struct export_operations r2nvmm_export_ops = {
	.get_parent	= r2nvmm_get_parent,
	.encode_fh	= r2nvmm_encode_fh,
	.fh_to_dentry	= r2nvmm_fh_to_dentry,
};
int r2nvmm_match(struct inode *ino, void *vfh)
{
	__u32 *fh = vfh;
	__u64 inum = fh[2];
	inum = ( inum << 32 ) | fh[1];
	return ino->i_no == inum && fh [0] == ino->i_generation;
}
#endif

#if 0
void r2nvmm_set_blocksize(struct super_block *sb, unsigned long size)
{
	int bits;

	/*
	 * We've already validated the user input and the value here must be
	 * between R2NVMM_MAX_BLOCK_SIZE and R2NVMM_MIN_BLOCK_SIZE
	 * and it must be a power of 2.
	 */
	bits = fls(size) - 1;
	sb->s_blocksize_bits = bits;
	sb->blocksize = (1 << bits);
}

int r2nvmm_get_block_info(struct super_block *sb,
		struct r2nvmm_sb_info *sbi)
{
	void *virt_addr = NULL;
	pfn_t __pfn_t;
	long size;

	if (!sb->s_bedv->bd_disk->fops->direct_access){
		r2nvmm_err(sb, "device does not support DAX\n");
		return -EINVAL;
	}

	sbi->s_bdev = sb->s_bdev;

	size = sb->s_bdev->bd_disk->fops->direct_access(sb->s_bdev, 0,
			&virt_addr, &__pfn_t);

	if (size <= 0) {
		r2nvmm_err(sb, "direct_access failed\n");
		return -EINVAL;
	}

	sbi->virt_addr = virt_addr;
	sbi->phys_addr = pfn_t_to_pfn(__pfn_t) << PAGE_SHIFT;
	sbi->initsize = size;

	r2nvmm_dbg("%s: dev %s, phys_addr 0x%llx, virt_addr %p, size %ld\n",
			sbi->phys_addr, sbi_virt_addr, sbi->initsize);
	return 0;
}

loff_t r2nvmm_max_size(int bits)
{
	loff_t res;

	res = (1UL << 63) -1;
	if (res > MAX_LFS_FILESIZE)
		//res = MAX_LFS_FILESIZE;
		res=(u64)memblock.pstorage.total_size;

	r2nvmm_dbg("max file size %llu bytes\n",res);
	return res;
}
#endif
inline int isdigit(int c)
{
	return '0' <= c && c <= '9' ;
}

int r2nvmm_parse_options(char *options, struct r2nvmm_sb_info *sbi, bool remount)
{
	char *this_char, *value, *rest;
	struct mempolicy *mpol = NULL;
	uid_t uid;
	gid_t gid;

	sbi->mode = R2NVMM_DEFAULT_MODE;

	while (options != NULL) {
		this_char = options;
		for (;;) {
			/*
			 * NUL-terminate this option: unfortunately,
			 * mount options form a comma-separated list,
			 * but mpol's nodelist may also contain commas.
			 */
			options = strchr(options, ',');
			if (options == NULL)
				break;
			options++;
			if (!isdigit(*options)) {
				options[-1] = '\0';
				break;
			}
		}
		if (!*this_char)
			continue;
		if ((value = strchr(this_char,'=')) != NULL) {
			*value++ = 0;
		} else {
			pr_err("r2nvmm: No value for mount option '%s'\n",
			       this_char);
			goto error;
		}

		if (!strcmp(this_char,"size")) {
			unsigned long long size;
			size = memparse(value,&rest);
			if (*rest == '%') {
				size <<= PAGE_SHIFT;
				size *= totalram_pages;
				do_div(size, 100);
				rest++;
			}
			if (*rest)
				goto bad_val;
			sbi->max_blocks =
				DIV_ROUND_UP(size, PAGE_SIZE);
		} else if (!strcmp(this_char,"nr_blocks")) {
			sbi->max_blocks = memparse(value, &rest);
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"nr_inodes")) {
			sbi->max_inodes = memparse(value, &rest);
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"mode")) {
			if (remount)
				continue;
			sbi->mode = simple_strtoul(value, &rest, 8) & 07777;
			//sbi->mode = simple_strtoul(value, &rest, 8) & S_IALLUGO;
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"uid")) {
			if (remount)
				continue;
			uid = simple_strtoul(value, &rest, 0);
			if (*rest)
				goto bad_val;
			sbi->uid = make_kuid(current_user_ns(), uid);
			if (!uid_valid(sbi->uid))
				goto bad_val;
		} else if (!strcmp(this_char,"gid")) {
			if (remount)
				continue;
			gid = simple_strtoul(value, &rest, 0);
			if (*rest)
				goto bad_val;
			sbi->gid = make_kgid(current_user_ns(), gid);
			if (!gid_valid(sbi->gid))
				goto bad_val;
#if 0
#ifdef CONFIG_NUMA
		} else if (!strcmp(this_char,"mpol")) {
			mpol_put(mpol);
			mpol = NULL;
			if (mpol_parse_str(value, &mpol))
				goto bad_val;
#endif
#endif
		} else {
			pr_err("r2nvmm: Bad mount option %s\n", this_char);
			goto error;
		}
	}
	sbi->mpol = mpol;
	return 0;

bad_val:
	pr_err("r2nvmm: Bad value '%s' for mount option '%s'\n",
	       value, this_char);
error:
#if 0
	mpol_put(mpol);
#endif
	return 1;
}
#if 0
struct r2nvmm_inode *r2nvmm_format(struct super_block *sb, unsigned long size)
{
	unsigned long blocksize;
	unsigned long reserved_space, reserved_blocks;
	struct r2nvmm_inode *root_i, *pi;
	struct r2nvmm_super_block *super;
	struct r2nvmm_sb_info *sbi = R2NVMM_SB(sb);

	r2nvmm_info("creating an empty r2nvmm of size %lu\n",size);
	sbi->num_blocks = ((unsigned long)(size) >> PAGE_SHIFT);

	if (!sbi->virt_addr){
		printk(KERN_ERR "ioremap of the r2nvmm image failed(1)\n");
		return ERR_PTR(-EINVAL);
	}

	r2nvmm_dbg("r2nvmm: Default block size set to 4K\n");
	blocksize=sbi->blocksize=R2NVMM_DEF_BLOCK_SIZE_4K;

	r2nvmm_set_blocksize(sb,blocksize);
	blocksize=sb->s_blocksize;

	if (sbi->blocksize && sbi->blocksize != blocksize)
		sbi->blocksize = blocksize;

	/* Reserve space for 8 special inodes */
	reserved_space = R2NVMM_SB_SIZE * 4;
	reserved_blocks = (reserved_space + blocksize -1) / blocksize;
	if (reserved_blocks > sbi->reserverd_blocks) {
		r2nvmm_dbg("Reserved %lu blocks, require %lu blocks. "
				"Increase reserved blocks number.\n",
				sbi->reserved_blocks, reserved_blocks);
		return ERR_PTR(-EINVAL);
	}
	r2nvmm_dbg("max file name len %d\n", (unsigned int)R2NVMM_NAME_LEN);

	super = r2nvmm_get_super(sb);

	/* clear out super-block and inode table */
	memset_nt(super, 0, sbi->reserved_blocks * sbi->blocksize);
	super->s_size = cpu_to_le64(size);
	super->s_blocksize = cpu_to_le32(blocksize);
	super->s_magic = cpu_to_le32(R2NVMM_SUPER_MAGIC);

	r2nvmm_init_blockmap(sb,0);
	
	if (r2nvmm_init_inode_inuse_list(sb) < 0)
		return ERR_PTR(-EINVAL);
	if (r2nvmm_init_inode_table(sb) < 0)
		return ERR_PTR(-EINVAL);

	pi = r2nvmm_get_inode_by_ino(sb, R2NVMM_BLOCKNODE_INO);
	pi->r2nvmm_ino = R2NVMM_BLOCKNODE_INO;
	//r2nvmm_flush_buffer(pi, CACHELINE_SIZE, 1);
}
#endif
int r2nvmm_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	//struct r2nvmm_sb_info *sbi = R2NVMM_SB(dentry->d_sb);

	//buf->f_type	= dentry->d_sb->s_magic;
	buf->f_type	= R2NVMM_SUPER_MAGIC;
	buf->f_bsize	= PAGE_SIZE;
	buf->f_namelen	= NAME_MAX;

	return 0;
}


struct inode *r2nvmm_alloc_inode(struct super_block *sb)
{
	struct r2nvmm_inode_info *info;
	info = kmem_cache_alloc(r2nvmm_inode_cachep, GFP_KERNEL |
			__GFP_THISNODE | __GFP_PSTORAGE);
	//info = kmem_cache_alloc(r2nvmm_inode_cachep, __GFP_THISNODE | __GFP_PSTORAGE);
	//info = kmem_cache_alloc(r2nvmm_inode_cachep, GFP_KERNEL | __GFP_PSTORAGE);
	//info = kmem_cache_alloc(r2nvmm_inode_cachep, GFP_KERNEL);
	//info = kmem_cache_alloc(r2nvmm_inode_cachep, GFP_NOFS);
	if (!info)
		return NULL;
	info->vfs_inode.i_version=1;
	return &info->vfs_inode;
}


void r2nvmm_destroy_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	if (S_ISLNK(inode->i_mode))
		kfree(inode->i_link);
	kmem_cache_free(r2nvmm_inode_cachep, R2NVMM_I(inode));
}

void r2nvmm_destroy_inode(struct inode *inode)
{
#if 0
	if (S_ISREG(inode->i_mode))
		mpol_free_shared_policy(&R2NVMM_I(inode)->policy);
#endif
	call_rcu(&inode->i_rcu, r2nvmm_destroy_callback);
}

static void r2nvmm_put_super(struct super_block *sb)
{
	struct r2nvmm_sb_info *sbi = R2NVMM_SB(sb);
	kfree(sbi);
	sb->s_fs_info=NULL;
}

inline void set_default_opts(struct r2nvmm_sb_info *fsi)
{
//	set_opt(fsi->mount_opts->s_mount_opt, HUGEIOREMAP);
//	set_opt(fsi->mount_opts->s_mount_opt, ERRORS_CONT);
//	fsi->reserved_blocks = RESERVED_BLOCKS;
	fsi->nodes = num_online_nodes();
	fsi->map_id = 0;
}

int r2nvmm_fill_super(struct super_block *sb, void *data, int silent)
{
	struct r2nvmm_sb_info *sbi;
	struct inode *inode;
	int err;
	//int i;
	//struct inode_map *inode_map;

	save_mount_options(sb, data);//generic_show_options()

	//sbi = kzalloc(sizeof(*sbi), __GFP_THISNODE | __GFP_PSTORAGE);
	sbi = kzalloc(sizeof(*sbi), __GFP_THISNODE | GFP_KERNEL | __GFP_PSTORAGE);
	//sbi = kzalloc(sizeof(*sbi), GFP_KERNEL | __GFP_PSTORAGE);
	//sbi = kzalloc(sizeof(*sbi), GFP_KERNEL );
	if (!sbi)
		return -ENOMEM;

	err=r2nvmm_parse_options(data, sbi, 0);
	if (err)
		return err;

	sbi->uid = current_fsuid();
	sbi->gid = current_fsgid();
	sbi->mode = S_IRWXUGO | S_ISVTX;
	sb->s_fs_info = sbi;
	sbi->sb = sb;
	set_default_opts(sbi);

#if 0	
	sbi->mode = (S_IRUGO | S_IXUGO | S_IWUSR);
	if (r2nvmm_get_block_info(sb,sbi))
		goto out;

	get_random_bytes(&random, sizeof(u32));
	atomic_set(&sbi->next_generation, random);
	sbi->shared_free_list.block_free_tree = RB_ROOT;
	spin_lock_init(&sbi->shared_free_list.s_lock);
//	set_opt(sbi->s_mount_opt, DAX);
//	clear_opt(sbi->s_mount_opt, PROTECT);
//	set_opt(sbi->s_mount_opt, HUGEIOREMAP);
#endif

	/* Init with default values */
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_op		= &r2nvmm_sops;
	sb->s_time_gran		= 1;
	sb->s_magic		= R2NVMM_SUPER_MAGIC;
//	sb->s_magic = le32_to_cpu(super->s_magic);
//	sb->s_maxbytes		= MAX_LFS_FILESIZE;
//	sb->s_maxbytes		= r2nvmm_max_size(sb->s_blocksize_bits);
	sb->s_maxbytes		= (u64)memblock.pstorage.total_size;
//	sb->s_export_op		= &r2nvmm_export_ops;
//	sb->s_xattr = NULL;
//	sb->s_flags |= MS_NOSEC;
//	sbi->inode_maps = kzalloc(sbi->nodes * sizeof(struct inode_map),
//					GFP_KERNEL | __GFP_PSTORAGE);
					//GFP_KERNEL);
#if 0
	if (!sbi->inode_maps) {
		err = -ENOMEM;
		pr_info("->inode_maps failed\n");
		goto out;
	}

	for (i = 0; i < sbi->nodes; i++) {
		inode_map = &sbi->inode_maps[i];
		mutex_init(&inode_map->inode_table_mutex);
		inode_map->inode_inuse_tree = RB_ROOT;
	}

	sbi->zeroed_page = kzalloc(PAGE_SIZE, GFP_KERNEL | __GFP_PSTORAGE);
	//sbi->zeroed_page = kzalloc(PAGE_SIZE, GFP_KERNEL );
	if (!sbi->zeroed_page) {
		err = -ENOMEM;
		pr_info("->zeroed_page failed\n");
		goto out;
	}

	/* If the FS was not formatted on this mount, scan the meta-data after
	 * truncate list has been processed */
	if ((sbi->s_mount_opt & NV2NVMM_MOUNT_FORMAT) == 0)
		r2nvmm_recovery(sb);


//	mutex_init(&sbi->s_lock);
	//set_opt(sbi->mount_opts.s_mount_opt, MOUNTING);
#endif
	inode = r2nvmm_get_inode(sb, NULL, S_IFDIR | sbi->mode, 0);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}
	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		pr_info(KERN_ERR "get r2nvmm root inode failed\n");
		err = -ENOMEM;
		goto out;
	}
	inode->i_uid = sbi->uid;
	inode->i_gid = sbi->gid;
#if 0
	if (!(sb->s_flags & MS_RDONLY)) {
		u64 mnt_write_time;
		/* update mount time and write time atomically. */
		mnt_write_time = (get_seconds() & 0xFFFFFFFF);
		mnt_write_time = mnt_write_time | (mnt_write_time << 32);

		r2nvmm_memunlock_range(sb, &super->s_mtime, 8);
		r2nvmm_memcpy_atomic(&super->s_mtime, &mnt_write_time, 8);
		r2nvmm_memlock_range(sb, &super->s_mtime, 8);

		r2nvmm_flush_buffer(&super->s_mtime, 8, false);
		PERSISTENT_MARK();
		PERSISTENT_BARRIER();
	}
#endif

//	clear_opt(sbi->s_mount_opt, MOUNTING);
	err = 0;

	return err;
out:
	if (sbi->zeroed_page) {
		kfree(sbi->zeroed_page);
		sbi->zeroed_page = NULL;
	}

	if (sbi->free_lists) {
		kfree(sbi->free_lists);
		sbi->free_lists = NULL;
	}

	if (sbi->inode_maps) {
		kfree(sbi->inode_maps);
		sbi->inode_maps = NULL;
	}
	r2nvmm_put_super(sb);
	//kfree(sbi);
	return err;
}

struct dentry *r2nvmm_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, r2nvmm_fill_super);
	//return mount_bdev(fs_type, flags, dev_name, data, r2nvmm_fill_super);
}

void r2nvmm_kill_sb(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	kill_litter_super(sb);
}
#if 0
static int r2nvmm_remount_fs(struct super_block *sb, int *flags, char *data)
{
	struct r2nvmm_sb_info *sbinfo = R2NVMM_SB(sb);
	struct r2nvmm_sb_info config = *sbinfo;
	unsigned long inodes;
	int error = -EINVAL;

	config.mpol = NULL;
	if (r2nvmm_parse_options(data, &config, true))
		return error;

	spin_lock(&sbinfo->stat_lock);
	inodes = sbinfo->max_inodes - sbinfo->free_inodes;
	if (percpu_counter_compare(&sbinfo->used_blocks, config.max_blocks) > 0)
		goto out;
	if (config.max_inodes < inodes)
		goto out;
	/*
	 * Those tests disallow limited->unlimited while any are in use;
	 * but we must separately disallow unlimited->limited, because
	 * in that case we have no record of how much is already in use.
	 */
	if (config.max_blocks && !sbinfo->max_blocks)
		goto out;
	if (config.max_inodes && !sbinfo->max_inodes)
		goto out;

	error = 0;
	sbinfo->huge = config.huge;
	sbinfo->max_blocks  = config.max_blocks;
	sbinfo->max_inodes  = config.max_inodes;
	sbinfo->free_inodes = config.max_inodes - inodes;

	/*
	 * Preserve previous mempolicy unless mpol remount option was specified.
	 */
	if (config.mpol) {
		mpol_put(sbinfo->mpol);
		sbinfo->mpol = config.mpol;	/* transfers initial ref */
	}
out:
	spin_unlock(&sbinfo->stat_lock);
	return error;
}
#endif

static void r2nvmm_init_inode(void *foo)
{
	struct r2nvmm_inode_info *info=foo;
	inode_init_once(&info->vfs_inode);
}

static int __init init_inodecache(void)
{
	r2nvmm_inode_cachep = kmem_cache_create("r2nvmm_inode_cache",
			sizeof(struct r2nvmm_inode_info), 0,
			SLAB_PANIC|SLAB_ACCOUNT, r2nvmm_init_inode);
	if (r2nvmm_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(r2nvmm_inode_cachep);
}
#if 0
static int __init init_rangenode_cache(void)
{
	r2nvmm_range_node_cachep = kmem_cache_create("r2nvmm_range_node_cache",
			sizeof(struct r2nvmm_range_node),
			0, SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD, NULL);
	if ( r2nvmm_range_node_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_rangenode_cache(void)
{
	kmem_cache_destroy(r2nvmm_range_node_cachep);
}
#endif
#if 0
static int r2nvmm_show_options(struct seq_file *seq, struct dentry *root)
{
	struct r2nvmm_sb_info *sbinfo = R2NVMM_SB(root->d_sb);

	if (sbinfo->max_blocks != r2nvmm_default_max_blocks())
		seq_printf(seq, ",size=%luk",
			sbinfo->max_blocks << (PAGE_SHIFT - 10));
	if (sbinfo->max_inodes != r2nvmm_default_max_inodes())
		seq_printf(seq, ",nr_inodes=%lu", sbinfo->max_inodes);
	if (sbinfo->mode != (S_IRWXUGO | S_ISVTX))
		seq_printf(seq, ",mode=%03ho", sbinfo->mode);
	if (!uid_eq(sbinfo->uid, GLOBAL_ROOT_UID))
		seq_printf(seq, ",uid=%u",
				from_kuid_munged(&init_user_ns, sbinfo->uid));
	if (!gid_eq(sbinfo->gid, GLOBAL_ROOT_GID))
		seq_printf(seq, ",gid=%u",
				from_kgid_munged(&init_user_ns, sbinfo->gid));
	r2nvmm_show_mpol(seq, sbinfo->mpol);
	return 0;
}
#endif
const struct super_operations r2nvmm_sops = {
	.drop_inode	= generic_delete_inode,
//	.alloc_inode	= r2nvmm_alloc_inode,
//	.destroy_inode	= r2nvmm_destroy_inode,
//	.write_inode	= r2nvmm_write_inode,
//	.dirty_inode	= r2nvmm_dirty_inode,
//	.put_super	= r2nvmm_put_super,
//	.remount_fs	= r2nvmm_remount,
//	.statfs		= r2nvmm_statfs,
//	.show_options	= r2nvmm_show_options,
	.show_options	= generic_show_options,
};

struct file_system_type r2nvmm_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "r2nvmm",
	.mount		= r2nvmm_mount,
	.kill_sb	= r2nvmm_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

int __init r2nvmm_init(void)
{
	int err;

	r2nvmm_proc_root = proc_mkdir(proc_dirname, NULL);
#if 0
	err=init_rangenode_cache();
	if(err){
		pr_err("init_rangenode_cache error - r2nvmm\n");
		goto out1;
	}
#endif

	err=init_inodecache();
	if (err){
		pr_err("init_inodecache error - r2nvmm\n");
		goto out1;
	}

	err=register_filesystem(&r2nvmm_fs_type);
	if(err){
		pr_err("register_filesystem error - r2nvmm\n");
		goto out2;
	}

	r2nvmm_mnt= kern_mount(&r2nvmm_fs_type);
	if (IS_ERR(r2nvmm_mnt)){
		err=PTR_ERR(r2nvmm_mnt);
		goto out2;
	}

	return 0;
out2:
	destroy_inodecache();
out1:
//	destroy_rangenode_cache();
	remove_proc_entry(proc_dirname, NULL);
	return err;
}

void __exit r2nvmm_exit(void)
{

	unregister_filesystem(&r2nvmm_fs_type);
	remove_proc_entry(proc_dirname, NULL);
	destroy_inodecache();
//	destroy_rangenode_cache();
}

MODULE_AUTHOR("Yongseob");
MODULE_DESCRIPTION("R2NVMM: A Persistent Memory File System");
MODULE_LICENSE("GPL");

module_init(r2nvmm_init);
module_exit(r2nvmm_exit);

