#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <vector>

#include "storage/bytes.h"
#include "storage/fault_injecting_source.h"
#include "storage/hex_dump.h"
#include "storage/memory_source.h"
#include "storage/partition.h"

using storage::Errc;

namespace {
	std::vector<std::byte> counting(std::size_t size) { // byte i is i & 0xFF
		std::vector<std::byte> bytes(size);
		for (std::size_t i = 0; i < size; i++)
			bytes[i] = static_cast<std::byte>(i & 0xFF);
		return bytes;
	}
	constexpr std::uint64_t max64 = std::numeric_limits<std::uint64_t>::max();
}

TEST_CASE("the byte reader decodes little-endian and refuses to read past the end") {
	const auto bytes = counting(16);
	storage::ByteReader r(bytes);
	CHECK(r.u8(0) == 0x00);
	CHECK(r.le16(1) == 0x0201);
	CHECK(r.le32(4) == 0x07060504u);
	CHECK(r.le64(8) == 0x0F0E0D0C0B0A0908ull);
	CHECK(r.ok());
	CHECK(r.matches(0, std::string_view("\x00\x01\x02", 3)));
	CHECK_FALSE(r.matches(15, "too long"));
	CHECK(r.ok()); // matches() doesn't count as a bad read

	CHECK(r.le32(13) == 0); // 13..16, one byte past
	CHECK_FALSE(r.ok());

	storage::ByteReader huge(bytes);
	CHECK(huge.le64(max64 / 2) == 0);
	CHECK(huge.bytes(4, std::numeric_limits<std::size_t>::max()).empty());
	CHECK_FALSE(huge.ok());
}

TEST_CASE("checked arithmetic notices overflow") {
	CHECK(storage::checked_add(1, 2) == 3u);
	CHECK(storage::checked_add(max64, 0) == max64);
	CHECK_FALSE(storage::checked_add(max64, 1).has_value());
	CHECK(storage::checked_mul(0, max64) == 0u);
	CHECK(storage::checked_mul(1u << 31, 1u << 31) == (std::uint64_t(1) << 62));
	CHECK_FALSE(storage::checked_mul(std::uint64_t(1) << 32, std::uint64_t(1) << 32).has_value());
}

TEST_CASE("the hex dump shows offsets, bytes and ascii") {
	std::vector<std::byte> bytes = counting(20);
	bytes[0] = std::byte{ 'F' };
	bytes[1] = std::byte{ 'e' };
	const std::string dump = storage::hex_dump(bytes, 0x1B0);
	CHECK(dump ==
		"00000000000001b0  46 65 02 03 04 05 06 07  08 09 0a 0b 0c 0d 0e 0f  Fe..............\n"
		"00000000000001c0  10 11 12 13                                       ....\n");
	CHECK(storage::hex_dump({}).empty());
}

TEST_CASE("short reads from the fault injector still add up in read_exact") {
	const auto bytes = counting(10000);
	storage::MemorySource memory(bytes);
	storage::FaultInjectingSource flaky(memory);
	flaky.set_max_read(7);

	std::array<std::byte, 100> buffer{};
	auto one = flaky.read_at(50, buffer);
	REQUIRE(one.has_value());
	CHECK(*one == 7);

	REQUIRE(storage::read_exact(flaky, 9000, buffer).has_value());
	for (std::size_t i = 0; i < buffer.size(); i++)
		REQUIRE(buffer[i] == bytes[9000 + i]);
	CHECK(flaky.reads() == 1 + 15); // ceil(100 / 7)
}

TEST_CASE("a bad region fails every read that touches it, and only those") {
	storage::MemorySource memory(counting(4096));
	storage::FaultInjectingSource bad(memory);
	bad.add_bad_region(1024, 512); // bytes 1024..1535

	std::array<std::byte, 512> buffer{};
	CHECK(bad.read_at(512, buffer).has_value());         // ends at 1023, just before
	CHECK(bad.read_at(1536, buffer).has_value());        // starts just after
	CHECK_FALSE(bad.read_at(513, buffer).has_value());   // its last byte is 1024
	CHECK_FALSE(bad.read_at(1535, std::span(buffer).first(1)).has_value());
	CHECK(bad.read_at(1024, std::span<std::byte>{}).has_value()); // nothing read, nothing touched
	auto failed = bad.read_at(1200, buffer);
	REQUIRE_FALSE(failed.has_value());
	CHECK(failed.error().code == Errc::io_error);
}

TEST_CASE("an unplugged device fails every read after the first few") {
	storage::MemorySource memory(counting(4096));
	storage::FaultInjectingSource unplugged(memory);
	unplugged.fail_after(2);
	std::array<std::byte, 16> buffer{};
	CHECK(unplugged.read_at(0, buffer).has_value());
	CHECK(unplugged.read_at(16, buffer).has_value());
	CHECK_FALSE(unplugged.read_at(32, buffer).has_value());
	CHECK_FALSE(unplugged.read_at(0, buffer).has_value());
}

TEST_CASE("a partition view reads only its own range") {
	const auto bytes = counting(4096);
	storage::MemorySource disk(bytes);
	auto opened = storage::PartitionView::open(disk, 1000, 2000);
	REQUIRE(opened.has_value());
	storage::PartitionView& view = **opened;
	CHECK(view.size_bytes() == 2000);
	CHECK(view.offset_in_parent() == 1000);

	std::array<std::byte, 64> buffer{};
	auto start = view.read_at(0, buffer);
	REQUIRE(start.has_value());
	CHECK(buffer[0] == bytes[1000]);

	auto end = view.read_at(2000 - 10, buffer); // cut short at the partition's end, not the disk's
	REQUIRE(end.has_value());
	CHECK(*end == 10);
	CHECK(buffer[9] == bytes[2999]);

	CHECK(view.read_at(2000, buffer).value() == 0);
	auto past = view.read_at(2001, buffer);
	REQUIRE_FALSE(past.has_value());
	CHECK(past.error().code == Errc::out_of_range);
}

TEST_CASE("a partition view can't reach past the disk") {
	storage::MemorySource disk(counting(4096));
	CHECK(storage::PartitionView::open(disk, 0, 4096).has_value());
	CHECK(storage::PartitionView::open(disk, 4096, 0).has_value());
	auto past = storage::PartitionView::open(disk, 4000, 97);
	REQUIRE_FALSE(past.has_value());
	CHECK(past.error().code == Errc::out_of_range);
	auto wrapping = storage::PartitionView::open(disk, max64 - 10, 20);
	REQUIRE_FALSE(wrapping.has_value());
	CHECK(wrapping.error().code == Errc::overflow);
}
