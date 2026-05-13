#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/blk_types.h>
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mpage.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/parser.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "../include/uapi/linux/simplefs_ioctl.h"

#define SIMPLEFS_NAME "simplefs"
#define SIMPLEFS_MAGIC 0x53464C494E55584FULL
#define SIMPLEFS_VERSION 1
#define SIMPLEFS_ROOT_INO 1
#define SIMPLEFS_FIRST_FILE_INO 2
#define SIMPLEFS_SECTOR_SIZE 512
#define SIMPLEFS_NAME_PREFIX "file"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define SIMPLEFS_AOPS_USE_KIOCB 1
#endif

struct simplefs_disk_superblock {
	__le64 magic;
	__le32 version;
	__le32 checksum;
	__le64 total_sectors;
	__le64 primary_sector;
	__le64 backup_sector;
	__le32 max_name_len;
	__le32 file_sectors;
	__le32 file_count;
	__le32 sector_size;
	u8 uuid[16];
};

struct simplefs_info {
	sector_t total_sectors;
	sector_t primary_sector;
	sector_t backup_sector;
	u32 max_name_len;
	u32 file_sectors;
	u32 file_sectors_limit;
	u32 file_count;
	u32 name_width;
};

static char *device_name;
static ulong sb_primary_sector;
static ulong sb_backup_sector = 1;
static uint max_filename_len = 32;
static uint max_file_sectors = 4;

module_param(device_name, charp, 0444);
MODULE_PARM_DESC(device_name, "Expected block device name, for example loop0 or /dev/loop0");
module_param(sb_primary_sector, ulong, 0444);
MODULE_PARM_DESC(sb_primary_sector, "Sector offset of the primary superblock copy");
module_param(sb_backup_sector, ulong, 0444);
MODULE_PARM_DESC(sb_backup_sector, "Sector offset of the backup superblock copy");
module_param(max_filename_len, uint, 0444);
MODULE_PARM_DESC(max_filename_len, "Maximum generated file name length");
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "Maximum file size in sectors");

static inline struct simplefs_info *simplefs_sb_info(const struct super_block *sb)
{
	return sb->s_fs_info;
}

static struct inode *simplefs_iget(struct super_block *sb, unsigned long ino);

static u32 simplefs_digits(u32 value)
{
	u32 digits = 1;

	while (value >= 10) {
		value /= 10;
		digits++;
	}

	return digits;
}

static sector_t simplefs_logical_to_physical(const struct simplefs_info *sbi,
					     sector_t logical_sector)
{
	sector_t first = min(sbi->primary_sector, sbi->backup_sector);
	sector_t second = max(sbi->primary_sector, sbi->backup_sector);
	sector_t physical = logical_sector;

	if (physical >= first)
		physical++;
	if (physical >= second)
		physical++;

	return physical;
}

static sector_t simplefs_file_sector(const struct simplefs_info *sbi, u32 file_index,
				     u32 sector_index)
{
	sector_t logical = (sector_t)file_index * sbi->file_sectors + sector_index;

	return simplefs_logical_to_physical(sbi, logical);
}

static int simplefs_format_name(const struct simplefs_info *sbi, u32 index,
				char *buf, size_t len)
{
	return scnprintf(buf, len, SIMPLEFS_NAME_PREFIX "%0*u",
			 sbi->name_width, index);
}

static int simplefs_lookup_name(const struct simplefs_info *sbi, const char *name, size_t len)
{
	char generated[SIMPLEFS_IOCTL_NAME_MAX];
	u32 i;

	for (i = 0; i < sbi->file_count; i++) {
		int written = simplefs_format_name(sbi, i, generated, sizeof(generated));

		if (written == len && !strncmp(name, generated, len))
			return i;
	}

	return -ENOENT;
}

static u32 simplefs_super_crc(struct simplefs_disk_superblock *ondisk)
{
	__le32 checksum = ondisk->checksum;
	u32 crc;

	ondisk->checksum = 0;
	crc = crc32_le(~0, (const unsigned char *)ondisk, sizeof(*ondisk));
	ondisk->checksum = checksum;

	return crc;
}

static int simplefs_validate_disk_superblock(const struct super_block *sb,
					     const struct simplefs_info *sbi,
					     struct simplefs_disk_superblock *ondisk)
{
	struct simplefs_disk_superblock copy;
	u32 checksum;
	sector_t total_sectors = sb->s_bdev->bd_nr_sectors;
	sector_t primary_sector = le64_to_cpu(ondisk->primary_sector);
	sector_t backup_sector = le64_to_cpu(ondisk->backup_sector);
	u32 file_sectors = le32_to_cpu(ondisk->file_sectors);
	u32 file_count = le32_to_cpu(ondisk->file_count);

	if (total_sectors < 3)
		return -EINVAL;
	if (le64_to_cpu(ondisk->magic) != SIMPLEFS_MAGIC)
		return -EINVAL;
	if (le32_to_cpu(ondisk->version) != SIMPLEFS_VERSION)
		return -EINVAL;
	if (le32_to_cpu(ondisk->sector_size) != SIMPLEFS_SECTOR_SIZE)
		return -EINVAL;
	if (le64_to_cpu(ondisk->total_sectors) != total_sectors)
		return -EINVAL;
	if (primary_sector != sbi->primary_sector || backup_sector != sbi->backup_sector)
		return -EINVAL;
	if (primary_sector == backup_sector ||
	    primary_sector >= total_sectors || backup_sector >= total_sectors)
		return -EINVAL;
	if (!file_sectors || !file_count)
		return -EINVAL;
	if (file_sectors > total_sectors - 2)
		return -EINVAL;
	if ((u64)file_sectors * file_count != total_sectors - 2)
		return -EINVAL;

	copy = *ondisk;
	checksum = simplefs_super_crc(&copy);
	if (checksum != le32_to_cpu(ondisk->checksum))
		return -EUCLEAN;

	return 0;
}

static void simplefs_fill_runtime_info(struct simplefs_info *sbi,
				       const struct simplefs_disk_superblock *ondisk)
{
	sbi->total_sectors = le64_to_cpu(ondisk->total_sectors);
	sbi->primary_sector = le64_to_cpu(ondisk->primary_sector);
	sbi->backup_sector = le64_to_cpu(ondisk->backup_sector);
	sbi->max_name_len = le32_to_cpu(ondisk->max_name_len);
	sbi->file_sectors = le32_to_cpu(ondisk->file_sectors);
	sbi->file_count = le32_to_cpu(ondisk->file_count);
	sbi->name_width = simplefs_digits(max_t(u32, 1, sbi->file_count - 1));
}

static void simplefs_build_disk_superblock(struct simplefs_info *sbi,
					   struct simplefs_disk_superblock *ondisk)
{
	memset(ondisk, 0, sizeof(*ondisk));
	ondisk->magic = cpu_to_le64(SIMPLEFS_MAGIC);
	ondisk->version = cpu_to_le32(SIMPLEFS_VERSION);
	ondisk->total_sectors = cpu_to_le64(sbi->total_sectors);
	ondisk->primary_sector = cpu_to_le64(sbi->primary_sector);
	ondisk->backup_sector = cpu_to_le64(sbi->backup_sector);
	ondisk->max_name_len = cpu_to_le32(sbi->max_name_len);
	ondisk->file_sectors = cpu_to_le32(sbi->file_sectors);
	ondisk->file_count = cpu_to_le32(sbi->file_count);
	ondisk->sector_size = cpu_to_le32(SIMPLEFS_SECTOR_SIZE);
	get_random_bytes(ondisk->uuid, sizeof(ondisk->uuid));
	ondisk->checksum = cpu_to_le32(simplefs_super_crc(ondisk));
}

static int simplefs_write_super_copy(struct super_block *sb, sector_t sector,
				     const struct simplefs_disk_superblock *ondisk)
{
	struct buffer_head *bh;

	bh = sb_getblk(sb, sector);
	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memset(bh->b_data, 0, sb->s_blocksize);
	memcpy(bh->b_data, ondisk, sizeof(*ondisk));
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	if (buffer_write_io_error(bh)) {
		brelse(bh);
		return -EIO;
	}

	brelse(bh);
	return 0;
}

static int simplefs_write_superblocks(struct super_block *sb, struct simplefs_info *sbi)
{
	struct simplefs_disk_superblock ondisk;
	int ret;

	simplefs_build_disk_superblock(sbi, &ondisk);

	ret = simplefs_write_super_copy(sb, sbi->primary_sector, &ondisk);
	if (ret)
		return ret;

	return simplefs_write_super_copy(sb, sbi->backup_sector, &ondisk);
}

static int simplefs_clear_sector(struct super_block *sb, sector_t sector)
{
	struct buffer_head *bh;

	bh = sb_getblk(sb, sector);
	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memset(bh->b_data, 0, sb->s_blocksize);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	if (buffer_write_io_error(bh)) {
		brelse(bh);
		return -EIO;
	}

	brelse(bh);
	return 0;
}

static int simplefs_zero_all_files(struct super_block *sb)
{
	struct simplefs_info *sbi = simplefs_sb_info(sb);
	u32 file_idx;
	u32 sector_idx;
	int ret;

	for (file_idx = 0; file_idx < sbi->file_count; file_idx++) {
		for (sector_idx = 0; sector_idx < sbi->file_sectors; sector_idx++) {
			ret = simplefs_clear_sector(sb, simplefs_file_sector(sbi, file_idx, sector_idx));
			if (ret)
				return ret;
		}
		cond_resched();
	}

	return 0;
}

static int simplefs_zero_data_area(struct super_block *sb)
{
	struct simplefs_info *sbi = simplefs_sb_info(sb);
	u32 file_idx;
	u32 sector_idx;
	int ret;

	for (file_idx = 0; file_idx < sbi->file_count; file_idx++) {
		for (sector_idx = 0; sector_idx < sbi->file_sectors; sector_idx++) {
			ret = simplefs_clear_sector(sb, simplefs_file_sector(sbi, file_idx, sector_idx));
			if (ret)
				return ret;
		}
		cond_resched();
	}

	return 0;
}

static int simplefs_writeback_cached_files(struct super_block *sb)
{
	struct simplefs_info *sbi = simplefs_sb_info(sb);
	u32 file_idx;
	int ret = 0;

	for (file_idx = 0; file_idx < sbi->file_count; file_idx++) {
		struct inode *inode;
		int file_ret;

		inode = ilookup(sb, SIMPLEFS_FIRST_FILE_INO + file_idx);
		if (!inode)
			continue;

		file_ret = filemap_write_and_wait(inode->i_mapping);
		if (file_ret && !ret)
			ret = file_ret;

		iput(inode);
		cond_resched();
	}

	return ret;
}

static void simplefs_invalidate_cached_files(struct super_block *sb)
{
	struct simplefs_info *sbi = simplefs_sb_info(sb);
	u32 file_idx;

	for (file_idx = 0; file_idx < sbi->file_count; file_idx++) {
		struct inode *inode;

		inode = ilookup(sb, SIMPLEFS_FIRST_FILE_INO + file_idx);
		if (!inode)
			continue;

		truncate_inode_pages(inode->i_mapping, 0);
		iput(inode);
		cond_resched();
	}
}

static int simplefs_wipe_fs(struct super_block *sb)
{
	struct simplefs_info *sbi = simplefs_sb_info(sb);
	sector_t sector;
	int ret;

	for (sector = 0; sector < sbi->total_sectors; sector++) {
		ret = simplefs_clear_sector(sb, sector);
		if (ret)
			return ret;
		cond_resched();
	}

	return 0;
}

static int simplefs_compute_file_crc(struct super_block *sb, u32 file_index, u32 *crc_out)
{
	struct inode *inode;
	loff_t remaining;
	u32 crc = ~0U;
	pgoff_t index = 0;
	int ret;

	inode = simplefs_iget(sb, SIMPLEFS_FIRST_FILE_INO + file_index);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ret = filemap_write_and_wait(inode->i_mapping);
	if (ret)
		goto out_iput;

	remaining = inode->i_size;
	while (remaining > 0) {
		struct folio *folio;
		size_t len;
		void *addr;

		folio = read_mapping_folio(inode->i_mapping, index, NULL);
		if (IS_ERR(folio)) {
			ret = PTR_ERR(folio);
			goto out_iput;
		}

		len = min_t(loff_t, remaining, folio_size(folio));
		addr = kmap_local_folio(folio, 0);
		crc = crc32_le(crc, addr, len);
		kunmap_local(addr);
		folio_put(folio);

		remaining -= len;
		index++;
	}

	*crc_out = crc;
	ret = 0;

out_iput:
	iput(inode);
	return ret;
}

static int simplefs_copy_hash_entries(struct super_block *sb, void __user *user_ptr,
				      u32 capacity, u32 *count_out)
{
	struct simplefs_info *sbi = simplefs_sb_info(sb);
	u32 count = sbi->file_count;
	u32 limit = min(capacity, count);
	u32 i;

	if (!user_ptr || !capacity) {
		*count_out = count;
		return 0;
	}

	for (i = 0; i < limit; i++) {
		struct simplefs_hash_entry entry;
		u32 crc;
		int ret;

		memset(&entry, 0, sizeof(entry));
		ret = simplefs_compute_file_crc(sb, i, &crc);
		if (ret)
			return ret;

		entry.inode_no = SIMPLEFS_FIRST_FILE_INO + i;
		entry.crc32 = crc;
		entry.size_bytes = (u64)sbi->file_sectors * sb->s_blocksize;
		simplefs_format_name(sbi, i, entry.name, sizeof(entry.name));

		if (copy_to_user((char __user *)user_ptr + i * sizeof(entry), &entry, sizeof(entry)))
			return -EFAULT;
	}

	*count_out = count;
	return 0;
}

static int simplefs_copy_sector_map(struct super_block *sb, const char *name,
				    void __user *user_ptr, u32 capacity, u32 *count_out)
{
	struct simplefs_info *sbi = simplefs_sb_info(sb);
	int file_idx;
	u32 count = sbi->file_sectors;
	u32 limit = min(capacity, count);
	u64 sector;
	u32 i;

	file_idx = simplefs_lookup_name(sbi, name, strnlen(name, SIMPLEFS_IOCTL_NAME_MAX));
	if (file_idx < 0)
		return file_idx;

	for (i = 0; i < limit; i++) {
		sector = simplefs_file_sector(sbi, file_idx, i);
		if (copy_to_user((u64 __user *)user_ptr + i, &sector, sizeof(sector)))
			return -EFAULT;
	}

	*count_out = count;
	return 0;
}

static int simplefs_device_matches(struct super_block *sb)
{
	const char *actual = sb->s_bdev->bd_disk->disk_name;
	const char *expected = device_name;

	if (!expected || !*expected)
		return -EINVAL;
	if (!strcmp(expected, actual))
		return 0;
	if (!strncmp(expected, "/dev/", 5) && !strcmp(expected + 5, actual))
		return 0;

	return -EINVAL;
}

static int simplefs_init_layout(struct super_block *sb, struct simplefs_info *sbi)
{
	sector_t usable_sectors;
	u32 prefix_len = strlen(SIMPLEFS_NAME_PREFIX);
	u32 file_sectors;

	if (sbi->primary_sector == sbi->backup_sector)
		return -EINVAL;
	if (sbi->primary_sector >= sbi->total_sectors || sbi->backup_sector >= sbi->total_sectors)
		return -EINVAL;
	if (!sbi->file_sectors_limit)
		return -EINVAL;
	if (sbi->total_sectors < 3)
		return -EINVAL;
	if (sbi->total_sectors - 2 > U32_MAX)
		return -EFBIG;

	usable_sectors = sbi->total_sectors - 2;
	file_sectors = sbi->file_sectors;
	if (file_sectors) {
		if (file_sectors > sbi->file_sectors_limit)
			return -EINVAL;
		if (usable_sectors % file_sectors)
			return -EINVAL;
	} else {
		file_sectors = min(sbi->file_sectors_limit, usable_sectors);
		while (file_sectors > 1 && usable_sectors % file_sectors)
			file_sectors--;
	}

	sbi->file_sectors = file_sectors;
	sbi->file_count = usable_sectors / sbi->file_sectors;
	if (!sbi->file_count)
		return -ENOSPC;

	sbi->name_width = simplefs_digits(max_t(u32, 1, sbi->file_count - 1));
	if (prefix_len + sbi->name_width > sbi->max_name_len ||
	    sbi->max_name_len > SIMPLEFS_IOCTL_NAME_MAX - 1)
		return -EINVAL;

	return 0;
}

static int simplefs_load_or_create_super(struct super_block *sb, struct simplefs_info *sbi)
{
	struct buffer_head *primary_bh = NULL;
	struct buffer_head *backup_bh = NULL;
	struct simplefs_disk_superblock *primary = NULL;
	struct simplefs_disk_superblock *backup = NULL;
	int primary_valid = 0;
	int backup_valid = 0;
	int ret;

	primary_bh = sb_bread(sb, sbi->primary_sector);
	backup_bh = sb_bread(sb, sbi->backup_sector);
	if (!primary_bh || !backup_bh) {
		ret = -EIO;
		goto out;
	}

	primary = (struct simplefs_disk_superblock *)primary_bh->b_data;
	backup = (struct simplefs_disk_superblock *)backup_bh->b_data;

	primary_valid = !simplefs_validate_disk_superblock(sb, sbi, primary);
	backup_valid = !simplefs_validate_disk_superblock(sb, sbi, backup);

	if (primary_valid) {
		simplefs_fill_runtime_info(sbi, primary);
		ret = 0;
		goto maybe_repair;
	}

	if (backup_valid) {
		simplefs_fill_runtime_info(sbi, backup);
		ret = 0;
		goto maybe_repair;
	}

	ret = simplefs_init_layout(sb, sbi);
	if (ret)
		goto out;

	ret = simplefs_write_superblocks(sb, sbi);
	if (ret)
		goto out;

	ret = simplefs_zero_data_area(sb);
	goto out;

maybe_repair:
	ret = simplefs_init_layout(sb, sbi);
	if (ret)
		goto out;
	if (!primary_valid || !backup_valid)
		ret = simplefs_write_superblocks(sb, sbi);

out:
	brelse(primary_bh);
	brelse(backup_bh);
	return ret;
}

static int simplefs_get_block(struct inode *inode, sector_t iblock,
			      struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct simplefs_info *sbi = simplefs_sb_info(sb);
	u32 file_index;
	sector_t physical;

	if (inode->i_ino < SIMPLEFS_FIRST_FILE_INO)
		return -EIO;

	if (iblock >= sbi->file_sectors)
		return -EFBIG;

	file_index = inode->i_ino - SIMPLEFS_FIRST_FILE_INO;
	if (file_index >= sbi->file_count)
		return -ENOENT;

	physical = simplefs_file_sector(sbi, file_index, iblock);
	map_bh(bh_result, sb, physical);
	bh_result->b_size = sb->s_blocksize;
	if (create)
		set_buffer_new(bh_result);

	return 0;
}

static int simplefs_read_folio(struct file *file, struct folio *folio)
{
	(void)file;
	return block_read_full_folio(folio, simplefs_get_block);
}

static void simplefs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, simplefs_get_block);
}

static int simplefs_write_begin(
#ifdef SIMPLEFS_AOPS_USE_KIOCB
				const struct kiocb *iocb,
#else
				struct file *file,
#endif
				struct address_space *mapping, loff_t pos,
				unsigned int len, struct folio **foliop,
				void **fsdata)
{
#ifdef SIMPLEFS_AOPS_USE_KIOCB
	(void)iocb;
#else
	(void)file;
#endif
	return block_write_begin(mapping, pos, len, foliop, simplefs_get_block);
}

static int simplefs_write_end(
#ifdef SIMPLEFS_AOPS_USE_KIOCB
			      const struct kiocb *iocb,
#else
			      struct file *file,
#endif
			      struct address_space *mapping, loff_t pos,
			      unsigned int len, unsigned int copied,
			      struct folio *folio, void *fsdata)
{
#ifdef SIMPLEFS_AOPS_USE_KIOCB
	return generic_write_end(iocb, mapping, pos, len, copied, folio, fsdata);
#else
	return generic_write_end(file, mapping, pos, len, copied, folio, fsdata);
#endif
}

static int simplefs_writepages(struct address_space *mapping,
			       struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, simplefs_get_block);
}

static bool simplefs_release_folio(struct folio *folio, gfp_t gfp)
{
	(void)gfp;
	return try_to_free_buffers(folio);
}

static sector_t simplefs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, simplefs_get_block);
}

static const struct address_space_operations simplefs_aops = {
	.dirty_folio = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
	.read_folio = simplefs_read_folio,
	.readahead = simplefs_readahead,
	.release_folio = simplefs_release_folio,
	.write_begin = simplefs_write_begin,
	.write_end = simplefs_write_end,
	.writepages = simplefs_writepages,
	.bmap = simplefs_bmap,
	.migrate_folio = buffer_migrate_folio,
};

static int simplefs_dir_iterate(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct simplefs_info *sbi = simplefs_sb_info(inode->i_sb);
	u32 index;
	char name[SIMPLEFS_IOCTL_NAME_MAX];

	if (!dir_emit_dots(file, ctx))
		return 0;

	if (ctx->pos < 2)
		ctx->pos = 2;

	index = ctx->pos - 2;
	while (index < sbi->file_count) {
		unsigned long ino = SIMPLEFS_FIRST_FILE_INO + index;
		int len = simplefs_format_name(sbi, index, name, sizeof(name));

		if (!dir_emit(ctx, name, len, ino, DT_REG))
			return 0;

		ctx->pos++;
		index++;
	}

	return 0;
}

static long simplefs_dir_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	void __user *argp = (void __user *)arg;
	int ret = -ENOTTY;

	switch (cmd) {
	case SIMPLEFS_IOCTL_ZERO_FILES:
		ret = simplefs_writeback_cached_files(sb);
		if (!ret)
			ret = simplefs_zero_all_files(sb);
		if (!ret)
			simplefs_invalidate_cached_files(sb);
		break;
	case SIMPLEFS_IOCTL_WIPE_FS:
		ret = simplefs_writeback_cached_files(sb);
		if (!ret)
			ret = simplefs_wipe_fs(sb);
		if (!ret)
			simplefs_invalidate_cached_files(sb);
		break;
	case SIMPLEFS_IOCTL_GET_HASHES:
	{
		struct simplefs_hash_query query;

		if (copy_from_user(&query, argp, sizeof(query)))
			return -EFAULT;

		ret = simplefs_writeback_cached_files(sb);
		if (ret)
			return ret;

		ret = simplefs_copy_hash_entries(sb, u64_to_user_ptr(query.entries_ptr),
						 query.capacity, &query.count);
		if (ret)
			return ret;

		if (copy_to_user(argp, &query, sizeof(query)))
			return -EFAULT;
		break;
	}
	case SIMPLEFS_IOCTL_GET_MAP:
	{
		struct simplefs_map_query query;

		if (copy_from_user(&query, argp, sizeof(query)))
			return -EFAULT;

		query.name[SIMPLEFS_IOCTL_NAME_MAX - 1] = '\0';
		ret = simplefs_copy_sector_map(sb, query.name,
					       u64_to_user_ptr(query.sectors_ptr),
					       query.capacity, &query.count);
		if (ret)
			return ret;

		if (copy_to_user(argp, &query, sizeof(query)))
			return -EFAULT;
		break;
	}
	}

	return ret;
}

static const struct file_operations simplefs_dir_operations = {
	.owner = THIS_MODULE,
	.iterate_shared = simplefs_dir_iterate,
	.read = generic_read_dir,
	.llseek = generic_file_llseek,
	.unlocked_ioctl = simplefs_dir_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = simplefs_dir_ioctl,
#endif
};

static const struct file_operations simplefs_file_operations = {
	.owner = THIS_MODULE,
	.llseek = generic_file_llseek,
	.read_iter = generic_file_read_iter,
	.write_iter = generic_file_write_iter,
};

static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct simplefs_info *sbi = simplefs_sb_info(dir->i_sb);
	struct inode *inode = NULL;
	int file_idx;

	if (dentry->d_name.len > sbi->max_name_len)
		return ERR_PTR(-ENAMETOOLONG);

	file_idx = simplefs_lookup_name(sbi, dentry->d_name.name, dentry->d_name.len);
	if (file_idx >= 0)
		inode = simplefs_iget(dir->i_sb, SIMPLEFS_FIRST_FILE_INO + file_idx);

	return d_splice_alias(inode, dentry);
}

static const struct inode_operations simplefs_dir_inode_operations = {
	.lookup = simplefs_lookup,
};

static const struct inode_operations simplefs_file_inode_operations = {};

static struct inode *simplefs_iget(struct super_block *sb, unsigned long ino)
{
	struct simplefs_info *sbi = simplefs_sb_info(sb);
	struct inode *inode;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (!(READ_ONCE(inode->i_state) & I_NEW))
		return inode;

	inode_init_owner(&nop_mnt_idmap, inode, NULL,
			 ino == SIMPLEFS_ROOT_INO ? S_IFDIR | 0555 : S_IFREG | 0666);
	inode_set_atime_to_ts(inode, current_time(inode));
	inode_set_mtime_to_ts(inode, current_time(inode));
	inode_set_ctime_to_ts(inode, current_time(inode));
	inode->i_blocks = ino == SIMPLEFS_ROOT_INO ? 0 : sbi->file_sectors;

	if (ino == SIMPLEFS_ROOT_INO) {
		set_nlink(inode, 2);
		inode->i_size = 0;
		inode->i_op = &simplefs_dir_inode_operations;
		inode->i_fop = &simplefs_dir_operations;
	} else if (ino >= SIMPLEFS_FIRST_FILE_INO &&
		   ino < SIMPLEFS_FIRST_FILE_INO + sbi->file_count) {
		set_nlink(inode, 1);
		inode->i_size = (loff_t)sbi->file_sectors * sb->s_blocksize;
		inode->i_op = &simplefs_file_inode_operations;
		inode->i_fop = &simplefs_file_operations;
		inode->i_mapping->a_ops = &simplefs_aops;
	} else {
		iget_failed(inode);
		return ERR_PTR(-ESTALE);
	}

	unlock_new_inode(inode);
	return inode;
}

static void simplefs_put_super(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

static const struct super_operations simplefs_super_ops = {
	.put_super = simplefs_put_super,
};

static int simplefs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *root;
	struct simplefs_info *sbi;
	int ret;

	(void)fc;

	ret = sb_set_blocksize(sb, SIMPLEFS_SECTOR_SIZE);
	if (!ret)
		return -EINVAL;

	ret = simplefs_device_matches(sb);
	if (ret)
		return ret;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sbi->total_sectors = sb->s_bdev->bd_nr_sectors;
	sbi->primary_sector = sb_primary_sector;
	sbi->backup_sector = sb_backup_sector;
	sbi->max_name_len = max_filename_len;
	sbi->file_sectors_limit = max_file_sectors;

	sb->s_magic = SIMPLEFS_MAGIC;
	sb->s_fs_info = sbi;
	sb->s_op = &simplefs_super_ops;
	sb->s_maxbytes = (loff_t)max_file_sectors * SIMPLEFS_SECTOR_SIZE;
	sb->s_time_gran = 1;

	ret = simplefs_load_or_create_super(sb, sbi);
	if (ret)
		goto err_clear_info;

	sb->s_maxbytes = (loff_t)sbi->file_sectors * SIMPLEFS_SECTOR_SIZE;

	root = simplefs_iget(sb, SIMPLEFS_ROOT_INO);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto err_clear_info;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto err_clear_info;
	}

	return 0;

err_clear_info:
	kfree(sbi);
	sb->s_fs_info = NULL;
	return ret;
}

static int simplefs_get_tree(struct fs_context *fc)
{
	return get_tree_bdev(fc, simplefs_fill_super);
}

static const struct fs_context_operations simplefs_context_ops = {
	.parse_monolithic = generic_parse_monolithic,
	.get_tree = simplefs_get_tree,
};

static int simplefs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &simplefs_context_ops;
	return 0;
}

static void simplefs_kill_sb(struct super_block *sb)
{
	kill_block_super(sb);
}

static struct file_system_type simplefs_fs_type = {
	.name = SIMPLEFS_NAME,
	.fs_flags = FS_REQUIRES_DEV,
	.init_fs_context = simplefs_init_fs_context,
	.kill_sb = simplefs_kill_sb,
	.owner = THIS_MODULE,
};

static int __init simplefs_init(void)
{
	int ret;

	ret = register_filesystem(&simplefs_fs_type);
	if (ret)
		return ret;

	pr_info("simplefs: registered\n");
	return 0;
}

static void __exit simplefs_exit(void)
{
	unregister_filesystem(&simplefs_fs_type);
	pr_info("simplefs: unregistered\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ivanov N.A.");
MODULE_DESCRIPTION("Simple sector-backed educational filesystem");

module_init(simplefs_init);
module_exit(simplefs_exit);
