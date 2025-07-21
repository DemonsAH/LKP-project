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
    struct file *filp = iocb->ki_filp;                        // Retrieve the file pointer
    struct inode *inode = file_inode(filp);                   // Get the inode
    struct super_block *sb = inode->i_sb;                     // Get the superblock
    loff_t pos = iocb->ki_pos;                                // Current file offset
    size_t count = iov_iter_count(to);                        // Number of bytes requested by userspace
    size_t copied = 0;                                        // Total number of bytes copied so far

    if (pos >= inode->i_size)                                 // If requested offset is beyond EOF
        return 0;                                             // Signal EOF to the application

    while (count > 0 && pos < inode->i_size) {                // Continue while data remains and not at EOF
        unsigned int block_size = 4096;                       // Assume block size is 4096 bytes
        unsigned int block_index = pos / block_size;         // Compute logical block index
        unsigned int offset = pos % block_size;              // Offset within the current block
        size_t bytes_left_in_block = block_size - offset;    // Bytes remaining in the current block
        size_t bytes_left_in_file = inode->i_size - pos;     // Bytes left until EOF
        size_t to_copy = min3(count, bytes_left_in_block, bytes_left_in_file);  // How much to copy this iteration

        sector_t disk_block = inode->i_blocks + block_index; // Simplified: start from inode->i_blocks
        struct buffer_head *bh = sb_bread(sb, disk_block);   // Read the block from disk
        if (!bh)
            return copied ? copied : -EIO;                   // On failure, return partial result or error

        if (copy_to_iter(bh->b_data + offset, to_copy, to) != to_copy) {
            brelse(bh);                                      // If copying to userspace fails, release buffer
            return copied ? copied : -EFAULT;
        }

        brelse(bh);                                          // Done with buffer, release it
        pos += to_copy;                                      // Advance file position
        copied += to_copy;                                   // Track bytes read
        count -= to_copy;                                    // Reduce remaining bytes to read
    }

    iocb->ki_pos = pos;                                      // Update file offset for caller
    return copied;                                           // Return total bytes copied
}

ssize_t ouichefs_write(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *filp = iocb->ki_filp;                        // Retrieve the file pointer
    struct inode *inode = file_inode(filp);                   // Get the inode
    struct super_block *sb = inode->i_sb;                     // Get the superblock
    loff_t pos = iocb->ki_pos;                                // Current file offset
    size_t count = iov_iter_count(from);                      // Number of bytes to write from userspace
    size_t copied = 0;                                        // Total bytes written so far

    while (count > 0) {                                       // Continue until all bytes are written
        unsigned int block_size = 4096;                       // Assume block size is 4096 bytes
        unsigned int block_index = pos / block_size;         // Compute logical block index
        unsigned int offset = pos % block_size;              // Offset within the current block
        size_t bytes_left_in_block = block_size - offset;    // Remaining space in current block
        size_t to_copy = min(count, bytes_left_in_block);    // How much to write in this iteration

        sector_t disk_block = inode->i_blocks + block_index; // Simplified: block number from i_blocks + index
        struct buffer_head *bh = sb_bread(sb, disk_block);   // Read block from disk (for writing)
        if (!bh)
            return copied ? copied : -EIO;                   // On failure, return partial or error

        if (copy_from_iter(bh->b_data + offset, to_copy, from) != to_copy) {
            brelse(bh);                                      // Release buffer on failure
            return copied ? copied : -EFAULT;
        }

        mark_buffer_dirty(bh);                               // Mark buffer as dirty (modified)
        sync_dirty_buffer(bh);                               // Immediately flush buffer to disk
        brelse(bh);                                          // Release the buffer

        pos += to_copy;                                      // Advance file offset
        copied += to_copy;                                   // Track total written bytes
        count -= to_copy;                                    // Decrease remaining bytes to write
    }

    if (pos > inode->i_size) {                               // If the file size increased
        inode->i_size = pos;                                 // Update inode size
        mark_inode_dirty(inode);                             // Notify VFS that inode needs to be flushed
    }

    iocb->ki_pos = pos;                                      // Update offset for caller
    return copied;
}

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.open = ouichefs_open,
	.llseek = generic_file_llseek,
	.read_iter = ouichefs_read,
	.write_iter = ouichefs_write,
	.fsync = generic_file_fsync,
};
