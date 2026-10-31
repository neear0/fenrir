#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "block_source.h"
#include "diagnostic.h"
#include "jobs.h"
#include "recovery_candidate.h"

// carving: files found by their content, where no filesystem record of them is left (deleted and overwritten
// metadata, a formatted volume, a lost partition). a signature only says where a file might start, so each one is
// followed through its format's own structure, and that's what decides whether it's a file, where it ends, and whether
// it was stored in one piece. what can't be checked that way is said in the candidate's reasons
namespace storage {
	enum class FileType : std::uint8_t { jpeg, png, pdf, zip };

	// what the carver knows. adding a format is a row in carving.cpp's table and a validator
	struct CarveFormat {
		FileType type;
		std::string_view name;      // "JPEG"
		std::string_view extension; // "jpg"
		std::string_view checked;   // what its validator checks, in plain words
	};
	std::span<const CarveFormat> carve_formats() noexcept;

	struct CarveOptions {
		// signatures are only looked for at multiples of this, from the source's first byte. files on a disk start at a
		// cluster, so 512 finds them and skips the copies embedded inside other files. 1 looks everywhere
		std::uint32_t alignment = 512;
		std::size_t chunk_size = std::size_t(4) << 20; // read at a time, at least 4096
		std::uint64_t max_file_size = std::uint64_t(1) << 30; // a file isn't followed further than this
		std::vector<FileType> types;                   // empty: all of them

		// only files that start in [from, to) are looked for, a file found is still followed past to. a carve in
		// slices goes on from the last one's CarveScan::next, and finds what one carve of everything would
		std::uint64_t from = 0;
		std::uint64_t to = UINT64_MAX;
	};

	struct CarveScan {
		std::vector<RecoveryCandidate> candidates; // in the order they're on the source, ids are indexes into this
		std::vector<Diagnostic> diagnostics;
		std::uint64_t rejected = 0; // signatures whose structure didn't hold up: false matches, or too little left
		std::uint64_t next = 0;     // where a carve of what comes after should start: to, or past the last file if it goes on
	};

	// every file of the chosen types in source. after a file is found the search goes on past its end, so what's
	// inside one isn't found again. invalid options: invalid_argument. unreadable parts are diagnostics, the scan goes on.
	// progress counts the bytes from `from` to `to` (or the end)
	Result<CarveScan> carve(IBlockSource& source, const CarveOptions& options = {}, const CancellationToken* cancel = nullptr,
		ProgressSink* progress = nullptr) noexcept;

	// takes out the carved files whose first byte is where a file in known starts: a filesystem scan found them
	// already, with their names. known's extents are counted from base in the carved source: a partition's first byte,
	// when the whole disk was carved. ids are renumbered. returns how many were taken out
	std::size_t drop_known(std::vector<RecoveryCandidate>& carved, std::span<const RecoveryCandidate> known, std::uint64_t base = 0);
}
