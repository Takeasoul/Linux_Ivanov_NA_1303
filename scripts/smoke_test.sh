#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${1:-/tmp/simplefs-smoke.img}"
MOUNTPOINT="${2:-/tmp/simplefs-smoke-mnt}"
IMAGE_SIZE_MB="${IMAGE_SIZE_MB:-2}"
LOOP_DEV=""
HASHES_LOG="/tmp/simplefs-hashes.log"
FIRST_FILE=""

if [[ "${EUID}" -ne 0 ]]; then
	echo "Run as root: sudo $0 [image] [mountpoint]" >&2
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
	rm -f "${HASHES_LOG}"
}
trap cleanup EXIT

cd "${ROOT_DIR}"
make

mkdir -p "${MOUNTPOINT}"
truncate -s "${IMAGE_SIZE_MB}M" "${IMAGE}"
LOOP_DEV="$(losetup --find --show "${IMAGE}")"
LOOP_NAME="$(basename "${LOOP_DEV}")"

insmod "${ROOT_DIR}/simplefs.ko" \
	device_name="${LOOP_NAME}" \
	sb_primary_sector=0 \
	sb_backup_sector=8 \
	max_filename_len=32 \
	max_file_sectors=4

mount -t simplefs "${LOOP_DEV}" "${MOUNTPOINT}"

"${ROOT_DIR}/tools/simplefsctl" fill "${MOUNTPOINT}" >/tmp/simplefs-fill.log
"${ROOT_DIR}/tools/simplefsctl" hashes "${MOUNTPOINT}" >"${HASHES_LOG}"
head -n 5 "${HASHES_LOG}"
FIRST_FILE="$(awk 'NR == 1 { print $2 }' "${HASHES_LOG}")"
"${ROOT_DIR}/tools/simplefsctl" map "${MOUNTPOINT}" "${FIRST_FILE}"
"${ROOT_DIR}/tools/simplefsctl" zero "${MOUNTPOINT}"
"${ROOT_DIR}/tools/simplefsctl" wipe "${MOUNTPOINT}"

echo "simplefs smoke test passed"
