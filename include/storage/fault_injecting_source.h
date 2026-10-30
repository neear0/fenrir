#pragma once

#include <cstdint>
#include <vector>

#include "block_source.h"

namespace storage {
	// wraps another source and makes it misbehave the way failing disks and flaky usb bridges do, so every reader can be
	// tested against it: reads that come back short, regions that can't be read, and a device that stops answering.
	// it only ever passes reads on, it can't make the inner source change. not thread safe
	class FaultInjectingSource final : public IBlockSource {
	public:
		explicit FaultInjectingSource(IBlockSource& inner) noexcept : inner_(inner) {}

		// no read gives more than bytes (0: no limit), like a device that reads in small chunks
		void set_max_read(std::size_t bytes) noexcept { max_read_ = bytes; }
		// a read that touches any byte of the region fails with io_error, like a bad sector does: the whole request fails,
		// not just the part on it
		void add_bad_region(std::uint64_t offset, std::uint64_t length);
		// the first reads reads work, every one after that fails with io_error: a device that's been unplugged
		void fail_after(std::uint64_t reads) noexcept { fail_after_ = reads; }

		std::uint64_t reads() const noexcept { return reads_; } // read_at() calls so far, failed ones included

		std::uint64_t size_bytes() const noexcept override { return inner_.size_bytes(); }
		Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) noexcept override;

	private:
		struct t_region {
			std::uint64_t first, end; // [first, end)
		};

		IBlockSource& inner_;
		std::size_t max_read_ = 0;
		std::vector<t_region> bad_;
		std::uint64_t fail_after_ = UINT64_MAX;
		std::uint64_t reads_ = 0;
	};
}
