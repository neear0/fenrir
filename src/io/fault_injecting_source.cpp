#include "storage/fault_injecting_source.h"

#include <algorithm>

#include "storage/bytes.h"

void storage::FaultInjectingSource::add_bad_region(std::uint64_t offset, std::uint64_t length) {
	if (length == 0)
		return;
	const std::uint64_t end = checked_add(offset, length).value_or(UINT64_MAX);
	bad_.push_back({ offset, end });
}

storage::Result<std::size_t> storage::FaultInjectingSource::read_at(std::uint64_t offset, std::span<std::byte> output) noexcept {
	if (reads_++ >= fail_after_)
		return fail(Errc::io_error, "fault injection: the device stopped answering");

	if (max_read_ != 0 && output.size() > max_read_)
		output = output.first(max_read_);

	const std::uint64_t size = inner_.size_bytes();
	if (offset <= size && !output.empty()) {
		const std::uint64_t end = offset + std::min<std::uint64_t>(output.size(), size - offset); // what the read would cover
		for (const t_region& region : bad_)
			if (offset < region.end && region.first < end)
				return fail(Errc::io_error, "fault injection: an unreadable region");
	}
	return inner_.read_at(offset, output);
}
