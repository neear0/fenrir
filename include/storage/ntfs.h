#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "block_source.h"
#include "diagnostic.h"
#include "jobs.h"
#include "recovery_candidate.h"

// ntfs, read-only: the boot sector, every record of the master file table and the cluster bitmap. deleting a file on
// ntfs clears the in-use flag of its mft record and frees its clusters in $Bitmap, the record itself (names, times,
// parent, and where every piece of its data was) stays until the record is reused. so a deleted file's data runs are
// still known, fragmented or not, and what decides how much of it is left is whether its clusters are still free.
// the directory tree is built from each record's parent reference, not from the directories' indexes, which drop
// deleted files. layout from microsoft's ntfs developer notes and the linux-ntfs documentation
namespace storage {
	struct NtfsVolume {
		std::uint32_t bytes_per_sector = 0;
		std::uint32_t sectors_per_cluster = 0;
		std::uint32_t cluster_size = 0;     // bytes
		std::uint64_t total_sectors = 0;
		std::uint64_t cluster_count = 0;
		std::uint64_t mft_cluster = 0, mft_mirror_cluster = 0;
		std::uint32_t record_size = 0;      // bytes, of an mft record
		std::uint64_t serial = 0;
		std::uint8_t major_version = 0, minor_version = 0; // from $Volume, 3.1 since windows xp
		bool dirty = false;                 // it wasn't unmounted cleanly
		std::string label;
		std::uint64_t records = 0;          // in the mft, as far as it was read
		std::uint64_t free_clusters = 0;    // counted in $Bitmap
		bool from_backup_boot_sector = false;
		bool mft_from_mirror = false;       // the mft's own record was damaged, $MFTMirr's copy was used
	};

	struct NtfsScan {
		NtfsVolume volume;
		std::vector<RecoveryCandidate> candidates; // directories before what's in them, ids are indexes into this
		std::vector<Diagnostic> diagnostics;
	};

	// volume is the partition alone, or a whole disk without a partition table. it's not an ntfs volume: unsupported.
	// the boot sector, the mft's own record or $Bitmap failing to read: their error. mft records that can't be read
	// or are damaged are counted in a diagnostic, and the scan goes on
	Result<NtfsScan> scan_ntfs(IBlockSource& volume, const CancellationToken* cancel = nullptr) noexcept;
}
