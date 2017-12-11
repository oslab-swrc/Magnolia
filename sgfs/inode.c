/*
 * Resizable simple ram filesystem for Linux.
 *
 * Copyright (C) 2000 Linus Torvalds.
 *               2000 Transmeta Corp.
 *
 * Usage limits added by David Gibson, Linuxcare Australia.
 * This file is released under the GPL.
 */

/*
 * NOTE! This filesystem is probably most useful
 * not as a real filesystem, but as an example of
 * how virtual filesystems can be written.
 *
 * It doesn't get much simpler than this. Consider
 * that this file implements the full semantics of
 * a POSIX-compliant read-write filesystem.
 *
 * Note in particular how the filesystem does not
 * need to implement any data structures of its own
 * to keep track of the virtual data: using the VFS
 * caches is sufficient.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/sched.h>
#include <linux/parser.h>
//#include <linux/magic.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include "r2nvmm.h"

extern struct inode *new_inode(struct super_block *sb);
const struct address_space_operations r2nvmm_aops;

int set_page_dirty_no_writeback(struct page *page)
{
	if (!PageDirty(page))
		return !TestSetPageDirty(page);
	return 0;
}

const struct address_space_operations r2nvmm_aops = {
	.readpage	= r2nvmm_readpage,
	.write_begin	= r2nvmm_write_begin,
	.write_end	= r2nvmm_write_end,
	//.set_page_dirty	= __set_page_dirty_no_writeback,
	.set_page_dirty	= set_page_dirty_no_writeback,
			// For address_space which do not use buffers nor write back
//	.direct_IO	= r2nvmm_direct_IO,
};

struct inode *r2nvmm_get_inode(struct super_block *sb, 
		const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode *inode=new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(inode, dir, mode);
		inode->i_blocks = 0;
		inode->i_generation = get_seconds();
		inode->i_mapping->a_ops = &r2nvmm_aops;
		//mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		mapping_set_gfp_mask(inode->i_mapping, __GFP_PSTORAGE);
		mapping_set_unevictable(inode->i_mapping);
		inode->i_atime= inode->i_mtime = inode->i_ctime = current_time(inode);
		switch(mode & S_IFMT){
		default:
			init_special_inode(inode,mode,dev);
			break;
		case S_IFREG:
			inode->i_op=&r2nvmm_file_inode_operations;
			inode->i_fop=&r2nvmm_file_operations;
			break;
		case S_IFDIR:
			inode->i_op=&r2nvmm_dir_inode_operations;
			inode->i_fop=&simple_dir_operations;
			inc_nlink(inode);
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			inode_nohighmem(inode);
			break;
		}
	}
	return inode;
}

int r2nvmm_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode = r2nvmm_get_inode(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC;

	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);
		error = 0;
		dir->i_mtime = dir->i_ctime=current_time(dir);
	}
	return error;
}

int r2nvmm_readpage(struct file *file, struct page *page)
{
	clear_highpage(page);
	flush_dcache_page(page);
	SetPageUptodate(page);
	unlock_page(page);
	return 0;
}

int r2nvmm_write_begin(struct file *file, struct address_space *mapping, loff_t pos,
		unsigned len, unsigned flags, struct page **pagep, void **fsdata)
{
	struct page *page;
	pgoff_t index;

	index = pos >> PAGE_SHIFT;

	page = grab_cache_page_write_begin(mapping, index, flags);
	if (!page)
		return -ENOMEM;

	*pagep = page;

	if (!PageUptodate(page) && ( len != PAGE_SIZE)) {
		unsigned from = pos & (PAGE_SIZE-1);

		zero_user_segments(page, 0, from, from + len, PAGE_SIZE);
	}
	return 0;
}

int r2nvmm_write_end(struct file *file, struct address_space *mapping, loff_t pos,
		unsigned len, unsigned copied, struct page *page, void *fsdata)
{
	struct inode *inode = page->mapping->host;
	loff_t last_pos = pos + copied;

	if (copied < len){
		unsigned from = pos & (PAGE_SIZE -1);
		zero_user(page, from + copied, len-copied);
	}

	if (!PageUptodate(page))
		SetPageUptodate(page);

	if (last_pos>inode->i_size)
		i_size_write(inode, last_pos);

	set_page_dirty(page);
	unlock_page(page);
	put_page(page);

	return copied;
}

#if 0
ssize_t r2nvmm_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *filp = iocb->ki_filp;
	struct adderss_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;

	return dax_do_io(iocb, inode, iter, r2nvmm_dax_get_block, NULL, DIO_LOCKING);
}
#endif
/*
 * File creation. Allocate an inode, and we're done..
 */
/* SMP-safe */

int r2nvmm_mkdir(struct inode * dir, struct dentry * dentry, umode_t mode)
{
	int retval = r2nvmm_mknod(dir, dentry, mode | S_IFDIR, 0);
	if (!retval)
		inc_nlink(dir);
	return retval;
}

int r2nvmm_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	return r2nvmm_mknod(dir, dentry, mode | S_IFREG, 0);
}

struct dentry *r2nvmm_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	return simple_lookup(dir, dentry, flags);
}

int r2nvmm_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	return simple_link(old_dentry, dir, dentry);
}

int r2nvmm_unlink(struct inode *dir, struct dentry *dentry)
{
	return simple_unlink(dir, dentry);
}

int r2nvmm_symlink(struct inode * dir, struct dentry *dentry, const char * symname)
{
	struct inode *inode;
	int error = -ENOSPC;

	inode = r2nvmm_get_inode(dir->i_sb, dir, S_IFLNK|S_IRWXUGO, 0);
	if (inode) {
		int l = strlen(symname)+1;
		error = page_symlink(inode, symname, l);
		if (!error) {
			d_instantiate(dentry, inode);
			dget(dentry);
			dir->i_mtime = dir->i_ctime = CURRENT_TIME;
		} else
			iput(inode);
	}
	return error;
}

int r2nvmm_rmdir(struct inode *dir, struct dentry *dentry)
{
	if (!simple_empty(dentry))
		return -ENOTEMPTY;

	drop_nlink(d_inode(dentry));
	r2nvmm_unlink(dir, dentry);
	drop_nlink(dir);
	return 0;
}

int r2nvmm_rename(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry,
		unsigned int flags)
{
	return simple_rename(old_dir, old_dentry, new_dir, new_dentry, flags);
}

const struct inode_operations r2nvmm_dir_inode_operations = {
	.create		= r2nvmm_create,
	.lookup		= r2nvmm_lookup,
	.link		= r2nvmm_link,
	.unlink		= r2nvmm_unlink,
	.symlink	= r2nvmm_symlink,
	.mkdir		= r2nvmm_mkdir,
	.rmdir		= r2nvmm_rmdir,
	.mknod		= r2nvmm_mknod,
	.rename		= r2nvmm_rename,
};

