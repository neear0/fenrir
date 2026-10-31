#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.h"
#include "hash.h"
#include "partition.h"
#include "recovery_candidate.h"
#include "result.h"

// a recovery session: one disk image, scanned and exported by the session's own threads while whoever drives it (the
// ui) reads snapshots of how far it got. the ui never touches the image: every read happens here, off its thread, and
// every write in the exporter. a scan reads the partition table, every FAT32 volume's directories, then (the deep scan)
// carves the whole image in slices, so files show up while it runs and a stopped deep scan can go on where it stopped
namespace storage {
	// an area of the image files were found in: the whole image (carving, or a disk without a partition table) or a
	// partition. found files' extents count from their volume's first byte
	struct SessionVolume {
		std::string name;                    // "the whole image", "partition 1"
		std::uint64_t offset = 0, size = 0;  // bytes, in the image
		FileSystem filesystem = FileSystem::unknown;
		bool scanned = false;                // its filesystem's records were read (FAT32 so far)
		std::uint64_t files = 0;             // found in it
	};

	struct FoundFile {
		RecoveryCandidate file;   // file.id is its index in SessionSnapshot::files, file.parent one too
		std::uint32_t volume = 0; // index into SessionSnapshot::volumes
	};

	struct ExportRecord {
		std::uint64_t id = 0;     // the FoundFile's
		std::string path;         // utf-8: what was written, empty when nothing was
		std::uint64_t bytes = 0;
		Sha256::Digest sha256{};  // of what was written
		bool complete = false;    // false for partial ones
		std::string error;        // empty when it was written
	};

	enum class SessionPhase : std::uint8_t {
		idle,        // nothing running
		opening,     // the image and its partition table
		filesystems, // FAT32 volumes' directories
		carving,     // the deep scan
		exporting,
	};
	std::string_view to_string(SessionPhase phase) noexcept;

	struct SessionSnapshot {
		std::uint64_t version = 0; // goes up with every change but progress: rebuild what's derived from the rest then
		SessionPhase phase = SessionPhase::idle;

		bool open = false;         // an image is open and its partition table read
		std::filesystem::path image;
		std::uint64_t image_size = 0;
		PartitionMap partitions;
		std::vector<SessionVolume> volumes;
		std::shared_ptr<const std::vector<FoundFile>> files; // never null, the same pointer until something's added
		std::vector<Diagnostic> diagnostics;                 // from every step, each saying where it came from
		std::uint64_t carve_rejected = 0, carve_dropped = 0; // signatures that weren't files, files a filesystem had already

		// the running job, or the last one
		std::uint64_t done = 0, total = 0; // bytes (files while reading filesystems)
		double seconds = 0.0;
		std::string status;                // what's going on or what happened, in plain words
		std::optional<Error> error;        // why the last job stopped short, Errc::cancelled when it was stopped

		// the deep scan covers [0, carved_to) of the image. stopped, resume() goes on from carved_to
		bool carving = false;              // the last scan asked for it
		std::uint64_t carved_to = 0;
		bool can_resume = false;

		// the last export
		std::shared_ptr<const std::vector<ExportRecord>> exported; // never null
		std::filesystem::path destination, report;
	};

	struct ScanOptions {
		bool carve = true;              // the deep scan, after the filesystems
		std::uint32_t alignment = 512;  // CarveOptions::alignment
	};

	struct FilePreview {
		std::uint64_t id = UINT64_MAX;  // the FoundFile it's of
		std::uint64_t offset = 0;       // in the image, of bytes[0]
		std::vector<std::byte> bytes;   // its first ones, up to 256
		std::string error;              // when they couldn't be read
		bool ready = false;
	};

	class RecoverySession {
	public:
		RecoverySession();
		~RecoverySession(); // stops what's running and waits for it
		RecoverySession(const RecoverySession&) = delete;
		RecoverySession& operator=(const RecoverySession&) = delete;

		// each starts a job and returns at once: false while another one runs
		bool scan(const std::filesystem::path& image, const ScanOptions& options = {}); // closes the last image
		bool resume(); // the deep scan, from where it was stopped
		// to new files in destination: FAT32 files under their partition's name and path, carved ones under carved/,
		// then a report of what was written. directories and unrecoverable files are left out
		bool export_files(std::vector<std::uint64_t> ids, const std::filesystem::path& destination);
		void cancel();   // the running job stops at its next step, what it found so far stays
		bool busy() const;
		void wait();     // until the running job is done

		std::shared_ptr<const SessionSnapshot> snapshot() const; // from any thread, a copy that doesn't change

		// a found file's first bytes, read on a thread of their own so asking never waits for the disk. the result
		// is there once it's ready, for the last id asked for
		void preview(std::uint64_t id);
		FilePreview preview_result() const;

	private:
		struct t_impl;
		std::unique_ptr<t_impl> impl_;
	};
}
