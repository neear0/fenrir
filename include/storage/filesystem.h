#pragma once

#include <string>
#include <vector>

#include "block_source.h"
#include "diagnostic.h"
#include "jobs.h"
#include "partition.h"
#include "recovery_candidate.h"

// every filesystem fenrir can read the records of, behind one call: what the session and the cli use, so they don't
// need to know which scanner is which
namespace storage {
	struct VolumeScan {
		FileSystem filesystem = FileSystem::unknown;
		std::string summary; // "FAT32, 512 byte clusters, 16,320 clusters (15,890 free), label FENRIR"
		std::vector<RecoveryCandidate> candidates; // directories before what's in them, ids are indexes into this
		std::vector<Diagnostic> diagnostics;
	};

	// FAT32, exFAT and NTFS
	bool can_scan(FileSystem filesystem) noexcept;

	// volume read as filesystem, which partition scanning guessed from its signature. one it can't read: unsupported
	Result<VolumeScan> scan_volume(IBlockSource& volume, FileSystem filesystem, const CancellationToken* cancel = nullptr) noexcept;
}
