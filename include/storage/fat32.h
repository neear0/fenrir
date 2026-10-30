#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "block_source.h"
#include "diagnostic.h"
#include "jobs.h"
#include "recovery_candidate.h"

// fat32, read-only: the boot sector, the fat, and every directory reachable from the root, deleted ones included.
// layout from microsoft's "FAT: General Overview of On-Disk Format" (1.03). what's recoverable of a deleted file is
// worked out from what fat leaves behind: deleting frees the file's clusters and marks its directory entries, the start
// cluster and size stay, the chain doesn't. so a deleted file is assumed to have been in one piece, and every cluster
// that assumption needs is checked to still be free
namespace storage {
	struct Fat32Volume {
		std::uint32_t bytes_per_sector = 0;
		std::uint32_t sectors_per_cluster = 0;
		std::uint32_t cluster_size = 0;     // bytes
		std::uint32_t reserved_sectors = 0;
		std::uint32_t fat_count = 0;
		std::uint32_t fat_sectors = 0;      // per fat
		std::uint64_t total_sectors = 0;
		std::uint32_t root_cluster = 0;
		std::uint32_t cluster_count = 0;    // data clusters, numbered 2 to cluster_count + 1
		std::uint64_t fat_offset = 0;       // bytes, of the fat in use
		std::uint64_t data_offset = 0;      // bytes, of cluster 2
		std::uint32_t active_fat = 0;       // 0 unless mirroring is off
		std::uint32_t volume_id = 0;
		std::string label;                  // from the root directory's label entry, or the boot sector's
		std::uint32_t free_clusters = 0;    // counted in the fat, not taken from fsinfo
		bool from_backup_boot_sector = false;
	};

	struct Fat32Scan {
		Fat32Volume volume;
		std::vector<RecoveryCandidate> candidates; // directories before what's in them, ids are indexes into this
		std::vector<Diagnostic> diagnostics;
	};

	// volume is the partition alone (open_partition()), or a whole disk without a partition table. it's not a fat32
	// volume: unsupported. reads of the boot sector and the fat failing: their error. a directory that can't be read
	// is a diagnostic, and the scan goes on
	Result<Fat32Scan> scan_fat32(IBlockSource& volume, const CancellationToken* cancel = nullptr) noexcept;
}
