#ifndef _UAPI_LINUX_SIMPLEFS_IOCTL_H
#define _UAPI_LINUX_SIMPLEFS_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SIMPLEFS_IOCTL_MAGIC 0xBC
#define SIMPLEFS_IOCTL_NAME_MAX 256

struct simplefs_hash_entry {
	__u32 inode_no;
	__u32 crc32;
	__u64 size_bytes;
	char name[SIMPLEFS_IOCTL_NAME_MAX];
};

struct simplefs_hash_query {
	__u64 entries_ptr;
	__u32 capacity;
	__u32 count;
};

struct simplefs_map_query {
	char name[SIMPLEFS_IOCTL_NAME_MAX];
	__u64 sectors_ptr;
	__u32 capacity;
	__u32 count;
};

#define SIMPLEFS_IOCTL_ZERO_FILES _IO(SIMPLEFS_IOCTL_MAGIC, 0x01)
#define SIMPLEFS_IOCTL_WIPE_FS _IO(SIMPLEFS_IOCTL_MAGIC, 0x02)
#define SIMPLEFS_IOCTL_GET_HASHES _IOWR(SIMPLEFS_IOCTL_MAGIC, 0x03, struct simplefs_hash_query)
#define SIMPLEFS_IOCTL_GET_MAP _IOWR(SIMPLEFS_IOCTL_MAGIC, 0x04, struct simplefs_map_query)

#endif
