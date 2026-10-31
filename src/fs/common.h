#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "storage/diagnostic.h"
#include "storage/recovery_candidate.h"

// what the filesystem scanners share: names into utf-8, notes kept to a sane number, and deleted files that claim the
// same clusters
namespace storage::fsutil {
	// control characters and stray surrogates become U+FFFD: names end up in terminals and the ui
	void put_utf8(std::string& out, char32_t c);
	std::string utf8(std::u16string_view text);

	// clusters [first, end) a deleted file's recovery relies on. a file in pieces has a claim per piece
	struct Claim {
		std::uint64_t first, end;
		std::size_t candidate; // index into the scan's candidates
	};

	// deleted files whose claims overlap: at most one of them still has its data there, so intact and partial ones
	// become uncertain, and each says which other file it shares clusters with. a pair is only checked a limited
	// number of claims apart, so thousands of entries on one cluster stay cheap
	void mark_shared_claims(std::vector<Claim>& claims, std::vector<RecoveryCandidate>& candidates);

	// diagnostics, up to a limit after which one line says there were more
	void note(std::vector<Diagnostic>& diagnostics, Diagnostic::Severity severity, std::string message);

	constexpr std::uint64_t max_table_bytes = 256ull << 20; // a fat or a cluster bitmap, more than any real volume needs
	constexpr int max_depth = 64;                           // directories nested deeper aren't read
}
