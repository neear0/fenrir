#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// what a filesystem scanner found: one file or directory, live or deleted, where its data is believed to be and how
// sure fenrir is about that. the same for every filesystem, so the ui, the exporter and the reports only know this
namespace storage {
	enum class Integrity : std::uint8_t {
		intact,        // every byte it needs is where it should be and nothing else claims it. for a deleted file that
		               // still rests on assumptions, the reasons say which
		partial,       // only the first recoverable_bytes are believed to be its own
		uncertain,     // its data may be there, but something contradicts it: export it, then check it
		unrecoverable, // its data is gone, or there's nothing that says where it was
	};
	std::string_view to_string(Integrity integrity) noexcept;

	// how it was found
	enum class Origin : std::uint8_t {
		filesystem, // a filesystem's own records: a directory entry, an mft record
		carved,     // its content, with no record of it left: no name, no path, no dates
	};

	struct Extent {
		std::uint64_t offset = 0; // bytes, from the start of the volume the scanner was given
		std::uint64_t length = 0;
		bool zeros = false;       // nothing on the disk: the file reads as zeros here (an ntfs sparse run, an exfat file past
		                          // its valid data length). offset means nothing then
	};

	// as the filesystem stored it: fat and exfat keep the local time of whoever wrote it, ntfs keeps utc
	struct Timestamp {
		std::uint16_t year = 0;
		std::uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0;
		std::uint16_t millisecond = 0;
		bool valid = false;  // false when the field was 0 or out of range
		bool date_only = false;
		bool utc = false;
	};

	struct RecoveryCandidate {
		static constexpr std::uint64_t no_parent = (std::numeric_limits<std::uint64_t>::max)(); // in brackets: windows.h may define max

		std::uint64_t id = 0;            // the same for the same volume scanned again: its place in the scan's order
		std::uint64_t parent = no_parent; // the id of the directory it's in
		std::string name;                // utf-8, the long name when there's a trustworthy one
		std::string short_name;          // fat's 8.3 name, empty on filesystems without one
		std::string path;                // "/DCIM/Holiday photo.jpg", '/' between names, whatever the filesystem uses
		bool deleted = false;
		bool directory = false;
		bool name_damaged = false;       // part of the name was lost and has been replaced with _

		std::uint64_t size = 0;              // what the directory entry says. carved: as far as its structure went
		std::uint64_t recoverable_bytes = 0; // what an export writes: size, or less for partial ones
		std::vector<Extent> extents;         // where the recoverable bytes are, in order
		std::vector<std::byte> resident;     // the bytes themselves, when the filesystem keeps them in the file's own record (ntfs
		                                     // resident data): extents is empty then
		Integrity integrity = Integrity::unrecoverable;
		std::vector<std::string> reasons;    // why that integrity, in plain words: what was checked and what was assumed

		Timestamp modified, created, accessed;
		std::uint32_t attributes = 0;   // the filesystem's own bits (fat: read-only 0x01, hidden 0x02, system 0x04, ...)
		std::uint64_t first_cluster = 0;
		std::uint64_t entry_offset = 0; // the evidence it came from: where its directory entry is, or for a carved file
		                                // where its first byte is

		Origin origin = Origin::filesystem;
		std::string format;             // carved: what its content was checked as, "JPEG", "PNG", "DOCX"...
	};
}
