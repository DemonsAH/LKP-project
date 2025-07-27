/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */
#ifndef _OUICHEFS_H
#define _OUICHEFS_H

#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/ioctl.h>

#define OUICHEFS_MAGIC 0x48434957

//Implementation for task 1.6
#define OUICHEFS_IOCTL_MAGIC 'O'
#define OUICHEFS_IOCTL_DUMP_BLOCK _IO(OUICHEFS_IOCTL_MAGIC, 0x01)

#define OUICHEFS_SB_BLOCK_NR 0

#define OUICHEFS_BLOCK_SIZE (1 << 12) /* 4 KiB */
#define OUICHEFS_MAX_FILESIZE (1 << 22) /* 4 MiB */
#define OUICHEFS_FILENAME_LEN 28
#define OUICHEFS_MAX_SUBFILES 128

/*
 * ouiche_fs partition layout
 *
 * +---------------+
 * |  superblock   |  1 block
 * +---------------+
 * |  inode store  |  sb->nr_istore_blocks blocks
 * +---------------+
 * | ifree bitmap  |  sb->nr_ifree_blocks blocks
 * +---------------+
 * | bfree bitmap  |  sb->nr_bfree_blocks blocks
 * +---------------+
 * |    data       |
 * |      blocks   |  rest of the blocks
 * +---------------+
 *
 */

// Number of bits used to store the slice number (we have at most 32 slices)
#define SLICE_BITS      5

// Mask to isolate the 5 bits used for the slice number (0b11111 = 0x1F)
#define SLICE_MASK      0x1F

// Mask to isolate the lower 27 bits for the block number (0x07FFFFFF)
#define BLOCK_MASK      0x07FFFFFF

// Pack a block number (lower 27 bits) and slice number (upper 5 bits) into a 32-bit value
static inline uint32_t pack_slice_ptr(uint32_t block_num, uint8_t slice_num)
{
    // Mask the slice number to 5 bits, shift it to bits 31–27, then OR with masked block number
    return ((slice_num & SLICE_MASK) << 27) | (block_num & BLOCK_MASK);
}

// Extract the block number (lower 27 bits) from a packed slice_ptr
static inline uint32_t extract_block_num(uint32_t packed_val)
{
    return packed_val & BLOCK_MASK;
}

// Extract the slice number (upper 5 bits) from a packed slice_ptr
static inline uint8_t extract_slice_num(uint32_t packed_val)
{
    return (packed_val >> 27) & SLICE_MASK;
}

struct ouichefs_inode {
	__le32 i_mode; /* File mode */
	__le32 i_uid; /* Owner id */
	__le32 i_gid; /* Group id */
	__le32 i_size; /* Size in bytes */
	__le32 i_ctime; /* Inode change time (sec)*/
	__le64 i_nctime; /* Inode change time (nsec) */
	__le32 i_atime; /* Access time (sec) */
	__le64 i_natime; /* Access time (nsec) */
	__le32 i_mtime; /* Modification time (sec) */
	__le64 i_nmtime; /* Modification time (nsec) */
	__le32 i_blocks; /* Block count */
	__le32 i_nlink; /* Hard links count */
	__le32 index_block; /* Block with list of blocks for this file */
};

// LKP impl. struct to help describing a silced block
struct ouichefs_sliced_block_meta {
    __le32 slice_bitmap;          // show if corresponding sliced block is free（1 = free, 0 = used）
    __le32 next_partial_block;    // point to next partial block index. 0 if there isn't any
    // following 31 sliced blocks are for intent
};


struct ouichefs_inode_info {
	uint32_t index_block; /* LKP impl: now for packed slice */
	struct inode vfs_inode;
};

#define OUICHEFS_INODES_PER_BLOCK \
	(OUICHEFS_BLOCK_SIZE / sizeof(struct ouichefs_inode))

struct ouichefs_sb_info {
	uint32_t magic; /* Magic number */

	uint32_t nr_blocks; /* Total number of blocks (incl sb & inodes) */
	uint32_t nr_inodes; /* Total number of inodes */

	uint32_t nr_istore_blocks; /* Number of inode store blocks */
	uint32_t nr_ifree_blocks; /* Number of inode free bitmap blocks */
	uint32_t nr_bfree_blocks; /* Number of block free bitmap blocks */

	uint32_t nr_free_inodes; /* Number of free inodes */
	uint32_t nr_free_blocks; /* Number of free blocks */

	unsigned long *ifree_bitmap; /* In-memory free inodes bitmap */
	unsigned long *bfree_bitmap; /* In-memory free blocks bitmap */
	uint32_t s_free_sliced_blocks; /* LKP impl */

	//add new variables for task 1.4
	uint32_t sliced_blocks;
	uint32_t total_free_slices;
	uint32_t files;
	uint32_t small_files;
	uint64_t total_data_size;
	uint64_t total_used_size;

	//add kobject for using sysfs
	struct kobject sysfs_kobj;
};

struct ouichefs_file_index_block {
	__le32 blocks[OUICHEFS_BLOCK_SIZE >> 2];
};

struct ouichefs_dir_block {
	struct ouichefs_file {
		__le32 inode;
		char filename[OUICHEFS_FILENAME_LEN];
	} files[OUICHEFS_MAX_SUBFILES];
};

/* superblock functions */
int ouichefs_fill_super(struct super_block *sb, void *data, int silent);

/* inode functions */
int ouichefs_init_inode_cache(void);
void ouichefs_destroy_inode_cache(void);
struct inode *ouichefs_iget(struct super_block *sb, unsigned long ino);

/* file functions */
extern const struct file_operations ouichefs_file_ops;
extern const struct file_operations ouichefs_dir_ops;
extern const struct address_space_operations ouichefs_aops;

uint32_t ouichefs_alloc_block(struct super_block *sb); //new function added for task1.5

/* Getters for superbock and inode */
#define OUICHEFS_SB(sb) (sb->s_fs_info)
#define OUICHEFS_INODE(inode) \
	(container_of(inode, struct ouichefs_inode_info, vfs_inode))

#endif /* _OUICHEFS_H */
