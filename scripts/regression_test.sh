#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${1:-/tmp/simplefs-reg.img}"
MOUNTPOINT="${2:-/tmp/simplefs-reg-mnt}"
IMAGE_SIZE_MB="${IMAGE_SIZE_MB:-2}"
LOOP_DEV=""
FIRST_FILE=""
LOOP_NAME=""

if [[ "${EUID}" -ne 0 ]]; then
	echo "Run as root: $0 [image] [mountpoint]" >&2
	exit 1
fi

cleanup() {
	set +e
	if mountpoint -q "${MOUNTPOINT}"; then
		umount "${MOUNTPOINT}"
	fi
	if lsmod | awk '{print $1}' | grep -qx simplefs; then
		rmmod simplefs
	fi
	if [[ -n "${LOOP_DEV}" ]]; then
		losetup -d "${LOOP_DEV}" 2>/dev/null
	fi
	rm -f "${IMAGE}"
	rm -f /tmp/simplefs-reg-write.err /tmp/simplefs-reg-path.err
	rm -f /tmp/simplefs-reg-hashes.* /tmp/simplefs-reg-map.* /tmp/simplefs-reg-mount.err
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

cd "${ROOT_DIR}"
make >/tmp/simplefs-reg-make.log

mkdir -p "${MOUNTPOINT}"
truncate -s "${IMAGE_SIZE_MB}M" "${IMAGE}"
LOOP_DEV="$(losetup --find --show "${IMAGE}")"
LOOP_NAME="$(basename "${LOOP_DEV}")"

insmod "${ROOT_DIR}/simplefs.ko" \
	device_name="${LOOP_NAME}" \
	sb_primary_sector=0 \
	sb_backup_sector=8 \
	max_filename_len=8 \
	max_file_sectors=4

mount -t simplefs "${LOOP_DEV}" "${MOUNTPOINT}"
FIRST_FILE="$(ls "${MOUNTPOINT}" | head -n 1)"
printf x >"${MOUNTPOINT}/${FIRST_FILE}"
exec 3<>"${MOUNTPOINT}/${FIRST_FILE}"

"${ROOT_DIR}/tools/simplefsctl" fill "${MOUNTPOINT}" >/dev/null
"${ROOT_DIR}/tools/simplefsctl" hashes "${MOUNTPOINT}" >/tmp/simplefs-reg-hashes.1
"${ROOT_DIR}/tools/simplefsctl" map "${MOUNTPOINT}" "${FIRST_FILE}" >/tmp/simplefs-reg-map.1

for _ in $(seq 1 16); do
	"${ROOT_DIR}/tools/simplefsctl" hashes "${MOUNTPOINT}" >/tmp/simplefs-reg-hashes."${_}" &
	"${ROOT_DIR}/tools/simplefsctl" map "${MOUNTPOINT}" "${FIRST_FILE}" >/tmp/simplefs-reg-map."${_}" &
done
wait

"${ROOT_DIR}/tools/simplefsctl" wipe "${MOUNTPOINT}"

if [[ -n "$(ls -A "${MOUNTPOINT}")" ]]; then
	fail "files remain visible after wipe"
fi

if printf y >&3 2>/tmp/simplefs-reg-write.err; then
	fail "write through open fd succeeded after wipe"
fi

if printf z >"${MOUNTPOINT}/${FIRST_FILE}" 2>/tmp/simplefs-reg-path.err; then
	fail "write by path succeeded after wipe"
fi

umount "${MOUNTPOINT}"
rmmod simplefs

printf '\1' | dd of="${LOOP_DEV}" bs=1 seek=$((0 * 512 + 12)) conv=notrunc status=none
insmod "${ROOT_DIR}/simplefs.ko" \
	device_name="${LOOP_NAME}" \
	sb_primary_sector=0 \
	sb_backup_sector=8 \
	max_filename_len=8 \
	max_file_sectors=4
if mount -t simplefs "${LOOP_DEV}" "${MOUNTPOINT}" 2>/tmp/simplefs-reg-mount.err; then
	fail "mount succeeded with corrupted primary superblock"
fi
rmmod simplefs

dd if=/dev/zero of="${IMAGE}" bs=1M count="${IMAGE_SIZE_MB}" conv=notrunc status=none
insmod "${ROOT_DIR}/simplefs.ko" \
	device_name="${LOOP_NAME}" \
	sb_primary_sector=0 \
	sb_backup_sector=8 \
	max_filename_len=8 \
	max_file_sectors=4
mount -t simplefs "${LOOP_DEV}" "${MOUNTPOINT}"
umount "${MOUNTPOINT}"
rmmod simplefs

printf '\1' | dd of="${LOOP_DEV}" bs=1 seek=$((0 * 512 + 12)) conv=notrunc status=none
printf '\1' | dd of="${LOOP_DEV}" bs=1 seek=$((8 * 512 + 12)) conv=notrunc status=none
insmod "${ROOT_DIR}/simplefs.ko" \
	device_name="${LOOP_NAME}" \
	sb_primary_sector=0 \
	sb_backup_sector=8 \
	max_filename_len=8 \
	max_file_sectors=4
if mount -t simplefs "${LOOP_DEV}" "${MOUNTPOINT}" 2>/tmp/simplefs-reg-mount.err; then
	fail "mount succeeded with both corrupted superblocks"
fi

echo "simplefs regression passed"
