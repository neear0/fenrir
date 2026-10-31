#pragma once

// the carver's insides: a reader the validators share, and one validator per format. every validator gets the bytes
// from a signature on and says whether they're a file, how far they're its own, and why

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "storage/block_source.h"
#include "storage/carving.h"

namespace storage::carving {
	// the source from a candidate's first byte, read a window at a time. the end of the source, the size limit and a
	// read that fails all look the same to a validator, there's no byte there; which one it was is kept for its reasons
	class t_reader {
	public:
		t_reader(IBlockSource& source, std::uint64_t start, std::uint64_t limit) noexcept : source_(source), start_(start), limit_(limit) {}

		// the byte at offset from the start, or -1
		int at(std::uint64_t offset) noexcept {
			if (offset - window_at_ < window_size_) // unsigned: below the window wraps round to huge
				return std::to_integer<int>(window_[static_cast<std::size_t>(offset - window_at_)]);
			return load(offset) ? std::to_integer<int>(window_[static_cast<std::size_t>(offset - window_at_)]) : -1;
		}

		bool matches(std::uint64_t offset, std::string_view text) noexcept {
			for (std::size_t i = 0; i < text.size(); i++)
				if (at(offset + i) != static_cast<unsigned char>(text[i]))
					return false;
			return true;
		}

		// big and little endian numbers, -1 when any byte is missing
		std::int64_t be16(std::uint64_t offset) noexcept { return number(offset, 2, true); }
		std::int64_t be32(std::uint64_t offset) noexcept { return number(offset, 4, true); }
		std::int64_t le16(std::uint64_t offset) noexcept { return number(offset, 2, false); }
		std::int64_t le32(std::uint64_t offset) noexcept { return number(offset, 4, false); }

		// length bytes at offset into out, false unless all of them are there
		bool read(std::uint64_t offset, std::span<std::byte> out) noexcept {
			for (std::size_t i = 0; i < out.size();) {
				if (at(offset + i) < 0)
					return false;
				const std::size_t from = static_cast<std::size_t>(offset + i - window_at_);
				const std::size_t n = std::min(out.size() - i, window_size_ - from);
				std::copy_n(window_.begin() + static_cast<std::ptrdiff_t>(from), n, out.begin() + static_cast<std::ptrdiff_t>(i));
				i += n;
			}
			return true;
		}

		std::uint64_t start() const noexcept { return start_; }
		std::uint64_t limit() const noexcept { return limit_; }
		bool limited() const noexcept { return limited_; }     // something past the limit was asked for
		bool unreadable() const noexcept { return unreadable_; } // a read failed
		std::uint64_t unreadable_at() const noexcept { return unreadable_at_; }

	private:
		static constexpr std::size_t window = std::size_t(256) << 10, sector = 512;

		bool load(std::uint64_t offset) noexcept;

		std::int64_t number(std::uint64_t offset, int width, bool big) noexcept {
			std::int64_t value = 0;
			for (int i = 0; i < width; i++) {
				const int b = at(offset + static_cast<std::uint64_t>(i));
				if (b < 0)
					return -1;
				value |= std::int64_t(b) << (8 * (big ? width - 1 - i : i));
			}
			return value;
		}

		IBlockSource& source_;
		std::uint64_t start_, limit_;
		std::vector<std::byte> window_;
		std::uint64_t window_at_ = 0;
		std::size_t window_size_ = 0;
		bool limited_ = false, unreadable_ = false;
		std::uint64_t unreadable_at_ = 0;
	};

	struct t_verdict {
		bool file = false;             // false: the signature was a false match, or too little of a file is left to use
		std::uint64_t recoverable = 0; // bytes from its first one that are believed to be its own
		std::uint64_t size = 0;        // as far as its structure went: its end, when that was reached
		Integrity integrity = Integrity::partial;
		std::vector<std::string> reasons;
		std::string_view format, extension; // when the content says more than the signature: a zip that's a DOCX
	};

	// why a structure stopped short, for a reason: the source ended, the size limit, or a read failed
	std::string stopped(const t_reader& reader, std::uint64_t at);

	t_verdict validate_jpeg(t_reader& reader);
	t_verdict validate_png(t_reader& reader);
	t_verdict validate_pdf(t_reader& reader);
	t_verdict validate_zip(t_reader& reader);
}
