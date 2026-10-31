#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "block_source.h"
#include "diagnostic.h"
#include "jobs.h"
#include "recovery_candidate.h"

// exfat, read-only: the boot region, the fat, the allocation bitmap, and every directory reachable from the root,
// deleted entry sets included. layout from microsoft's "exFAT file system specification". deleting a file on exfat
// clears the in-use bit of each of its directory entries and frees its clusters in the allocation bitmap. its size,
// first cluster and whether it was stored in one piece (NoFatChain) stay in the entries, and the fat usually keeps its chain, so
// unlike fat32 a deleted file in pieces can often still be followed
namespace storage {
	struct ExfatVolume {
		std::uint32_t bytes_per_sector = 0;
		std::uint32_t sectors_per_cluster = 0;
		std::uint32_t cluster_size = 0;       // bytes
		std::uint64_t total_sectors = 0;
		std::uint32_t fat_count = 0;          // 2 only with TexFAT
		std::uint32_t active_fat = 0;
		std::uint32_t cluster_count = 0;      // data clusters, numbered 2 to cluster_count + 1
		std::uint32_t root_cluster = 0;
		std::uint64_t fat_offset = 0;         // bytes, of the fat in use
		std::uint64_t heap_offset = 0;        // bytes, of cluster 2
		std::uint32_t serial = 0;
		std::uint16_t revision = 0;           // 0x0100 is 1.00
		bool dirty = false;                   // it wasn't unmounted cleanly
		std::string label;
		std::uint64_t free_clusters = 0;      // counted in the allocation bitmap
		bool from_backup_boot_region = false;
	};

	struct ExfatScan {
		ExfatVolume volume;
		std::vector<RecoveryCandidate> candidates; // directories before what's in them, ids are indexes into this
		std::vector<Diagnostic> diagnostics;
	};

	// volume is the partition alone, or a whole disk without a partition table. it's not an exfat volume: unsupported.
	// the boot region, the fat or the allocation bitmap failing to read: their error. a directory that can't be read is
	// a diagnostic, and the scan goes on
	Result<ExfatScan> scan_exfat(IBlockSource& volume, const CancellationToken* cancel = nullptr) noexcept;
}
