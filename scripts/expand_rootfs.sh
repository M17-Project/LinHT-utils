#!/bin/bash

set -e  # Exit on any error

# Function to log messages
log() {
    echo "$(date): $1" | tee -a /var/log/expand-rootfs.log
}

# Check if already expanded
if [ -f /var/lib/expand-rootfs-done ]; then
    log "Root filesystem already expanded"
    exit 0
fi

log "Starting root filesystem expansion"

# Get root partition
ROOT_PART=$(findmnt / -o source -n | head -1)
if [ -z "$ROOT_PART" ]; then
    log "Error: Could not detect root partition"
    exit 1
fi

log "Root partition detected: $ROOT_PART"

# Extract device name (remove partition number)
ROOT_DEV=$(echo "$ROOT_PART" | sed 's/p[0-9]*$//')
PART_NUM=$(echo "$ROOT_PART" | sed 's/.*p//' | sed 's/.*[^0-9]//')

log "Root device: $ROOT_DEV, Partition: $PART_NUM"

# Verify device exists
if [ ! -b "$ROOT_DEV" ]; then
    log "Error: Device $ROOT_DEV not found"
    exit 1
fi

# Use parted to expand partition to 100% of disk
log "Expanding partition $PART_NUM on $ROOT_DEV"
parted "$ROOT_DEV" --script resizepart "$PART_NUM" 100%

if [ $? -ne 0 ]; then
    log "Error: Failed to resize partition"
    exit 1
fi

# Update partition table
partprobe "$ROOT_DEV" || partx -u "$ROOT_PART"

# Wait a moment for kernel to recognize changes
sleep 2

# Expand filesystem
log "Expanding filesystem on $ROOT_PART"
if command -v resize2fs >/dev/null 2>&1; then
    resize2fs "$ROOT_PART"
    if [ $? -eq 0 ]; then
        log "Filesystem expansion completed successfully"
    else
        log "Error: Failed to expand filesystem"
        exit 1
    fi
else
    log "Error: resize2fs command not found"
    exit 1
fi

# Mark as completed
touch /var/lib/expand-rootfs-done
log "Root filesystem expansion completed"

exit 0
