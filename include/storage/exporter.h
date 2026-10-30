#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "block_source.h"
#include "hash.h"
#include "jobs.h"
#include "recovery_candidate.h"

// the one place the engine writes: recovered files, as new files in a destination directory the user picked. it only
// reads the source (through IBlockSource, which can't write) and only ever creates files, it never opens one that's
// already there, so neither the image nor anything else in the destination can be overwritten.
// the destination still has to be on a different drive than a *device* being recovered from, or the export could land
// on the free space it's recovering. there are no device sources yet: when one comes, it has to check that first
namespace storage {
	struct ExportedFile {
		std::filesystem::path path; // what was written
		std::uint64_t bytes = 0;
		Sha256::Digest sha256{};    // of exactly what was written, hashed as it was read from the source
		bool complete = false;      // every byte of the file's size: false for partial ones
	};

	// writes candidate's recoverable bytes from volume (the source its scanner was given) to a new file in destination,
	// at the candidate's path: "/DCIM/photo.jpg" becomes destination/DCIM/photo.jpg. every name is made safe first
	// (safe_name()), so nothing read from the disk can point outside destination. a name that's taken gets " (2)" and
	// up, a partial file ".partial" on the end. directories are just created. unrecoverable candidates: unsupported
	Result<ExportedFile> export_candidate(IBlockSource& volume, const RecoveryCandidate& candidate, const std::filesystem::path& destination,
		const CancellationToken* cancel = nullptr, ProgressSink* progress = nullptr) noexcept;

	// a name from a disk as a single file name that's safe and valid on windows and posix: no separators, no control or
	// reserved characters, no "." or "..", no device names (CON, NUL, COM1...), no trailing dots or spaces, at most 200
	// bytes. utf-8 in and out
	std::string safe_name(std::string_view name);
}
