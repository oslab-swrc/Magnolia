/* file.c: r2nvmm file operations
 *
 * Completely Enough Yongseob
 * Persistent memory file operations
 *
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
#include <linux/mm.h>
#include "r2nvmm.h"


#define iterate_iovec(i, n, __v, __p, skip, STEP) {	\
	size_t left;					\
	size_t wanted = n;				\
	__p = i->iov;					\
	__v.iov_len = min(n, __p->iov_len - skip);	\
	if (likely(__v.iov_len)) {			\
		__v.iov_base = __p->iov_base + skip;	\
		left = (STEP);				\
		__v.iov_len -= left;			\
		skip += __v.iov_len;			\
		n -= __v.iov_len;			\
	} else {					\
		left = 0;				\
	}						\
	while (unlikely(!left && n)) {			\
		__p++;					\
		__v.iov_len = min(n, __p->iov_len);	\
		if (unlikely(!__v.iov_len))		\
			continue;			\
		__v.iov_base = __p->iov_base;		\
		left = (STEP);				\
		__v.iov_len -= left;			\
		skip = __v.iov_len;			\
		n -= __v.iov_len;			\
	}						\
	n = wanted - n;					\
}

#define iterate_kvec(i, n, __v, __p, skip, STEP) {	\
	size_t wanted = n;				\
	__p = i->kvec;					\
	__v.iov_len = min(n, __p->iov_len - skip);	\
	if (likely(__v.iov_len)) {			\
		__v.iov_base = __p->iov_base + skip;	\
		(void)(STEP);				\
		skip += __v.iov_len;			\
		n -= __v.iov_len;			\
	}						\
	while (unlikely(n)) {				\
		__p++;					\
		__v.iov_len = min(n, __p->iov_len);	\
		if (unlikely(!__v.iov_len))		\
			continue;			\
		__v.iov_base = __p->iov_base;		\
		(void)(STEP);				\
		skip = __v.iov_len;			\
		n -= __v.iov_len;			\
	}						\
	n = wanted;					\
}

#define iterate_bvec(i, n, __v, __bi, skip, STEP) {	\
	struct bvec_iter __start;			\
	__start.bi_size = n;				\
	__start.bi_bvec_done = skip;			\
	__start.bi_idx = 0;				\
	for_each_bvec(__v, i->bvec, __bi, __start) {	\
		if (!__v.bv_len)			\
			continue;			\
		(void)(STEP);				\
	}						\
}

#define iterate_all_kinds(i, n, v, I, B, K) {			\
	size_t skip = i->iov_offset;				\
	if (unlikely(i->type & ITER_BVEC)) {			\
		struct bio_vec v;				\
		struct bvec_iter __bi;				\
		iterate_bvec(i, n, v, __bi, skip, (B))		\
	} else if (unlikely(i->type & ITER_KVEC)) {		\
		const struct kvec *kvec;			\
		struct kvec v;					\
		iterate_kvec(i, n, v, kvec, skip, (K))		\
	} else {						\
		const struct iovec *iov;			\
		struct iovec v;					\
		iterate_iovec(i, n, v, iov, skip, (I))		\
	}							\
}


static void memcpy_from_page(char *to, struct page *page, size_t offset, size_t len)
{
	char *from = kmap_atomic(page);
	memcpy(to, from + offset, len);
	kunmap_atomic(from);
}
#if 0
static void memcpy_to_page(struct page *page, size_t offset, const char *from, size_t len)
{
	char *to = kmap_atomic(page);
	memcpy(to + offset, from, len);
	kunmap_atomic(to);
}
#endif
size_t iov_iter_copy_from_user_atomic_nocache_PS(struct page *page,
		struct iov_iter *i, unsigned long offset, size_t bytes)
{
	char *kaddr=kmap_atomic(page), *p=kaddr+offset;
	if (unlikely(i->type & ITER_PIPE)){
		kunmap_atomic(kaddr);
		WARN_ON(1);
		return 0;
	}
	iterate_all_kinds(i, bytes, v,
			__copy_from_user_inatomic_nocache((p += v.iov_len) - v.iov_len,
				v.iov_base, v.iov_len),
			memcpy_from_page(( p += v.bv_len) -v.bv_len, v.bv_page,
				v.bv_offset, v.bv_len),
			memcpy((p += v.iov_len) - v.iov_len, v.iov_base, v.iov_len)
			)
		kunmap_atomic(kaddr);
	return bytes;
}

ssize_t r2nvmm_perform_write(struct file *file, struct iov_iter *i,
		loff_t pos)
{
	struct address_space *mapping = file->f_mapping;
	const struct address_space_operations *a_ops=mapping->a_ops;
	long status=0;
	ssize_t written=0;
	unsigned int flags=0;

	if (!iter_is_iovec(i))
		flags |=AOP_FLAG_UNINTERRUPTIBLE;
	do{
		struct page *page;
		unsigned long offset;	/* offset into storage page */
		unsigned long bytes;	/* Bytes to write to pages */
		size_t copied;		/* Bytes copied from user */
		void *fsdata;

		offset = (pos & (PAGE_SIZE -1));
		bytes= min_t(unsigned long, PAGE_SIZE-offset,
				i->count);

again:
		if (unlikely(iov_iter_fault_in_readable(i,bytes))) {
			status=-EFAULT;
			break;
		}

		if (fatal_signal_pending(current)){
			status=-EINTR;
			break;
		}
		status=a_ops->write_begin(file,mapping,pos,bytes,flags,
				&page,&fsdata);
		if(unlikely(status<0))
			break;
		if(mapping_writably_mapped(mapping))
			flush_dcache_page(page);

		copied=iov_iter_copy_from_user_atomic_nocache_PS(page,i,offset,bytes);
		flush_dcache_page(page);

		status=a_ops->write_end(file,mapping,pos,bytes,copied,
				page,fsdata);
		if(unlikely(status<0))
			break;
		copied=status;

		cond_resched();

		iov_iter_advance(i,copied);
		if (unlikely(copied==0)){
			bytes=min_t(unsigned long, PAGE_SIZE-offset,
					iov_iter_single_seg_count(i));
			goto again;
		}
		pos +=copied;
		written +=copied;

		//balance_dirty_pages_ratelimited(mapping);
		//memory storage need not to be balanced
	}while(i->count);

	return written ? written : status;
}

ssize_t __r2nvmm_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file=iocb->ki_filp;
	//struct address_space *mapping=file->f_mapping;
	//struct inode *inode=mapping->host;
	ssize_t written=0;
	ssize_t err;
	//ssize_t status;

	//	current->backing_dev_info=inode_to_bdi(inode);
	//we have no backing device
	err=file_remove_privs(file);
	if(err)
		goto out;

	err=file_update_time(file);
	if (err)
		goto out;

	written = r2nvmm_perform_write(file, from, iocb->ki_pos);
	if (likely(written)>0)
		iocb->ki_pos +=written;

out:
	current->backing_dev_info=NULL;
	return written ? written : err;
}

static ssize_t r2nvmm_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	ssize_t ret;

	inode_lock(inode);
	ret=generic_write_checks(iocb,from);
	if(ret>0)
		ret=__r2nvmm_file_write_iter(iocb,from);
	inode_unlock(inode);

#if 0
	if(ret>0)
		ret=generic_write_sync(iocb,ret);
	//we do not need sync op, 
	//because we write directly to the memory storage
#endif
	return ret;
}

loff_t r2nvmm_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode * inode= file->f_mapping->host;

	return generic_file_llseek_size(file, offset, whence,
			inode->i_sb->s_maxbytes,
			i_size_read(inode));
}

#if 0
static long r2nvmm_fallocate(struct file *file, int mode, loff_t offset,
							 loff_t len)
{
	struct inode *inode = file_inode(file);
	struct r2nvmm_sb_info *sbinfo = R2NVMM_SB(inode->i_sb);
	struct r2nvmm_inode_info *info = R2NVMM_I(inode);
	struct r2nvmm_falloc r2nvmm_falloc;
	pgoff_t start, index, end;
	int error;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP;

	inode_lock(inode);

	if (mode & FALLOC_FL_PUNCH_HOLE) {
		struct address_space *mapping = file->f_mapping;
		loff_t unmap_start = round_up(offset, PAGE_SIZE);
		loff_t unmap_end = round_down(offset + len, PAGE_SIZE) - 1;
		DECLARE_WAIT_QUEUE_HEAD_ONSTACK(r2nvmm_falloc_waitq);

		/* protected by i_mutex */
		if (info->seals & F_SEAL_WRITE) {
			error = -EPERM;
			goto out;
		}

		r2nvmm_falloc.waitq = &r2nvmm_falloc_waitq;
		r2nvmm_falloc.start = unmap_start >> PAGE_SHIFT;
		r2nvmm_falloc.next = (unmap_end + 1) >> PAGE_SHIFT;
		spin_lock(&inode->i_lock);
		inode->i_private = &r2nvmm_falloc;
		spin_unlock(&inode->i_lock);

		if ((u64)unmap_end > (u64)unmap_start)
			unmap_mapping_range(mapping, unmap_start,
					    1 + unmap_end - unmap_start, 0);
		r2nvmm_truncate_range(inode, offset, offset + len - 1);
		/* No need to unmap again: hole-punching leaves COWed pages */

		spin_lock(&inode->i_lock);
		inode->i_private = NULL;
		wake_up_all(&r2nvmm_falloc_waitq);
		WARN_ON_ONCE(!list_empty(&r2nvmm_falloc_waitq.task_list));
		spin_unlock(&inode->i_lock);
		error = 0;
		goto out;
	}

	/* We need to check rlimit even when FALLOC_FL_KEEP_SIZE */
	error = inode_newsize_ok(inode, offset + len);
	if (error)
		goto out;

	if ((info->seals & F_SEAL_GROW) && offset + len > inode->i_size) {
		error = -EPERM;
		goto out;
	}

	start = offset >> PAGE_SHIFT;
	end = (offset + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	/* Try to avoid a swapstorm if len is impossible to satisfy */
	if (sbinfo->max_blocks && end - start > sbinfo->max_blocks) {
		error = -ENOSPC;
		goto out;
	}

	r2nvmm_falloc.waitq = NULL;
	r2nvmm_falloc.start = start;
	r2nvmm_falloc.next  = start;
	r2nvmm_falloc.nr_falloced = 0;
	r2nvmm_falloc.nr_unswapped = 0;
	spin_lock(&inode->i_lock);
	inode->i_private = &r2nvmm_falloc;
	spin_unlock(&inode->i_lock);

	for (index = start; index < end; index++) {
		struct page *page;

		/*
		 * Good, the fallocate(2) manpage permits EINTR: we may have
		 * been interrupted because we are using up too much memory.
		 */
		if (signal_pending(current))
			error = -EINTR;
		else if (r2nvmm_falloc.nr_unswapped > r2nvmm_falloc.nr_falloced)
			error = -ENOMEM;
		else
			error = r2nvmm_getpage(inode, index, &page, SGP_FALLOC);
		if (error) {
			/* Remove the !PageUptodate pages we added */
			if (index > start) {
				r2nvmm_undo_range(inode,
				    (loff_t)start << PAGE_SHIFT,
				    ((loff_t)index << PAGE_SHIFT) - 1, true);
			}
			goto undone;
		}

		/*
		 * Inform r2nvmm_writepage() how far we have reached.
		 * No need for lock or barrier: we have the page lock.
		 */
		r2nvmm_falloc.next++;
		if (!PageUptodate(page))
			r2nvmm_falloc.nr_falloced++;

		/*
		 * If !PageUptodate, leave it that way so that freeable pages
		 * can be recognized if we need to rollback on error later.
		 * But set_page_dirty so that memory pressure will swap rather
		 * than free the pages we are allocating (and SGP_CACHE pages
		 * might still be clean: we now need to mark those dirty too).
		 */
		set_page_dirty(page);
		unlock_page(page);
		put_page(page);
		cond_resched();
	}

	if (!(mode & FALLOC_FL_KEEP_SIZE) && offset + len > inode->i_size)
		i_size_write(inode, offset + len);
	inode->i_ctime = current_time(inode);
undone:
	spin_lock(&inode->i_lock);
	inode->i_private = NULL;
	spin_unlock(&inode->i_lock);
out:
	inode_unlock(inode);
	return error;
}
#endif

const struct file_operations r2nvmm_file_operations = {
	.read_iter	= generic_file_read_iter,
	.write_iter	= r2nvmm_file_write_iter,
	//	.read		= r2nvmm_file_read,
	//	.write		= r2nvmm_file_write,
	//	.mmap		= r2nvmm_file_mmap,
	.fsync		= noop_fsync,
		.llseek		= r2nvmm_file_llseek,
	//.fallocate		= r2nvmm_fallocate,
	.splice_read		= generic_file_splice_read,
	.splice_write		= iter_file_splice_write,
};

const struct inode_operations r2nvmm_file_inode_operations = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
	//	.setattr	= r2nvmm_setattr,
	//	.getattr	= r2nvmm_getattr,
	.get_acl	= NULL,
};

