#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace storage {
	// 16 bytes a line: the offset, the bytes in hex, then printable ascii with dots for the rest. base is the offset of
	// bytes[0] in its source, so the left column shows where each line really is:
	// 00000000000001b0  00 00 00 00 00 00 00 00  4e 2a 8f 1c 00 00 00 20  ........N*.....
	std::string hex_dump(std::span<const std::byte> bytes, std::uint64_t base = 0);
}
