#pragma once

#include <algorithm>
#include <vector>

#include "block_source.h"

namespace storage {
	// bytes already in memory as a source: test images built in code, and sectors that have been read already
	class MemorySource final : public IBlockSource {
	public:
		explicit MemorySource(std::vector<std::byte> bytes) noexcept : bytes_(std::move(bytes)) {}

		std::uint64_t size_bytes() const noexcept override { return bytes_.size(); }

		Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) noexcept override {
			if (offset > bytes_.size())
				return fail(Errc::out_of_range, "reading memory: the offset is past its end");
			const std::size_t at = static_cast<std::size_t>(offset);
			const std::size_t count = std::min(output.size(), bytes_.size() - at);
			std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(at), count, output.begin());
			return count;
		}

	private:
		std::vector<std::byte> bytes_;
	};
}
