#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/uapi/linux/simplefs_ioctl.h"

static void die_perror(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

static void die_msg(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(EXIT_FAILURE);
}

static int open_mountpoint(const char *mountpoint)
{
	int fd = open(mountpoint, O_RDONLY | O_DIRECTORY);

	if (fd < 0)
		die_perror("open mountpoint");

	return fd;
}

static void xgetrandom(void *buf, size_t len)
{
	char *pos = buf;

	while (len) {
		ssize_t ret = getrandom(pos, len, 0);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			die_perror("getrandom");
		}
		if (!ret)
			die_msg("getrandom returned EOF");

		pos += ret;
		len -= ret;
	}
}

static void xpwrite_full(int fd, const void *buf, size_t len, off_t offset)
{
	const char *pos = buf;

	while (len) {
		ssize_t ret = pwrite(fd, pos, len, offset);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			die_perror("pwrite");
		}
		if (!ret)
			die_msg("pwrite wrote zero bytes");

		pos += ret;
		offset += ret;
		len -= ret;
	}
}

static void xpread_full(int fd, void *buf, size_t len, off_t offset)
{
	char *pos = buf;

	while (len) {
		ssize_t ret = pread(fd, pos, len, offset);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			die_perror("pread");
		}
		if (!ret)
			die_msg("pread returned EOF");

		pos += ret;
		offset += ret;
		len -= ret;
	}
}

static void command_zero(const char *mountpoint)
{
	int fd = open_mountpoint(mountpoint);

	if (ioctl(fd, SIMPLEFS_IOCTL_ZERO_FILES) < 0)
		die_perror("ioctl ZERO_FILES");

	close(fd);
}

static void command_wipe(const char *mountpoint)
{
	int fd = open_mountpoint(mountpoint);

	if (ioctl(fd, SIMPLEFS_IOCTL_WIPE_FS) < 0)
		die_perror("ioctl WIPE_FS");

	close(fd);
}

static void command_hashes(const char *mountpoint)
{
	int fd = open_mountpoint(mountpoint);
	struct simplefs_hash_query query = {0};
	struct simplefs_hash_entry *entries;
	uint32_t i;

	if (ioctl(fd, SIMPLEFS_IOCTL_GET_HASHES, &query) < 0)
		die_perror("ioctl GET_HASHES(count)");

	if (!query.count) {
		close(fd);
		return;
	}

	entries = calloc(query.count, sizeof(*entries));
	if (!entries)
		die_perror("calloc hashes");

	query.entries_ptr = (uintptr_t)entries;
	query.capacity = query.count;
	if (ioctl(fd, SIMPLEFS_IOCTL_GET_HASHES, &query) < 0)
		die_perror("ioctl GET_HASHES(data)");

	for (i = 0; i < query.count; i++) {
		printf("%u %s size=%llu crc32=0x%08x\n",
		       entries[i].inode_no, entries[i].name,
		       (unsigned long long)entries[i].size_bytes,
		       entries[i].crc32);
	}

	free(entries);
	close(fd);
}

static void command_map(const char *mountpoint, const char *filename)
{
	int fd = open_mountpoint(mountpoint);
	struct simplefs_map_query query = {0};
	uint64_t *sectors;
	uint32_t i;

	snprintf(query.name, sizeof(query.name), "%s", filename);
	if (ioctl(fd, SIMPLEFS_IOCTL_GET_MAP, &query) < 0)
		die_perror("ioctl GET_MAP(count)");

	sectors = calloc(query.count ? query.count : 1, sizeof(*sectors));
	if (!sectors)
		die_perror("calloc map");

	query.sectors_ptr = (uintptr_t)sectors;
	query.capacity = query.count;
	if (ioctl(fd, SIMPLEFS_IOCTL_GET_MAP, &query) < 0)
		die_perror("ioctl GET_MAP(data)");

	printf("%s:", filename);
	for (i = 0; i < query.count; i++)
		printf(" %llu", (unsigned long long)sectors[i]);
	printf("\n");

	free(sectors);
	close(fd);
}

static void fill_one_file(const char *dirpath, const char *name)
{
	char *path = NULL;
	uint64_t written;
	uint64_t read_back = 0;
	int fd;

	if (asprintf(&path, "%s/%s", dirpath, name) < 0)
		die_msg("asprintf failed");

	fd = open(path, O_RDWR);
	if (fd < 0)
		die_perror(path);

	xgetrandom(&written, sizeof(written));
	xpwrite_full(fd, &written, sizeof(written), 0);
	xpread_full(fd, &read_back, sizeof(read_back), 0);

	if (read_back != written) {
		fprintf(stderr, "verification failed for %s\n", path);
		close(fd);
		free(path);
		exit(EXIT_FAILURE);
	}

	printf("%s ok value=%llu\n", name, (unsigned long long)written);
	close(fd);
	free(path);
}

static void command_fill(const char *mountpoint)
{
	DIR *dir;
	struct dirent *entry;

	dir = opendir(mountpoint);
	if (!dir)
		die_perror("opendir");

	while ((entry = readdir(dir)) != NULL) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN)
			continue;

		fill_one_file(mountpoint, entry->d_name);
	}

	closedir(dir);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s fill <mountpoint>\n"
		"  %s zero <mountpoint>\n"
		"  %s wipe <mountpoint>\n"
		"  %s hashes <mountpoint>\n"
		"  %s map <mountpoint> <filename>\n",
		prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (!strcmp(argv[1], "fill")) {
		command_fill(argv[2]);
		return EXIT_SUCCESS;
	}
	if (!strcmp(argv[1], "zero")) {
		command_zero(argv[2]);
		return EXIT_SUCCESS;
	}
	if (!strcmp(argv[1], "wipe")) {
		command_wipe(argv[2]);
		return EXIT_SUCCESS;
	}
	if (!strcmp(argv[1], "hashes")) {
		command_hashes(argv[2]);
		return EXIT_SUCCESS;
	}
	if (!strcmp(argv[1], "map")) {
		if (argc != 4) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		command_map(argv[2], argv[3]);
		return EXIT_SUCCESS;
	}

	usage(argv[0]);
	return EXIT_FAILURE;
}
