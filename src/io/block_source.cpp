#include "storage/block_source.h"

storage::Result<void> storage::read_exact(IBlockSource& source, std::uint64_t offset, std::span<std::byte> output) noexcept {
	// checked before reading, so a request that can't be met reads nothing
	const std::uint64_t size = source.size_bytes();
	if (offset > size || output.size() > size - offset)
		return fail(Errc::out_of_range, "read_exact: past the end of the source");

	std::size_t done = 0;
	while (done < output.size()) {
		Result<std::size_t> read = source.read_at(offset + done, output.subspan(done));
		if (!read)
			return std::unexpected(read.error());
		if (read.value() == 0) // the source said it was big enough, then ended
			return fail(Errc::io_error, "read_exact: the source ended early");
		done += read.value();
	}
	return {};
}
