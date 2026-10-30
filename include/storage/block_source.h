#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "result.h"

namespace storage {
	// bytes at 64-bit offsets from something that holds a disk's or partition's contents: an image file now, a device opened
	// read-only later. there's no way to write through it, every scanner gets one of these and nothing else
	class IBlockSource {
	public:
		IBlockSource() = default;
		IBlockSource(const IBlockSource&) = delete;
		IBlockSource& operator=(const IBlockSource&) = delete;
		virtual ~IBlockSource() = default;

		// fixed for as long as the source is open
		virtual std::uint64_t size_bytes() const noexcept = 0;

		// reads up to output.size() bytes from offset and says how many it read: all of them, fewer only where the source
		// ends, 0 at offset == size_bytes(). an offset past the end is out_of_range
		virtual Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) noexcept = 0;
	};

	// read_at() for exactly output.size() bytes, out_of_range when the source ends before that
	Result<void> read_exact(IBlockSource& source, std::uint64_t offset, std::span<std::byte> output) noexcept;
}
