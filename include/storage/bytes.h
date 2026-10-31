#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

// decoding on-disk structures, which are hostile input: every field read is bounds checked and every offset sum checked
namespace storage {
	// little-endian fields out of a buffer. a read past the end gives 0 and clears ok(), so a parser can read a whole
	// structure and check once at the end instead of after every field
	class ByteReader {
	public:
		explicit ByteReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

		bool ok() const noexcept { return ok_; }
		std::size_t size() const noexcept { return bytes_.size(); }

		std::uint8_t u8(std::size_t offset) noexcept { return static_cast<std::uint8_t>(le(offset, 1)); }
		std::uint16_t le16(std::size_t offset) noexcept { return static_cast<std::uint16_t>(le(offset, 2)); }
		std::uint32_t le32(std::size_t offset) noexcept { return static_cast<std::uint32_t>(le(offset, 4)); }
		std::uint64_t le64(std::size_t offset) noexcept { return le(offset, 8); }

		// length bytes at offset, empty (and ok() cleared) when they don't all fit
		std::span<const std::byte> bytes(std::size_t offset, std::size_t length) noexcept {
			if (!fits(offset, length))
				return {};
			return bytes_.subspan(offset, length);
		}

		// true when the bytes at offset are exactly text, false (without clearing ok()) when they differ or don't fit
		bool matches(std::size_t offset, std::string_view text) const noexcept {
			if (offset > bytes_.size() || text.size() > bytes_.size() - offset)
				return false;
			for (std::size_t i = 0; i < text.size(); i++)
				if (bytes_[offset + i] != static_cast<std::byte>(text[i]))
					return false;
			return true;
		}

	private:
		bool fits(std::size_t offset, std::size_t length) noexcept {
			if (offset > bytes_.size() || length > bytes_.size() - offset) {
				ok_ = false;
				return false;
			}
			return true;
		}

		std::uint64_t le(std::size_t offset, std::size_t width) noexcept {
			if (!fits(offset, width))
				return 0;
			std::uint64_t value = 0;
			for (std::size_t i = 0; i < width; i++)
				value |= std::uint64_t(bytes_[offset + i]) << (8 * i);
			return value;
		}

		std::span<const std::byte> bytes_;
		bool ok_ = true;
	};

	// a + b and a * b, or nothing when the result doesn't fit in 64 bits
	constexpr std::optional<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) noexcept {
		if (a > (std::numeric_limits<std::uint64_t>::max)() - b)
			return std::nullopt;
		return a + b;
	}

	constexpr std::optional<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) noexcept {
		if (a != 0 && b > (std::numeric_limits<std::uint64_t>::max)() / a)
			return std::nullopt;
		return a * b;
	}
}
