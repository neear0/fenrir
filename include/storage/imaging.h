#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "block_source.h"
#include "jobs.h"
#include "result.h"

// imaging a disk that may be failing: everything that can be read is copied into an image file, next to a map of which
// parts of the image are copies and which aren't. a stopped copy goes on later from its map, and nothing that wasn't
// read can pass for data: the parts of the image that weren't read are zeros only because the file was made that size.
// the map is a GNU ddrescue mapfile (the format, not ddrescue's code), so ddrescue and ddrescueview can read Fenrir's
// maps and Fenrir can go on with a copy ddrescue started
namespace storage {
	// what's known about a stretch of the source, as the characters ddrescue writes for it
	enum class BlockStatus : char {
		non_tried = '?',   // not read yet
		non_trimmed = '*', // a read covering it failed, and its edges haven't been read sector by sector yet
		non_scraped = '/', // between the trimmed edges of a failed read, not read sector by sector yet
		bad = '-',         // its sectors failed every read tried so far
		finished = '+',    // read, and in the image
	};

	// how far a copy got, the status line's character: the pass it's in, or finished
	enum class ImagingPhase : char {
		copying = '?',
		trimming = '*',
		scraping = '/',
		retrying = '-',
		finished = '+',
	};

	struct MapBlock {
		std::uint64_t pos = 0;
		std::uint64_t size = 0;
		BlockStatus status = BlockStatus::non_tried;
		std::uint64_t end() const noexcept { return pos + size; }
	};

	// the whole source, [0, size()), as blocks that each have one status: no gaps, no overlaps, and neighbours with the
	// same status merged
	class ImageMap {
	public:
		// a source of size bytes, none of it read yet
		explicit ImageMap(std::uint64_t size = 0);

		// a mapfile's text: comment lines start with '#', then a status line (position, phase and, as ddrescue writes it
		// since 1.21, the pass), then one line per block (position, size, status). numbers in hex (0x...) or decimal.
		// the blocks have to start at 0 and follow each other with no gap. anything else is invalid_argument: a map read
		// from a file is hostile input like any other
		static Result<ImageMap> parse(std::string_view text) noexcept;
		// the map as a mapfile, the way ddrescue writes one. every line of comment becomes a "# " line at the top
		std::string serialize(std::string_view comment = {}) const;

		std::uint64_t size() const noexcept { return size_; }
		std::size_t block_count() const noexcept { return blocks_.size(); }
		std::vector<MapBlock> blocks() const;

		// [pos, pos + size) gets status, cut to the map's size
		void set(std::uint64_t pos, std::uint64_t size, BlockStatus status);
		// the block pos is in. pos has to be < size()
		MapBlock block_at(std::uint64_t pos) const noexcept;
		// the first stretch at or after from that has status, starting at from when from's block has it
		std::optional<MapBlock> find(BlockStatus status, std::uint64_t from = 0) const noexcept;
		// how many bytes have status
		std::uint64_t bytes(BlockStatus status) const noexcept;
		// bytes that are neither finished nor bad: not tried, or not tried closely enough yet
		std::uint64_t unknown_bytes() const noexcept {
			return bytes(BlockStatus::non_tried) + bytes(BlockStatus::non_trimmed) + bytes(BlockStatus::non_scraped);
		}

		// the status line
		ImagingPhase phase = ImagingPhase::copying;
		std::uint64_t current_pos = 0;
		unsigned pass = 1;

	private:
		struct t_block {
			std::uint64_t size;
			BlockStatus status;
		};
		void split(std::uint64_t at);
		void count(BlockStatus status, std::uint64_t size, bool add) noexcept;

		std::uint64_t size_ = 0;
		std::map<std::uint64_t, t_block> blocks_; // by position
		std::uint64_t totals_[5]{};               // bytes per status
	};

	struct ImagingOptions {
		std::uint32_t sector_size = 512;     // the smallest read, when trimming, scraping and retrying. a power of two
		std::uint32_t cluster_size = 65536;  // reads while copying, a multiple of sector_size
		std::uint64_t skip_min = 65536;      // after a failed read in the first pass, skip this far ahead, then twice as far
		std::uint64_t skip_max = 1u << 30;   // while reads keep failing, up to this. 0: no skipping
		bool trim = true;                    // read the edges of failed reads sector by sector
		bool scrape = true;                  // then everything between them
		unsigned retries = 0;                // then this many more passes over the bad sectors
		std::uint64_t max_read_errors = 0;   // stop after this many failed reads (the map is kept), 0: never
		std::chrono::milliseconds save_every{ 10000 }; // the map is saved this often, after each pass and at the end. 0: after every read
	};

	struct ImagingStatus {
		ImagingPhase phase = ImagingPhase::copying;
		unsigned pass = 1;
		std::uint64_t pos = 0;  // where it's reading
		std::uint64_t size = 0; // of the source
		std::uint64_t finished = 0, bad = 0, unknown = 0; // bytes, as in the map
		std::uint64_t read_errors = 0; // failed reads in this run
		bool resumed = false;   // it went on from a map that was already there
	};

	// told how a copy's going, on the copy's own thread: after every read and every time the map has been saved
	class ImagingObserver {
	public:
		virtual ~ImagingObserver() = default;
		virtual void update(const ImagingStatus& status, const ImageMap& map) noexcept = 0;
		// the image has been flushed and mapfile now says what's in it
		virtual void saved(const ImagingStatus&) noexcept {}
	};

	// copies source into the file image, keeping mapfile up to date. both new: image is created at source's size and
	// mapfile next to it. mapfile already there: the copy goes on from it, into the image it was made with, if their
	// sizes match source's and samples of what's been copied still read the same from both (mismatch when not). an
	// image without its mapfile, or a mapfile without its image, are left alone (exists, mismatch): nothing here ever
	// replaces a file other than a map it's continuing.
	// the passes are ddrescue's: copying in clusters (skipping ahead past failures, then going back for what was
	// skipped), trimming failed areas' edges, scraping what's between them, then retrying bad sectors. a failed read
	// marks the area, it never stops the copy; a failed write to the image does. data is flushed to the image before
	// any map that counts it as finished is saved, so after a crash the map can be behind the image but never ahead of it.
	// cancelled: the map is saved first, so the copy can go on later
	Result<ImagingStatus> acquire_image(IBlockSource& source, const std::filesystem::path& image, const std::filesystem::path& mapfile,
		const ImagingOptions& options = {}, const CancellationToken* cancel = nullptr, ImagingObserver* observer = nullptr) noexcept;

	// mapfile read and parsed, for showing a copy's state
	Result<ImageMap> load_map(const std::filesystem::path& mapfile) noexcept;

	// a slice of a map for drawing it as a bar: how much of it is copied and how much bad, the rest isn't read yet
	struct MapCell {
		float finished = 0.f, bad = 0.f; // fractions of the slice
	};
	// the map in cells equal slices (fewer when it's smaller than that)
	std::vector<MapCell> summarize(const ImageMap& map, std::size_t cells);
}
