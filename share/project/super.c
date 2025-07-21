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
#include <linux/slab.h>
#include <linux/statfs.h>

#include "ouichefs.h"
#include "bitmap.h"

static int ouichefs_sysfs_init(struct super_block *sb);
static void ouichefs_sysfs_cleanup(struct super_block *sb);

static struct kmem_cache *ouichefs_inode_cache;

int ouichefs_init_inode_cache(void)
{
	ouichefs_inode_cache = kmem_cache_create(
		"ouichefs_cache", sizeof(struct ouichefs_inode_info), 0, 0,
		NULL);
	if (!ouichefs_inode_cache)
		return -ENOMEM;
	return 0;
}

void ouichefs_destroy_inode_cache(void)
{
	kmem_cache_destroy(ouichefs_inode_cache);
}

static struct inode *ouichefs_alloc_inode(struct super_block *sb)
{
	struct ouichefs_inode_info *ci;

	/* ci = kzalloc(sizeof(struct ouichefs_inode_info), GFP_KERNEL); */
	ci = kmem_cache_alloc(ouichefs_inode_cache, GFP_KERNEL);
	if (!ci)
		return NULL;
	inode_init_once(&ci->vfs_inode);
	return &ci->vfs_inode;
}

static void ouichefs_destroy_inode(struct inode *inode)
{
	struct ouichefs_inode_info *ci;

	ci = OUICHEFS_INODE(inode);
	kmem_cache_free(ouichefs_inode_cache, ci);
}

static int ouichefs_write_inode(struct inode *inode,
				struct writeback_control *wbc)
{
	struct ouichefs_inode *disk_inode;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	uint32_t ino = inode->i_ino;
	uint32_t inode_block = (ino / OUICHEFS_INODES_PER_BLOCK) + 1;
	uint32_t inode_shift = ino % OUICHEFS_INODES_PER_BLOCK;

	if (ino >= sbi->nr_inodes)
		return 0;

	bh = sb_bread(sb, inode_block);
	if (!bh)
		return -EIO;
	disk_inode = (struct ouichefs_inode *)bh->b_data;
	disk_inode += inode_shift;

	/* update the mode using what the generic inode has */
	disk_inode->i_mode = cpu_to_le32(inode->i_mode);
	disk_inode->i_uid = cpu_to_le32(i_uid_read(inode));
	disk_inode->i_gid = cpu_to_le32(i_gid_read(inode));
	disk_inode->i_size = cpu_to_le32(inode->i_size);
	disk_inode->i_ctime = cpu_to_le32(inode->i_ctime.tv_sec);
	disk_inode->i_nctime = cpu_to_le64(inode->i_ctime.tv_nsec);
	disk_inode->i_atime = cpu_to_le32(inode->i_atime.tv_sec);
	disk_inode->i_natime = cpu_to_le64(inode->i_atime.tv_nsec);
	disk_inode->i_mtime = cpu_to_le32(inode->i_mtime.tv_sec);
	disk_inode->i_nmtime = cpu_to_le64(inode->i_mtime.tv_nsec);
	disk_inode->i_blocks = cpu_to_le32(inode->i_blocks);
	disk_inode->i_nlink = cpu_to_le32(inode->i_nlink);
	disk_inode->index_block = cpu_to_le32(ci->index_block);

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

static int sync_sb_info(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_sb_info *disk_sb;
	struct buffer_head *bh;

	/* Flush superblock */
	bh = sb_bread(sb, 0);
	if (!bh)
		return -EIO;
	disk_sb = (struct ouichefs_sb_info *)bh->b_data;

	disk_sb->nr_blocks = cpu_to_le32(sbi->nr_blocks);
	disk_sb->nr_inodes = cpu_to_le32(sbi->nr_inodes);
	disk_sb->nr_istore_blocks = cpu_to_le32(sbi->nr_istore_blocks);
	disk_sb->nr_ifree_blocks = cpu_to_le32(sbi->nr_ifree_blocks);
	disk_sb->nr_bfree_blocks = cpu_to_le32(sbi->nr_bfree_blocks);
	disk_sb->nr_free_inodes = cpu_to_le32(sbi->nr_free_inodes);
	disk_sb->nr_free_blocks = cpu_to_le32(sbi->nr_free_blocks);

	mark_buffer_dirty(bh);
	if (wait)
		sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

static int sync_ifree(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	int i, idx;

	/* Flush free inodes bitmask */
	for (i = 0; i < sbi->nr_ifree_blocks; i++) {
		idx = sbi->nr_istore_blocks + i + 1;

		bh = sb_bread(sb, idx);
		if (!bh)
			return -EIO;

		copy_bitmap_to_le64((__le64 *)bh->b_data,
			(void *)sbi->ifree_bitmap + i * OUICHEFS_BLOCK_SIZE);

		mark_buffer_dirty(bh);
		if (wait)
			sync_dirty_buffer(bh);
		brelse(bh);
	}

	return 0;
}

static int sync_bfree(struct super_block *sb, int wait)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	int i, idx;

	/* Flush free blocks bitmask */
	for (i = 0; i < sbi->nr_bfree_blocks; i++) {
		idx = sbi->nr_istore_blocks + sbi->nr_ifree_blocks + i + 1;

		bh = sb_bread(sb, idx);
		if (!bh)
			return -EIO;

		copy_bitmap_to_le64((__le64 *)bh->b_data,
			(void *)sbi->bfree_bitmap + i * OUICHEFS_BLOCK_SIZE);

		mark_buffer_dirty(bh);
		if (wait)
			sync_dirty_buffer(bh);
		brelse(bh);
	}

	return 0;
}

static void ouichefs_put_super(struct super_block *sb)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	if (sbi) {
		ouichefs_sysfs_cleanup(sb);
		kfree(sbi->ifree_bitmap);
		kfree(sbi->bfree_bitmap);
		kfree(sbi);
	}
}

static int ouichefs_sync_fs(struct super_block *sb, int wait)
{
	int ret = 0;

	ret = sync_sb_info(sb, wait);
	if (ret)
		return ret;
	ret = sync_ifree(sb, wait);
	if (ret)
		return ret;
	ret = sync_bfree(sb, wait);
	if (ret)
		return ret;

	return 0;
}

static int ouichefs_statfs(struct dentry *dentry, struct kstatfs *stat)
{
	struct super_block *sb = dentry->d_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	stat->f_type = OUICHEFS_MAGIC;
	stat->f_bsize = OUICHEFS_BLOCK_SIZE;
	stat->f_blocks = sbi->nr_blocks;
	stat->f_bfree = sbi->nr_free_blocks;
	stat->f_bavail = sbi->nr_free_blocks;
	stat->f_files = sbi->nr_inodes;
	stat->f_ffree = sbi->nr_free_inodes;
	stat->f_namelen = OUICHEFS_FILENAME_LEN;

	return 0;
}

static struct super_operations ouichefs_super_ops = {
	.put_super = ouichefs_put_super,
	.alloc_inode = ouichefs_alloc_inode,
	.destroy_inode = ouichefs_destroy_inode,
	.write_inode = ouichefs_write_inode,
	.sync_fs = ouichefs_sync_fs,
	.statfs = ouichefs_statfs,
};

/* Fill the struct superblock from partition superblock */
int ouichefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct buffer_head *bh = NULL;
	struct ouichefs_sb_info *csb = NULL;
	struct ouichefs_sb_info *sbi = NULL;
	struct inode *root_inode = NULL;
	int ret = 0, i;

	/* Init sb */
	sb->s_magic = OUICHEFS_MAGIC;
	sb_set_blocksize(sb, OUICHEFS_BLOCK_SIZE);
	sb->s_maxbytes = OUICHEFS_MAX_FILESIZE;
	sb->s_op = &ouichefs_super_ops;
	sb->s_time_gran = 1;

	/* Read sb from disk */
	bh = sb_bread(sb, OUICHEFS_SB_BLOCK_NR);
	if (!bh)
		return -EIO;
	csb = (struct ouichefs_sb_info *)bh->b_data;

	/* Check magic number */
	if (le32_to_cpu(csb->magic) != sb->s_magic) {
		pr_err("Wrong magic number\n");
		brelse(bh);
		return -EPERM;
	}

	/* Alloc sb_info */
	sbi = kzalloc(sizeof(struct ouichefs_sb_info), GFP_KERNEL);
	if (!sbi) {
		brelse(bh);
		return -ENOMEM;
	}
	sbi->nr_blocks = le32_to_cpu(csb->nr_blocks);
	sbi->nr_inodes = le32_to_cpu(csb->nr_inodes);
	sbi->nr_istore_blocks = le32_to_cpu(csb->nr_istore_blocks);
	sbi->nr_ifree_blocks = le32_to_cpu(csb->nr_ifree_blocks);
	sbi->nr_bfree_blocks = le32_to_cpu(csb->nr_bfree_blocks);
	sbi->nr_free_inodes = le32_to_cpu(csb->nr_free_inodes);
	sbi->nr_free_blocks = le32_to_cpu(csb->nr_free_blocks);
	sb->s_fs_info = sbi;

	brelse(bh);

	/* Alloc and copy ifree_bitmap */
	sbi->ifree_bitmap =
		kzalloc(sbi->nr_ifree_blocks * OUICHEFS_BLOCK_SIZE, GFP_KERNEL);
	if (!sbi->ifree_bitmap) {
		ret = -ENOMEM;
		goto free_sbi;
	}
	for (i = 0; i < sbi->nr_ifree_blocks; i++) {
		int idx = sbi->nr_istore_blocks + i + 1;

		bh = sb_bread(sb, idx);
		if (!bh) {
			ret = -EIO;
			goto free_ifree;
		}

		copy_bitmap_from_le64((void *)sbi->ifree_bitmap + i * OUICHEFS_BLOCK_SIZE,
			(__le64 *)bh->b_data);

		brelse(bh);
	}

	/* Alloc and copy bfree_bitmap */
	sbi->bfree_bitmap =
		kzalloc(sbi->nr_bfree_blocks * OUICHEFS_BLOCK_SIZE, GFP_KERNEL);
	if (!sbi->bfree_bitmap) {
		ret = -ENOMEM;
		goto free_ifree;
	}
	for (i = 0; i < sbi->nr_bfree_blocks; i++) {
		int idx = sbi->nr_istore_blocks + sbi->nr_ifree_blocks + i + 1;

		bh = sb_bread(sb, idx);
		if (!bh) {
			ret = -EIO;
			goto free_bfree;
		}

		copy_bitmap_from_le64((void *)sbi->bfree_bitmap + i * OUICHEFS_BLOCK_SIZE,
			(__le64 *)bh->b_data);

		brelse(bh);
	}

	/* 
	 * Create root inode.
	 *
	 * 1 is used instead of 0 to stay compatible with userspace applications,
	 * as this is the "de facto standard".
	 *
	 * See:
	 * - https://github.com/rgouicem/ouichefs/commit/296e162
	 * - https://github.com/rgouicem/ouichefs/pull/23
	 */
	root_inode = ouichefs_iget(sb, 1);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		goto free_bfree;
	}
	inode_init_owner(&nop_mnt_idmap, root_inode, NULL, root_inode->i_mode);
	/* d_make_root should only be run once */
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto free_bfree;
	}

	ouichefs_sysfs_init(sb);
	return 0;

free_bfree:
	kfree(sbi->bfree_bitmap);
free_ifree:
	kfree(sbi->ifree_bitmap);
free_sbi:
	kfree(sbi);

	return ret;
}

// task1.4

#define DEFINE_OUICHEFS_ATTR_U32(name, field)                                   \
static ssize_t name##_show(struct kobject *kobj,                                \
                           struct kobj_attribute *attr, char *buf)              \
{                                                                               \
    struct ouichefs_sb_info *sbi = container_of(kobj, struct ouichefs_sb_info, sysfs_kobj); \
    return sprintf(buf, "%u\n", sbi->field);                                    \
}                                                                               \
static struct kobj_attribute name##_attr = __ATTR_RO(name);

#define DEFINE_OUICHEFS_ATTR_U64(name, field)                                   \
static ssize_t name##_show(struct kobject *kobj,                                \
                           struct kobj_attribute *attr, char *buf)              \
{                                                                               \
    struct ouichefs_sb_info *sbi = container_of(kobj, struct ouichefs_sb_info, sysfs_kobj); \
    return sprintf(buf, "%llu\n", (unsigned long long)sbi->field);             \
}                                                                               \
static struct kobj_attribute name##_attr = __ATTR_RO(name);

static void ouichefs_release_kobj(struct kobject *kobj)
{
    // empty, no special resource to free
}

static struct kobj_type ouichefs_kobj_type = {
    .release = ouichefs_release_kobj,
    .sysfs_ops = &kobj_sysfs_ops,
};

// 1. 定义所有属性
DEFINE_OUICHEFS_ATTR_U32(free_blocks, nr_free_blocks);
DEFINE_OUICHEFS_ATTR_U32(sliced_blocks, sliced_blocks);
DEFINE_OUICHEFS_ATTR_U32(total_free_slices, total_free_slices);
DEFINE_OUICHEFS_ATTR_U32(files, files);
DEFINE_OUICHEFS_ATTR_U32(small_files, small_files);
DEFINE_OUICHEFS_ATTR_U64(total_data_size, total_data_size);
DEFINE_OUICHEFS_ATTR_U64(total_used_size, total_used_size);

static ssize_t efficiency_show(struct kobject *kobj,
                               struct kobj_attribute *attr, char *buf)
{
    struct ouichefs_sb_info *sbi = container_of(kobj, struct ouichefs_sb_info, sysfs_kobj);
    if (sbi->total_used_size == 0)
        return sprintf(buf, "0\n");
    return sprintf(buf, "%llu\n", (unsigned long long)
        (sbi->total_data_size * 100 / sbi->total_used_size));
}
static struct kobj_attribute efficiency_attr = __ATTR_RO(efficiency);

// used_blocks 由 nr_blocks - nr_free_blocks 得到
static ssize_t used_blocks_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
    struct ouichefs_sb_info *sbi = container_of(kobj, struct ouichefs_sb_info, sysfs_kobj);
    return sprintf(buf, "%u\n", sbi->nr_blocks - sbi->nr_free_blocks);
}
static struct kobj_attribute used_blocks_attr = __ATTR_RO(used_blocks);

// 2. 属性列表
static struct attribute *ouichefs_attrs[] = {
    &free_blocks_attr.attr,
    &used_blocks_attr.attr,
    &sliced_blocks_attr.attr,
    &total_free_slices_attr.attr,
    &files_attr.attr,
    &small_files_attr.attr,
    &total_data_size_attr.attr,
    &total_used_size_attr.attr,
    &efficiency_attr.attr,
    NULL,
};
static struct attribute_group ouichefs_attr_group = {
    .attrs = ouichefs_attrs,
};

// 3. 初始化函数（在 fill_super 中调用）
static int ouichefs_sysfs_init(struct super_block *sb)
{
    struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
    const char *devname = sb->s_id;
    struct kobject *ouichefs_root_kobj;

    // 创建 /sys/fs/ouichefs 目录
    ouichefs_root_kobj = kobject_create_and_add("ouichefs", fs_kobj);
    if (!ouichefs_root_kobj)
        return -ENOMEM;

    // 初始化当前 partition 的 kobject（例如 loop0）
    kobject_init(&sbi->sysfs_kobj, &ouichefs_kobj_type);
    if (kobject_add(&sbi->sysfs_kobj, ouichefs_root_kobj, "%s", devname))
        return -ENOMEM;

    return sysfs_create_group(&sbi->sysfs_kobj, &ouichefs_attr_group);
}

// 4. 清理函数（在 put_super 中调用）
static void ouichefs_sysfs_cleanup(struct super_block *sb)
{
    struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
    sysfs_remove_group(&sbi->sysfs_kobj, &ouichefs_attr_group);
    kobject_put(&sbi->sysfs_kobj);
}

// === SYSFS 导出逻辑结束 ===
