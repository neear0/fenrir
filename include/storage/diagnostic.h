#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace storage {
	// something a scan noticed about the disk's structures. scans of damaged disks keep going and say what they found
	// here, rather than failing
	struct Diagnostic {
		enum class Severity : std::uint8_t {
			info,    // worth knowing, nothing's wrong: e.g. a hybrid mbr
			warning, // something's damaged or odd, what was found is still usable
			error,   // something's damaged enough that part of what was found is missing or can't be trusted
		};
		Severity severity;
		std::string message; // numbers and offsets only, never file names or anything else read from a filesystem
	};
	std::string_view to_string(Diagnostic::Severity severity) noexcept;

	bool has(const std::vector<Diagnostic>& diagnostics, Diagnostic::Severity severity) noexcept;
}
