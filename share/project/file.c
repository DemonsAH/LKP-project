// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/printk.h>


#include "ouichefs.h"
#include "bitmap.h"

/*
 * Map the buffer_head passed in argument with the iblock-th block of the file
 * represented by inode. If the requested block is not allocated and create is
 * true, allocate a new block on disk and map it.
 */
static int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
				   struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	int ret = 0, bno;

	/* If block number exceeds filesize, fail */
	if (iblock >= OUICHEFS_BLOCK_SIZE >> 2)
		return -EFBIG;

	/* Read index block from disk */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/*
	 * Check if iblock is already allocated. If not and create is true,
	 * allocate it. Else, get the physical block number.
	 */
	if (index->blocks[iblock] == 0) {
		if (!create) {
			ret = 0;
			goto brelse_index;
		}
		bno = get_free_block(sbi);
		if (!bno) {
			ret = -ENOSPC;
			goto brelse_index;
		}
		index->blocks[iblock] = cpu_to_le32(bno);
		mark_buffer_dirty(bh_index);
	} else {
		bno = le32_to_cpu(index->blocks[iblock]);
	}

	/* Map the physical block to the given buffer_head */
	map_bh(bh_result, sb, bno);

brelse_index:
	brelse(bh_index);

	return ret;
}

/*
 * Called by the page cache to read a page from the physical disk and map it in
 * memory.
 */
static void ouichefs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ouichefs_file_get_block);
}

/*
 * Called by the page cache to write a dirty page to the physical disk (when
 * sync is called or when memory is needed).
 */
static int ouichefs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, ouichefs_file_get_block, wbc);
}

/*
 * Called by the VFS when a write() syscall occurs on file before writing the
 * data in the page cache. This functions checks if the write will be able to
 * complete and allocates the necessary blocks through block_write_begin().
 */
static int ouichefs_write_begin(struct file *file,
				struct address_space *mapping, loff_t pos,
				unsigned int len, struct page **pagep,
				void **fsdata)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(file->f_inode->i_sb);
	int err;
	uint32_t nr_allocs = 0;

	/* Check if the write can be completed (enough space?) */
	if (pos + len > OUICHEFS_MAX_FILESIZE)
		return -ENOSPC;
	nr_allocs = max(pos + len, file->f_inode->i_size) / OUICHEFS_BLOCK_SIZE;
	if (nr_allocs > file->f_inode->i_blocks - 1)
		nr_allocs -= file->f_inode->i_blocks - 1;
	else
		nr_allocs = 0;
	if (nr_allocs > sbi->nr_free_blocks)
		return -ENOSPC;

	/* prepare the write */
	err = block_write_begin(mapping, pos, len, pagep,
				ouichefs_file_get_block);
	/* if this failed, reclaim newly allocated blocks */
	if (err < 0) {
		pr_err("%s:%d: newly allocated blocks reclaim not implemented yet\n",
		       __func__, __LINE__);
	}
	return err;
}

/*
 * Called by the VFS after writing data from a write() syscall to the page
 * cache. This functions updates inode metadata and truncates the file if
 * necessary.
 */
static int ouichefs_write_end(struct file *file, struct address_space *mapping,
			      loff_t pos, unsigned int len, unsigned int copied,
			      struct page *page, void *fsdata)
{
	int ret;
	struct inode *inode = file->f_inode;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;

	/* Complete the write() */
	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len) {
		pr_err("%s:%d: wrote less than asked... what do I do? nothing for now...\n",
		       __func__, __LINE__);
	} else {
		uint32_t nr_blocks_old = inode->i_blocks;

		/* Update inode metadata */
		inode->i_blocks = (roundup(inode->i_size, OUICHEFS_BLOCK_SIZE) /
				   OUICHEFS_BLOCK_SIZE) +
				  1;
		inode->i_mtime = inode->i_ctime = current_time(inode);
		mark_inode_dirty(inode);

		/* If file is smaller than before, free unused blocks */
		if (nr_blocks_old > inode->i_blocks) {
			int i;
			struct buffer_head *bh_index;
			struct ouichefs_file_index_block *index;

			/* Free unused blocks from page cache */
			truncate_pagecache(inode, inode->i_size);

			/* Read index block to remove unused blocks */
			bh_index = sb_bread(sb, ci->index_block);
			if (!bh_index) {
				pr_err("failed truncating '%s'. we just lost %llu blocks\n",
				       file->f_path.dentry->d_name.name,
				       nr_blocks_old - inode->i_blocks);
				goto end;
			}
			index = (struct ouichefs_file_index_block *)
					bh_index->b_data;

			for (i = inode->i_blocks - 1; i < nr_blocks_old - 1;
			     i++) {
				put_block(OUICHEFS_SB(sb), le32_to_cpu(index->blocks[i]));
				index->blocks[i] = 0;
			}
			mark_buffer_dirty(bh_index);
			brelse(bh_index);
		}
	}
end:
	return ret;
}

const struct address_space_operations ouichefs_aops = {
	.readahead = ouichefs_readahead,
	.writepage = ouichefs_writepage,
	.write_begin = ouichefs_write_begin,
	.write_end = ouichefs_write_end
};

static int ouichefs_open(struct inode *inode, struct file *file)
{
	bool wronly = (file->f_flags & O_WRONLY) != 0;
	bool rdwr = (file->f_flags & O_RDWR) != 0;
	bool trunc = (file->f_flags & O_TRUNC) != 0;
	inode->i_fop = &ouichefs_file_ops; // 1.6 change fixing ioctl bug

	if ((wronly || rdwr) && trunc && (inode->i_size != 0)) {
		struct super_block *sb = inode->i_sb;
		struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
		struct ouichefs_file_index_block *index;
		struct buffer_head *bh_index;
		sector_t iblock;

		/* Read index block from disk */
		bh_index = sb_bread(sb, ci->index_block);
		if (!bh_index)
			return -EIO;
		index = (struct ouichefs_file_index_block *)bh_index->b_data;

		for (iblock = 0; index->blocks[iblock] != 0; iblock++) {
			put_block(sbi, le32_to_cpu(index->blocks[iblock]));
			index->blocks[iblock] = 0;
		}
		inode->i_size = 0;
		inode->i_blocks = 1;

		mark_buffer_dirty(bh_index);
		brelse(bh_index);
	}

	return 0;
}

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/uio.h>

ssize_t ouichefs_read(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *filp = iocb->ki_filp;
	struct inode *inode = file_inode(filp);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	loff_t pos = iocb->ki_pos;
	size_t count = iov_iter_count(to);

	if (pos >= inode->i_size)
		return 0;

	// support only small files stored in slice format
	if (ci->index_block == 0)
		return 0;

	uint32_t block_no = ci->index_block & ((1 << 27) - 1);
	uint32_t slice_start = ci->index_block >> 27;

	struct buffer_head *bh = sb_bread(sb, block_no);
	if (!bh)
		return -EIO;

	size_t copied = 0;
	while (count > 0 && pos < inode->i_size) {
		size_t slice_offset = pos % 128;
		uint32_t slice_index = slice_start + (pos / 128);
		void *src = bh->b_data + slice_index * 128 + slice_offset;

		size_t remain = inode->i_size - pos;
		size_t in_slice = 128 - slice_offset;
		size_t to_copy = min3(count, remain, in_slice);

		if (copy_to_iter(src, to_copy, to) != to_copy) {
			brelse(bh);
			return -EFAULT;
		}

		pos += to_copy;
		copied += to_copy;
		count -= to_copy;
	}

	iocb->ki_pos = pos;
	brelse(bh);
	return copied;
}

// 1.8 NEW CODE(1.10 updated for multi slice)
int convert_slice_to_block(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

	uint32_t raw = ci->index_block;
	uint32_t slice_no = raw >> 27;
	uint32_t slice_block = raw & ((1 << 27) - 1);
	size_t size = inode->i_size;

    printk(KERN_INFO "[OuicheFS] Entering convert_slice_to_block()\n");
    printk(KERN_INFO "[OuicheFS] old size = %lld\n", inode->i_size);


	struct buffer_head *bh_slice = sb_bread(sb, slice_block);
	if (!bh_slice) {
		printk(KERN_INFO "[OuicheFS] sb_bread failed for slice block %u\n", slice_block);
		return -EIO;
	}
	// Copy full file content from slices
	char *buffer = kmalloc(size, GFP_KERNEL);
	if (!buffer) {
		printk(KERN_INFO "[OuicheFS] kmalloc failed for size %zu\n", size);
		brelse(bh_slice);
		return -ENOMEM;
	}

	size_t copied = 0;
	while (copied < size) {
		size_t slice_index = slice_no + (copied / 128);
		size_t offset = copied % 128;
		size_t to_copy = min_t(size_t, 128 - offset, size - copied);
		void *src = bh_slice->b_data + slice_index * 128 + offset;
		memcpy(buffer + copied, src, to_copy);
		copied += to_copy;
	}

	brelse(bh_slice);
	release_slice(inode); // clear old slice allocation

	/* update super block data */
//	sbi->small_files--;

	// Allocate new index + data block
	uint32_t index_block = ouichefs_alloc_block(sb);
	if (!index_block) {
		kfree(buffer);
		return -ENOSPC;
	}

	struct buffer_head *bh_index = sb_getblk(sb, index_block);
	if (!bh_index) {
		kfree(buffer);
		return -EIO;
	}

	struct ouichefs_file_index_block *index = (void *)bh_index->b_data;
	memset(index, 0, sizeof(*index));

	uint32_t data_block = ouichefs_alloc_block(sb);
	if (!data_block) {
		put_block(sbi, index_block);
		brelse(bh_index);
		kfree(buffer);
		return -ENOSPC;
	}

	index->blocks[0] = cpu_to_le32(data_block);
	mark_buffer_dirty(bh_index);
	sync_dirty_buffer(bh_index);
	brelse(bh_index);

	struct buffer_head *bh_data = sb_getblk(sb, data_block);
	if (!bh_data) {
		kfree(buffer);
		return -EIO;
	}

	memset(bh_data->b_data, 0, OUICHEFS_BLOCK_SIZE);
	memcpy(bh_data->b_data, buffer, size);
	mark_buffer_dirty(bh_data);
	sync_dirty_buffer(bh_data);
	brelse(bh_data);

	// Update inode
	ci->index_block = index_block;
	inode->i_blocks = 2;
	mark_inode_dirty(inode);

	kfree(buffer);
	return 0;
}

ssize_t ouichefs_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *filp = iocb->ki_filp;
	struct inode *inode = file_inode(filp);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	size_t count = iov_iter_count(from);
	/* update super block info */
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	loff_t old_size = inode->i_size;

	printk(KERN_INFO ">>>> ouichefs_write called, size = %zu\n", iov_iter_count(from));
	printk(KERN_INFO ">>> ouichefs_write: ci->index_block = %u\n", ci->index_block);


	// === 1.8 legacy fallback ===
	if (count > OUICHEFS_MAX_FILESIZE)
		return -EFBIG;

	// if file was a slice and now enlarged → convert to traditional block
	if (count > 128 && inode->i_size <= 128 && ci->index_block != 0) {
		int ret = convert_slice_to_block(inode);
		if (ret < 0)
			return ret;
		// fall through to legacy writer (not shown here)
	}

	// === Allocate temporary buffer ===
	void *kbuf = kmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_iter(kbuf, count, from) != count) {
		kfree(kbuf);
		return -EFAULT;
	}

	// === 1.10: Multi-slice write (count <= 4096) ===
	size_t num_slices = DIV_ROUND_UP(count, 128);
	if (num_slices > 31) {
		kfree(kbuf);
		return -EFBIG;
	}

	uint32_t block_no = 0, slice_start = 0;
	struct buffer_head *bh;
//	struct ouichefs_sb_info *sbi = sb->s_fs_info;
	int found = 0;

	// search partially filled blocks
	uint32_t curr = sbi->s_free_sliced_blocks;
	while (curr) {
		bh = sb_bread(sb, curr);
		if (!bh) break;
		struct ouichefs_sliced_block_meta *meta = (void *)bh->b_data;
		uint32_t bitmap = le32_to_cpu(meta->slice_bitmap);

		for (int i = 1; i <= 32 - num_slices; i++) {
			if ((bitmap & (((1 << num_slices) - 1) << i)) == (((1 << num_slices) - 1) << i)) {
				// enough free slices
				block_no = curr;
				slice_start = i;
				bitmap &= ~(((1 << num_slices) - 1) << i);
				meta->slice_bitmap = cpu_to_le32(bitmap);

				mark_buffer_dirty(bh);
				sync_dirty_buffer(bh);
				brelse(bh);
				found = 1;
				goto slice_allocated;
			}
		}

		uint32_t next = le32_to_cpu(meta->next_partial_block);
		brelse(bh);
		curr = next;
	}

	// allocate new sliced block
	if (!found) {
		block_no = ouichefs_alloc_block(sb);
		if (!block_no) {
			kfree(kbuf);
			return -ENOSPC;
		}

		if (unlikely(block_no == 0)) {
			kfree(kbuf);
			pr_err("ouichefs: allocator returned block 0! abort.\n");
			return -EUCLEAN;
		}

		bh = sb_bread(sb, block_no);
		if (!bh) {
			kfree(kbuf);
			return -EIO;
		}

		memset(bh->b_data, 0, 128);
		struct ouichefs_sliced_block_meta *meta = (struct ouichefs_sliced_block_meta *)bh->b_data;

		uint32_t bitmap = ~0u;
		bitmap &= ~1; // slice 0 reserved
		bitmap &= ~(((1 << num_slices) - 1) << 1);
		meta->slice_bitmap = cpu_to_le32(bitmap);
		meta->next_partial_block = cpu_to_le32(sbi->s_free_sliced_blocks);
		sbi->s_free_sliced_blocks = block_no;

		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);

		slice_start = 1;
		goto slice_allocated;
	}

slice_allocated:
	// update inode
	ci->index_block = (slice_start << 27) | block_no;
	inode->i_blocks = 1;
	inode->i_size = count;
	mark_inode_dirty(inode);

	// write to slices
	bh = sb_bread(sb, block_no);
	if (!bh) {
		kfree(kbuf);
		return -EIO;
	}

	size_t written = 0;
	for (int s = 0; s < num_slices; s++) {
		size_t to_copy = min_t(size_t, 128, count - written);
		void *dst = bh->b_data + ((slice_start + s) * 128);
		memset(dst, 0, 128);
		memcpy(dst, kbuf + written, to_copy);
		written += to_copy;
	}

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	/* detect new small file and update small_files count */
	if (old_size == 0 && count <= 128) {
		sbi->small_files++;
	}

	/* if the slice is new allocated */
	if (!found) {
		sbi->sliced_blocks++;
		sbi->total_used_size += OUICHEFS_BLOCK_SIZE;
		/* 32 slices for new sliced block. slice 0 kept for meta data */
		sbi->total_free_slices += (31 - num_slices);
	} else {
		/* partial free sliced block used */
		sbi->total_free_slices -= num_slices;
	}

	/* update total data size */
	sbi->total_data_size += (count - old_size);

	/* check if it's small file */
	if (old_size > 0 && old_size <= 128 && count > 128) {
		sbi->small_files--;
	}

	iocb->ki_pos += count;
	inode->i_size = max_t(loff_t, inode->i_size, iocb->ki_pos);
	mark_inode_dirty(inode);

	printk(KERN_INFO "[OuicheFS]Write: block=%u slice=%u (index_block=0x%x)\n", block_no, slice_start, ci->index_block);

	kfree(kbuf);
	return count;
}

//Implementation for task 1.6
#include <linux/uaccess.h>  // for copy_to_user if needed

long ouichefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	uint32_t block_no;
	int i;

	if (cmd != OUICHEFS_IOCTL_DUMP_BLOCK)
		return -ENOTTY;

	// only support slice-based files
	if (ci->index_block == 0)
		return -EINVAL;

	block_no = ci->index_block & ((1 << 27) - 1);
	bh = sb_bread(sb, block_no);
	if (!bh)
		return -EIO;

	printk(KERN_INFO "---- [OuicheFS] Dumping Block %u ----\n", block_no);

	uint32_t slice_start = ci->index_block >> 27;
	uint32_t num_slices = DIV_ROUND_UP(inode->i_size, 128);

	for (i = 0; i < num_slices; i++) {
		char line[129];
		memcpy(line, bh->b_data + (slice_start + i) * 128, 128);
		line[128] = '\0';
		printk(KERN_INFO "Slice %02d: %.128s\n", slice_start + i, line);
	}

	brelse(bh);
	return 0;
}

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.open = ouichefs_open,
	.llseek = generic_file_llseek,
	.read_iter = ouichefs_read,
	.write_iter = ouichefs_write,
	.fsync = generic_file_fsync,
	.unlocked_ioctl = ouichefs_ioctl,
};

uint32_t ouichefs_alloc_block(struct super_block *sb)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	return get_free_block(sbi);
}
