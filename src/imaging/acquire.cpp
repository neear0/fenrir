// a copy of a source that may be failing, ddrescue's way: the easy parts first, then closer and closer around what
// failed. reads only through IBlockSource, writes only through image_file.h

#include <algorithm>
#include <bit>
#include <chrono>
#include <format>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#include "image_file.h"
#include "storage/bytes.h"
#include "storage/file_image_source.h"
#include "storage/imaging.h"

namespace {
	using namespace storage;
	namespace fs = std::filesystem;
	using clock = std::chrono::steady_clock;

	constexpr std::uint64_t max_map_bytes = std::uint64_t(1) << 30;
	constexpr std::uint32_t max_cluster = 64u << 20;
	constexpr std::size_t samples = 16; // sectors compared between the source and the image before a copy goes on

	struct t_read {
		std::size_t got = 0; // read before it failed, or all of it
		bool failed = false;
	};

	struct t_copy {
		IBlockSource& source;
		imaging::ImageFile image;
		fs::path mapfile; // already there: saved before the copy starts
		ImageMap map;
		const ImagingOptions& o;
		const CancellationToken* cancel;
		ImagingObserver* observer;
		ImagingStatus status;
		std::vector<std::byte> buffer;
		clock::time_point last_save = clock::now();

		const ImagingStatus& refresh() {
			status.phase = map.phase;
			status.pass = map.pass;
			status.pos = map.current_pos;
			status.size = map.size();
			status.finished = map.bytes(BlockStatus::finished);
			status.bad = map.bytes(BlockStatus::bad);
			status.unknown = map.unknown_bytes();
			return status;
		}

		// the image flushed first, then the map: the map on disk never says more is in the image than is
		Result<void> save(bool now) {
			if (!now && clock::now() - last_save < o.save_every)
				return {};
			if (Result<void> flushed = image.flush(); !flushed)
				return flushed;
			const auto time = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
			const std::string comment = std::format("Saved: {:%Y-%m-%d %H:%M:%S} UTC{}", time, map.phase == ImagingPhase::finished ? "\nFinished" : "");
			if (Result<void> saved = imaging::save_text(mapfile, map.serialize(comment), true); !saved)
				return saved;
			last_save = clock::now();
			if (observer)
				observer->saved(refresh());
			return {};
		}

		// [pos, pos + length) into buffer, as far as it goes before a read fails
		t_read read(std::uint64_t pos, std::size_t length) noexcept {
			t_read r;
			while (r.got < length) {
				Result<std::size_t> got = source.read_at(pos + r.got, std::span(buffer).subspan(r.got, length - r.got));
				if (!got || *got == 0) { // 0 before the end: the source got shorter
					r.failed = true;
					break;
				}
				r.got += *got;
			}
			return r;
		}

		// reads [pos, pos + length), writes what it got into the image, marks that finished and the rest on_fail
		Result<t_read> piece(std::uint64_t pos, std::size_t length, BlockStatus on_fail) {
			map.current_pos = pos;
			const t_read r = read(pos, length);
			if (r.got > 0) {
				if (Result<void> wrote = image.write_at(pos, std::span(buffer).first(r.got)); !wrote) {
					(void)save(true); // what was written before this is still worth keeping
					return std::unexpected(wrote.error());
				}
				map.set(pos, r.got, BlockStatus::finished);
			}
			if (r.failed) {
				map.set(pos + r.got, length - r.got, on_fail);
				status.read_errors++;
			}

			if (cancel && cancel->cancelled()) {
				if (Result<void> saved = save(true); !saved)
					return std::unexpected(saved.error());
				return fail(Errc::cancelled, "imaging");
			}
			if (o.max_read_errors != 0 && status.read_errors >= o.max_read_errors) {
				if (Result<void> saved = save(true); !saved)
					return std::unexpected(saved.error());
				return fail(Errc::io_error, "imaging: it stopped after the most read errors it was allowed");
			}
			if (observer)
				observer->update(refresh(), map);
			if (Result<void> saved = save(false); !saved)
				return std::unexpected(saved.error());
			return r;
		}

		// from pos to the end of its sector, or to end when that's sooner
		std::size_t sector_from(std::uint64_t pos, std::uint64_t end) const noexcept {
			return static_cast<std::size_t>(std::min<std::uint64_t>(o.sector_size - pos % o.sector_size, end - pos));
		}

		void begin(ImagingPhase phase, unsigned pass) {
			map.phase = phase;
			map.pass = pass;
			map.current_pos = 0;
		}

		// what hasn't been tried, a cluster at a time. pass 1 skips ahead after a failed read, further each time, so a
		// bad area costs a few failed reads instead of one per cluster; pass 2 goes back for what was skipped
		Result<void> copy(unsigned pass) {
			begin(ImagingPhase::copying, pass);
			const bool skipping = pass == 1 && o.skip_max > 0 && o.skip_min > 0;
			std::uint64_t pos = 0, skip = o.skip_min;
			while (const std::optional<MapBlock> block = map.find(BlockStatus::non_tried, pos)) {
				pos = block->pos;
				const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(o.cluster_size - pos % o.cluster_size, block->end() - pos));
				Result<t_read> r = piece(pos, length, BlockStatus::non_trimmed);
				if (!r)
					return std::unexpected(r.error());
				pos += length;
				if (r->failed && skipping) {
					pos = checked_add(pos, skip).value_or(map.size());
					pos -= pos % o.sector_size;
					skip = skip > o.skip_max / 2 ? o.skip_max : skip * 2;
				} else if (!r->failed)
					skip = o.skip_min;
			}
			return save(true);
		}

		// each failed area's edges, sector by sector from both ends until a sector fails: what's between those two bad
		// sectors is left for scraping. most failed reads only had a few bad sectors in them
		Result<void> trim() {
			begin(ImagingPhase::trimming, 1);
			std::uint64_t pos = 0;
			while (const std::optional<MapBlock> block = map.find(BlockStatus::non_trimmed, pos)) {
				pos = block->end();
				if (!o.trim) {
					map.set(block->pos, block->size, BlockStatus::non_scraped);
					continue;
				}
				std::uint64_t front = block->pos, back = block->end();
				bool hit = false;
				while (front < back && !hit) {
					const std::size_t length = sector_from(front, back);
					Result<t_read> r = piece(front, length, BlockStatus::bad);
					if (!r)
						return std::unexpected(r.error());
					hit = r->failed;
					front += length;
				}
				while (back > front) {
					const std::uint64_t start = std::max(back - 1 - (back - 1) % o.sector_size, front);
					Result<t_read> r = piece(start, static_cast<std::size_t>(back - start), BlockStatus::bad);
					if (!r)
						return std::unexpected(r.error());
					back = start;
					if (r->failed)
						break;
				}
				if (back > front)
					map.set(front, back - front, BlockStatus::non_scraped);
			}
			return save(true);
		}

		// every sector of the blocks marked which, once
		Result<void> sector_pass(BlockStatus which, ImagingPhase phase, unsigned pass) {
			begin(phase, pass);
			std::uint64_t pos = 0;
			while (const std::optional<MapBlock> block = map.find(which, pos)) {
				pos = block->end();
				for (std::uint64_t at = block->pos; at < block->end();) {
					const std::size_t length = sector_from(at, block->end());
					if (Result<t_read> r = piece(at, length, BlockStatus::bad); !r)
						return std::unexpected(r.error());
					at += length;
				}
			}
			return save(true);
		}

		Result<ImagingStatus> run() {
			if (Result<void> r = copy(1); !r)
				return std::unexpected(r.error());
			if (Result<void> r = copy(2); !r)
				return std::unexpected(r.error());
			if (Result<void> r = trim(); !r)
				return std::unexpected(r.error());
			if (o.scrape)
				if (Result<void> r = sector_pass(BlockStatus::non_scraped, ImagingPhase::scraping, 1); !r)
					return std::unexpected(r.error());
			for (unsigned pass = 1; pass <= o.retries; pass++)
				if (Result<void> r = sector_pass(BlockStatus::bad, ImagingPhase::retrying, pass); !r)
					return std::unexpected(r.error());
			begin(ImagingPhase::finished, 1);
			map.current_pos = map.size();
			if (Result<void> r = save(true); !r)
				return std::unexpected(r.error());
			return refresh();
		}
	};

	Result<void> check_options(const ImagingOptions& o) {
		if (o.sector_size == 0 || !std::has_single_bit(o.sector_size))
			return fail(Errc::invalid_argument, "imaging: the sector size isn't a power of two");
		if (o.cluster_size == 0 || o.cluster_size % o.sector_size != 0 || o.cluster_size > max_cluster)
			return fail(Errc::invalid_argument, "imaging: the cluster size isn't a multiple of the sector size up to 64 MiB");
		if (o.skip_min > o.skip_max)
			return fail(Errc::invalid_argument, "imaging: the smallest skip is bigger than the largest");
		return {};
	}

	bool same_path(const fs::path& a, const fs::path& b) {
		std::error_code error;
		if (fs::equivalent(a, b, error))
			return true;
		return fs::absolute(a, error).lexically_normal() == fs::absolute(b, error).lexically_normal();
	}

	// sectors spread over what the map says was copied, read from both: a different source or image, or one that
	// changed since, doesn't match. a sector the source can't read now is passed over, it's a failing disk
	Result<void> check_samples(IBlockSource& source, imaging::ImageFile& image, const ImageMap& map, std::uint32_t sector_size) {
		std::vector<MapBlock> finished;
		for (const MapBlock& block : map.blocks())
			if (block.status == BlockStatus::finished)
				finished.push_back(block);
		if (finished.empty())
			return {};
		std::vector<std::byte> from_source(sector_size), from_image(sector_size);
		const std::size_t count = std::min(samples, finished.size());
		for (std::size_t i = 0; i < count; i++) {
			const MapBlock& block = finished[i * finished.size() / count];
			const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(sector_size, block.size));
			if (!read_exact(source, block.pos, std::span(from_source).first(length)))
				continue;
			if (Result<void> read = image.read_at(block.pos, std::span(from_image).first(length)); !read)
				return read;
			if (!std::equal(from_source.begin(), from_source.begin() + static_cast<std::ptrdiff_t>(length), from_image.begin()))
				return fail(Errc::mismatch, "imaging: what's already in the image isn't what the source has there");
		}
		return {};
	}

	Result<ImagingStatus> acquire(IBlockSource& source, const fs::path& image, const fs::path& mapfile, const ImagingOptions& o,
		const CancellationToken* cancel, ImagingObserver* observer) {
		if (Result<void> checked = check_options(o); !checked)
			return std::unexpected(checked.error());
		if (source.size_bytes() == 0)
			return fail(Errc::invalid_argument, "imaging: the source is empty");
		if (image.empty() || mapfile.empty() || is_device_path(image) || is_device_path(mapfile))
			return fail(Errc::unsupported, "imaging: the image and its map have to be files");
		const fs::path pending = imaging::pending_path(mapfile);
		if (same_path(image, mapfile) || same_path(image, pending))
			return fail(Errc::invalid_argument, "imaging: the image and its map are the same file");

		std::error_code error;
		const bool have_map = fs::exists(mapfile, error);
		const bool have_image = fs::exists(image, error);
		if (error)
			return fail(Errc::io_error, "imaging: looking for the image and its map", static_cast<std::uint32_t>(error.value()));

		// a map save that was cut off leaves its .new file: removed when it's one of this copy's maps, otherwise left alone
		if (fs::exists(pending, error)) {
			Result<ImageMap> leftover = load_map(pending);
			if (!leftover || leftover->size() != source.size_bytes())
				return fail(Errc::exists, "imaging: there's a file where the map is saved before it's moved in place");
			if (Result<void> removed = imaging::remove_pending(pending); !removed)
				return std::unexpected(removed.error());
		}

		ImageMap map(source.size_bytes());
		bool resumed = false;
		if (have_map) {
			Result<ImageMap> loaded = load_map(mapfile);
			if (!loaded)
				return std::unexpected(loaded.error());
			if (loaded->size() != source.size_bytes())
				return fail(Errc::mismatch, "imaging: the map is of a source with a different size");
			if (!have_image && loaded->bytes(BlockStatus::finished) != 0)
				return fail(Errc::mismatch, "imaging: the map says data was copied, but its image isn't there");
			map = std::move(*loaded);
			resumed = true;
		} else if (have_image)
			return fail(Errc::exists, "imaging: the image is already there, without a map to go on from");

		Result<imaging::ImageFile> file = fail(Errc::not_found, "imaging");
		if (have_image) {
			file = imaging::ImageFile::open_existing(image);
			if (!file)
				return std::unexpected(file.error());
			if (file->size() != source.size_bytes())
				return fail(Errc::mismatch, "imaging: the image isn't the source's size");
			if (Result<void> same = check_samples(source, *file, map, o.sector_size); !same)
				return std::unexpected(same.error());
		} else {
			const fs::space_info space = fs::space(fs::absolute(image, error).parent_path(), error);
			if (!error && space.available < source.size_bytes())
				return fail(Errc::no_space, "imaging: the image's drive doesn't have room for it");
			// the map before the image: a crash in between leaves a map with nothing copied, which the next run goes on
			// from by making the image. the other way round would leave an image without a map, which is never touched
			if (!have_map)
				if (Result<void> saved = imaging::save_text(mapfile, map.serialize(), false); !saved)
					return std::unexpected(saved.error());
			file = imaging::ImageFile::create_new(image, source.size_bytes());
			if (!file)
				return std::unexpected(file.error());
		}

		t_copy copy{ source, std::move(*file), mapfile, std::move(map), o, cancel, observer };
		copy.status.resumed = resumed;
		copy.buffer.resize(o.cluster_size);
		return copy.run();
	}
}

storage::Result<storage::ImageMap> storage::load_map(const std::filesystem::path& mapfile) noexcept {
	try {
		Result<std::unique_ptr<FileImageSource>> file = FileImageSource::open(mapfile);
		if (!file)
			return std::unexpected(file.error());
		const std::uint64_t size = (*file)->size_bytes();
		if (size > max_map_bytes)
			return fail(Errc::invalid_argument, "reading a mapfile: it's too big to be one");
		std::string text(static_cast<std::size_t>(size), '\0');
		if (Result<void> read = read_exact(**file, 0, std::as_writable_bytes(std::span(text))); !read)
			return std::unexpected(read.error());
		return ImageMap::parse(text);
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "reading a mapfile");
	}
}

storage::Result<storage::ImagingStatus> storage::acquire_image(IBlockSource& source, const std::filesystem::path& image, const std::filesystem::path& mapfile,
	const ImagingOptions& options, const CancellationToken* cancel, ImagingObserver* observer) noexcept {
	try {
		return acquire(source, image, mapfile, options, cancel, observer);
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "imaging");
	} catch (const std::filesystem::filesystem_error& e) {
		return fail(Errc::io_error, "imaging", static_cast<std::uint32_t>(e.code().value()));
	}
}
