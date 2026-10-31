// the signature scan: the source a chunk at a time, every aligned position checked against each format's signature,
// and every match handed to that format's validator, which reads on from there as far as it needs

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <new>
#include <unordered_set>

#include "formats.h"
#include "storage/bytes.h"

namespace {
	using namespace storage;
	using namespace storage::carving;

	constexpr std::size_t overlap = 16; // longer than any signature, so one that starts at a chunk's end is still seen whole
	constexpr std::size_t piece = 4096; // what's retried on its own when a chunk can't be read
	constexpr std::size_t max_candidates = 1'000'000;
	constexpr std::size_t max_diagnostics = 200;

	constexpr CarveFormat infos[] = {
		{ FileType::jpeg, "JPEG", "jpg", "its markers, and its image data decoded block by block (baseline JPEGs); progressive ones only by their markers" },
		{ FileType::png, "PNG", "png", "every chunk's CRC, from the header to the end chunk" },
		{ FileType::pdf, "PDF", "pdf", "every object, the cross-reference table and where startxref points, incremental updates too" },
		{ FileType::zip, "ZIP", "zip", "every local header, the central directory and its end record, and stored entries against their CRC" },
	};

	struct t_format {
		const CarveFormat& info;
		std::string_view signature;
		t_verdict (*validate)(t_reader&);
	};
	const t_format formats[] = {
		{ infos[0], "\xFF\xD8\xFF", validate_jpeg },
		{ infos[1], "\x89PNG\r\n\x1A\n", validate_png },
		{ infos[2], "%PDF-", validate_pdf },
		{ infos[3], "PK\x03\x04", validate_zip },
	};

	std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
		const std::uint64_t over = value % alignment;
		return over == 0 ? value : value + (alignment - over);
	}

	class t_carver {
	public:
		t_carver(IBlockSource& source, const CarveOptions& options) : source_(source), options_(options) {
			for (const t_format& f : formats)
				if (options.types.empty() || std::ranges::contains(options.types, f.info.type)) {
					enabled_.push_back(&f);
					starts_[static_cast<unsigned char>(f.signature[0])] = true;
				}
		}

		Result<CarveScan> run(const CancellationToken* cancel, ProgressSink* progress) {
			const std::uint64_t size = source_.size_bytes();
			const std::uint64_t a = options_.alignment;
			const std::size_t chunk = static_cast<std::size_t>(align_up(options_.chunk_size, a));
			std::unique_ptr<std::byte[]> buffer(new std::byte[chunk + overlap]);

			std::uint64_t resume = 0;
			for (std::uint64_t chunk_at = 0; chunk_at < size; chunk_at += chunk) {
				if (cancel && cancel->cancelled())
					return fail(Errc::cancelled, "carving");
				const std::uint64_t positions_end = std::min<std::uint64_t>(chunk_at + chunk, size);
				if (resume < positions_end) { // else the file found last goes past this chunk, nothing in it is looked at
					const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk + overlap, size - chunk_at));
					fill(chunk_at, { buffer.get(), n });
					for (std::uint64_t p = std::max(resume, align_up(chunk_at, a)); p < positions_end; p += a) {
						const std::size_t at = static_cast<std::size_t>(p - chunk_at);
						if (!starts_[std::to_integer<std::size_t>(buffer[at])])
							continue; // nearly every position, with --every-byte
						const std::uint64_t next = check(p, { buffer.get() + at, n - at }, size);
						if (scan_.candidates.size() >= max_candidates) {
							note(Diagnostic::Severity::warning, std::format("the carver stopped at byte {}: it found {} files, as many as it keeps", p, max_candidates));
							return std::move(scan_);
						}
						if (next > p) {
							resume = next;
							p = next - a; // the loop adds a back
						}
					}
				}
				if (progress)
					progress->progress(positions_end, size);
			}
			for (const auto& [from, to] : unreadable_)
				note(Diagnostic::Severity::warning, std::format("bytes {} to {} couldn't be read, nothing in them could be found", from, to - 1));
			return std::move(scan_);
		}

	private:
		// a file at p, if a signature matches and its validator agrees: where to look next, else 0
		std::uint64_t check(std::uint64_t p, std::span<const std::byte> here, std::uint64_t size) {
			for (const t_format* f : enabled_) {
				if (f->signature.size() > here.size() || std::memcmp(here.data(), f->signature.data(), f->signature.size()) != 0)
					continue;
				t_reader reader(source_, p, std::min(options_.max_file_size, size - p));
				t_verdict v = f->validate(reader);
				if (!v.file || v.recoverable == 0) {
					scan_.rejected++;
					continue;
				}
				RecoveryCandidate c;
				c.id = scan_.candidates.size();
				c.origin = Origin::carved;
				c.format = v.format.empty() ? f->info.name : v.format;
				c.name = std::format("{:010x}.{}", p, v.extension.empty() ? f->info.extension : v.extension);
				c.path = "/carved/" + c.name;
				c.size = v.size != 0 ? v.size : v.recoverable;
				c.recoverable_bytes = v.recoverable;
				c.extents = { Extent{ p, v.recoverable } };
				c.integrity = v.integrity;
				c.entry_offset = p;
				c.reasons.push_back(std::format("found by its content at byte {}: no name, path or dates are left with it", p));
				std::ranges::move(v.reasons, std::back_inserter(c.reasons));
				scan_.candidates.push_back(std::move(c));
				return align_up(p + v.recoverable, options_.alignment);
			}
			return 0;
		}

		// what can't be read is zeroed, which never starts a signature, and noted
		void fill(std::uint64_t offset, std::span<std::byte> out) {
			if (read_exact(source_, offset, out))
				return;
			for (std::size_t i = 0; i < out.size(); i += piece) {
				const std::span<std::byte> part = out.subspan(i, std::min(piece, out.size() - i));
				if (read_exact(source_, offset + i, part))
					continue;
				std::ranges::fill(part, std::byte{ 0 });
				const std::uint64_t from = offset + i, to = from + part.size();
				if (!unreadable_.empty() && unreadable_.back().second >= from)
					unreadable_.back().second = std::max(unreadable_.back().second, to);
				else
					unreadable_.emplace_back(from, to);
			}
		}

		void note(Diagnostic::Severity severity, std::string message) {
			if (scan_.diagnostics.size() < max_diagnostics)
				scan_.diagnostics.push_back({ severity, std::move(message) });
		}

		IBlockSource& source_;
		const CarveOptions& options_;
		std::vector<const t_format*> enabled_;
		std::array<bool, 256> starts_{}; // the first bytes of their signatures
		std::vector<std::pair<std::uint64_t, std::uint64_t>> unreadable_; // [from, to), merged, chunk overlaps read twice
		CarveScan scan_;
	};
}

bool storage::carving::t_reader::load(std::uint64_t offset) noexcept {
	const std::uint64_t end = source_.size_bytes() > start_ ? source_.size_bytes() - start_ : 0;
	if (offset >= limit_ || offset >= end) {
		limited_ |= offset >= limit_ && offset < end;
		return false;
	}
	if (unreadable_ && offset >= unreadable_at_)
		return false; // not retried: everything from the first failure on counts as gone
	try {
		window_.resize(window);
	} catch (const std::bad_alloc&) {
		unreadable_ = true;
		unreadable_at_ = offset;
		return false;
	}

	// the window, else just the sector with offset in it, in case what failed was further on
	const std::uint64_t stop = std::min(limit_, end);
	for (std::size_t want : { window, sector }) {
		window_at_ = offset - offset % sector;
		const std::size_t size = static_cast<std::size_t>(std::min<std::uint64_t>(want, stop - window_at_));
		if (read_exact(source_, start_ + window_at_, { window_.data(), size })) {
			window_size_ = size;
			return true;
		}
	}
	window_size_ = 0;
	unreadable_ = true;
	unreadable_at_ = offset - offset % sector;
	return false;
}

std::string storage::carving::stopped(const t_reader& reader, std::uint64_t at) {
	if (reader.unreadable() && at >= reader.unreadable_at())
		return std::format("it can't be read past byte {} of it", reader.unreadable_at());
	if (reader.limited())
		return std::format("it goes on past the {} byte limit, it isn't followed further", reader.limit());
	return std::format("the source ends {} bytes into it, before the file does", at);
}

std::span<const storage::CarveFormat> storage::carve_formats() noexcept { return infos; }

storage::Result<storage::CarveScan> storage::carve(IBlockSource& source, const CarveOptions& options, const CancellationToken* cancel, ProgressSink* progress) noexcept {
	if (options.alignment == 0 || options.alignment > (1u << 20) || options.chunk_size < piece || options.chunk_size > (std::size_t(1) << 30)
		|| options.max_file_size == 0)
		return fail(Errc::invalid_argument, "carving: the alignment, chunk size or file size limit is out of range");
	try {
		return t_carver(source, options).run(cancel, progress);
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "carving");
	}
}

std::size_t storage::drop_known(std::vector<RecoveryCandidate>& carved, std::span<const RecoveryCandidate> known, std::uint64_t base) {
	std::unordered_set<std::uint64_t> starts;
	for (const RecoveryCandidate& k : known)
		if (!k.directory && !k.extents.empty() && k.extents.front().length != 0)
			if (const auto start = checked_add(base, k.extents.front().offset))
				starts.insert(*start);
	const std::size_t dropped = std::erase_if(carved, [&](const RecoveryCandidate& c) { return !c.extents.empty() && starts.contains(c.extents.front().offset); });
	for (std::size_t i = 0; i < carved.size(); i++)
		carved[i].id = i;
	return dropped;
}
