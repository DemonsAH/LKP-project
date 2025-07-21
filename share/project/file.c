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
    struct file *filp = iocb->ki_filp;                        // 获取文件指针
    struct inode *inode = file_inode(filp);                   // 获取 inode
    struct super_block *sb = inode->i_sb;                     // 获取超级块
    loff_t pos = iocb->ki_pos;                                // 当前文件偏移
    size_t count = iov_iter_count(to);                        // 用户希望读取的字节数
    size_t copied = 0;                                        // 实际拷贝的字节数

    if (pos >= inode->i_size)                                 // 如果请求位置在文件尾后
        return 0;                                             // 表示 EOF，直接返回

    while (count > 0 && pos < inode->i_size) {                // 只要还有数据要读并未到 EOF
        unsigned int block_size = 4096;                       // 假设块大小为 4096 字节
        unsigned int block_index = pos / block_size;         // 当前逻辑块编号
        unsigned int offset = pos % block_size;              // 当前块内的偏移量
        size_t bytes_left_in_block = block_size - offset;    // 当前块中还可读多少字节
        size_t bytes_left_in_file = inode->i_size - pos;     // 文件中还剩多少字节可读
        size_t to_copy = min3(count, bytes_left_in_block, bytes_left_in_file);  // 决定本轮拷贝多少

        sector_t disk_block = inode->i_blocks + block_index; // 简化映射：inode->i_blocks 是起始块
        struct buffer_head *bh = sb_bread(sb, disk_block);   // 从磁盘读取该块
        if (!bh)
            return copied ? copied : -EIO;                   // 如果读失败，返回已读或错误

        if (copy_to_iter(bh->b_data + offset, to_copy, to) != to_copy) {
            brelse(bh);                                      // 用户空间拷贝失败，释放 buffer
            return copied ? copied : -EFAULT;
        }

        brelse(bh);                                          // 成功读完该块，释放 buffer
        pos += to_copy;                                      // 更新文件偏移
        copied += to_copy;                                   // 累加已读取字节
        count -= to_copy;                                    // 剩余请求减去本轮已完成部分
    }

    iocb->ki_pos = pos;                                      // 写回更新后的偏移量
    return copied;                                           // 返回总共读取的字节数
}


ssize_t ouichefs_write(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *filp = iocb->ki_filp;                        // 获取文件指针
    struct inode *inode = file_inode(filp);                   // 获取 inode
    struct super_block *sb = inode->i_sb;                     // 获取超级块
    loff_t pos = iocb->ki_pos;                                // 当前文件偏移
    size_t count = iov_iter_count(from);                      // 用户希望写入的字节数
    size_t copied = 0;                                        // 实际写入的字节数

    while (count > 0) {                                       // 直到写完所有数据
        unsigned int block_size = 4096;                       // 假设块大小为 4096
        unsigned int block_index = pos / block_size;         // 当前逻辑块编号
        unsigned int offset = pos % block_size;              // 当前块内的写偏移
        size_t bytes_left_in_block = block_size - offset;    // 当前块中剩余空间
        size_t to_copy = min(count, bytes_left_in_block);    // 本次最多写入多少字节

        sector_t disk_block = inode->i_blocks + block_index; // 简化：从 i_blocks 起偏移
        struct buffer_head *bh = sb_bread(sb, disk_block);   // 读取磁盘块（为了写入）
        if (!bh)
            return copied ? copied : -EIO;                   // 错误处理

        if (copy_from_iter(bh->b_data + offset, to_copy, from) != to_copy) {
            brelse(bh);                                      // 拷贝失败，释放 buffer
            return copied ? copied : -EFAULT;
        }

        mark_buffer_dirty(bh);                               // 标记为脏页（已修改）
        sync_dirty_buffer(bh);                               // 立即写入磁盘
        brelse(bh);                                          // 释放 buffer

        pos += to_copy;                                      // 更新偏移
        copied += to_copy;                                   // 累加写入字节
        count -= to_copy;                                    // 更新剩余请求
    }

    if (pos > inode->i_size) {                               // 如果写入扩展了文件
        inode->i_size = pos;                                 // 更新文件大小
        mark_inode_dirty(inode);                             // 通知 VFS inode 需要写回
    }

    iocb->ki_pos = pos;                                      // 更新偏移值
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
