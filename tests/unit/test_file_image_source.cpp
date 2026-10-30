#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "storage/file_image_source.h"

namespace fs = std::filesystem;
using storage::Errc;
using storage::FileImageSource;

namespace {
	// a directory of its own under the temp directory, removed with what's in it at the end of the test
	struct t_scratch {
		fs::path dir;
		t_scratch() {
			std::random_device random;
			dir = fs::temp_directory_path() / ("fenrir_tests_" + std::to_string(random()) + std::to_string(random()));
			fs::create_directories(dir);
		}
		~t_scratch() {
			std::error_code ignored;
			fs::remove_all(dir, ignored);
		}
		t_scratch(const t_scratch&) = delete;
		t_scratch& operator=(const t_scratch&) = delete;
	};

	std::vector<std::byte> pattern(std::size_t size) { // every byte its offset's low bits, easy to check at any offset
		std::vector<std::byte> bytes(size);
		for (std::size_t i = 0; i < size; i++)
			bytes[i] = static_cast<std::byte>((i * 131 + 7) & 0xFF);
		return bytes;
	}

	fs::path write_fixture(const fs::path& dir, const std::string& name, const std::vector<std::byte>& bytes) {
		const fs::path path = dir / name;
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		REQUIRE(out.good());
		return path;
	}

	std::vector<std::byte> read_back(const fs::path& path) {
		std::ifstream in(path, std::ios::binary);
		std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::vector<std::byte> bytes(chars.size());
		for (std::size_t i = 0; i < chars.size(); i++)
			bytes[i] = static_cast<std::byte>(chars[i]);
		return bytes;
	}
}

TEST_CASE("an image opens read-only with its size") {
	t_scratch scratch;
	const auto bytes = pattern(4096 + 17);
	const fs::path path = write_fixture(scratch.dir, "image.img", bytes);

	auto opened = FileImageSource::open(path);
	REQUIRE(opened.has_value());
	CHECK(opened.value()->size_bytes() == bytes.size());
}

TEST_CASE("reads at the start, the middle and up to the end") {
	t_scratch scratch;
	const auto bytes = pattern(10000);
	auto opened = FileImageSource::open(write_fixture(scratch.dir, "image.img", bytes));
	REQUIRE(opened.has_value());
	FileImageSource& image = *opened.value();

	std::array<std::byte, 512> buffer{};
	for (std::uint64_t offset : { std::uint64_t(0), std::uint64_t(1), std::uint64_t(511), std::uint64_t(4096), std::uint64_t(10000 - 512) }) {
		CAPTURE(offset);
		auto read = image.read_at(offset, buffer);
		REQUIRE(read.has_value());
		REQUIRE(read.value() == buffer.size());
		for (std::size_t i = 0; i < buffer.size(); i++)
			REQUIRE(buffer[i] == bytes[offset + i]);
	}
}

TEST_CASE("a read running past the end is cut short there") {
	t_scratch scratch;
	const auto bytes = pattern(1000);
	auto opened = FileImageSource::open(write_fixture(scratch.dir, "image.img", bytes));
	REQUIRE(opened.has_value());
	FileImageSource& image = *opened.value();

	std::array<std::byte, 64> buffer{};
	buffer.fill(std::byte{ 0xEE });
	auto read = image.read_at(1000 - 3, buffer);
	REQUIRE(read.has_value());
	CHECK(read.value() == 3);
	CHECK(buffer[0] == bytes[997]);
	CHECK(buffer[2] == bytes[999]);
	CHECK(buffer[3] == std::byte{ 0xEE }); // the rest is left as it was
}

TEST_CASE("at the end a read gives 0 bytes, past it out_of_range") {
	t_scratch scratch;
	auto opened = FileImageSource::open(write_fixture(scratch.dir, "image.img", pattern(1000)));
	REQUIRE(opened.has_value());
	FileImageSource& image = *opened.value();

	std::array<std::byte, 16> buffer{};
	auto at_end = image.read_at(1000, buffer);
	REQUIRE(at_end.has_value());
	CHECK(at_end.value() == 0);

	for (std::uint64_t offset : { std::uint64_t(1001), std::uint64_t(1) << 40, std::numeric_limits<std::uint64_t>::max() }) {
		CAPTURE(offset);
		auto past = image.read_at(offset, buffer);
		REQUIRE_FALSE(past.has_value());
		CHECK(past.error().code == Errc::out_of_range);
	}
}

TEST_CASE("an empty buffer reads nothing anywhere in range") {
	t_scratch scratch;
	auto opened = FileImageSource::open(write_fixture(scratch.dir, "image.img", pattern(100)));
	REQUIRE(opened.has_value());
	auto read = opened.value()->read_at(50, {});
	REQUIRE(read.has_value());
	CHECK(read.value() == 0);
}

TEST_CASE("an empty image has size 0 and nothing to read") {
	t_scratch scratch;
	auto opened = FileImageSource::open(write_fixture(scratch.dir, "empty.img", {}));
	REQUIRE(opened.has_value());
	FileImageSource& image = *opened.value();
	CHECK(image.size_bytes() == 0);

	std::array<std::byte, 8> buffer{};
	auto read = image.read_at(0, buffer);
	REQUIRE(read.has_value());
	CHECK(read.value() == 0);
	CHECK_FALSE(image.read_at(1, buffer).has_value());
}

TEST_CASE("read_exact needs all of it or reads none") {
	t_scratch scratch;
	const auto bytes = pattern(1024);
	auto opened = FileImageSource::open(write_fixture(scratch.dir, "image.img", bytes));
	REQUIRE(opened.has_value());
	FileImageSource& image = *opened.value();

	std::array<std::byte, 512> buffer{};
	CHECK(storage::read_exact(image, 512, buffer).has_value());
	CHECK(buffer[0] == bytes[512]);

	buffer.fill(std::byte{ 0xEE });
	auto short_read = storage::read_exact(image, 513, buffer);
	REQUIRE_FALSE(short_read.has_value());
	CHECK(short_read.error().code == Errc::out_of_range);
	CHECK(buffer[0] == std::byte{ 0xEE }); // refused before reading

	auto overflowing = storage::read_exact(image, std::numeric_limits<std::uint64_t>::max(), buffer);
	REQUIRE_FALSE(overflowing.has_value());
	CHECK(overflowing.error().code == Errc::out_of_range);
}

TEST_CASE("missing files, directories and empty paths are refused") {
	t_scratch scratch;
	auto missing = FileImageSource::open(scratch.dir / "nope.img");
	REQUIRE_FALSE(missing.has_value());
	CHECK(missing.error().code == Errc::not_found);

	auto directory = FileImageSource::open(scratch.dir);
	REQUIRE_FALSE(directory.has_value());
	CHECK(directory.error().code == Errc::unsupported);

	auto empty = FileImageSource::open(fs::path());
	REQUIRE_FALSE(empty.has_value());
	CHECK(empty.error().code == Errc::invalid_argument);
}

TEST_CASE("scanning leaves the image exactly as it was") {
	t_scratch scratch;
	const auto bytes = pattern(64 * 1024);
	const fs::path path = write_fixture(scratch.dir, "image.img", bytes);
	const auto written_at = fs::last_write_time(path);
	{
		auto opened = FileImageSource::open(path);
		REQUIRE(opened.has_value());
		std::array<std::byte, 4096> buffer{};
		for (std::uint64_t offset = 0; offset < bytes.size(); offset += 1000)
			REQUIRE(opened.value()->read_at(offset, buffer).has_value());
	}
	CHECK(fs::last_write_time(path) == written_at);
	CHECK(read_back(path) == bytes);
}

#ifdef _WIN32
TEST_CASE("windows: device paths are refused before anything is opened") {
	for (const wchar_t* path : { L"\\\\.\\PhysicalDrive0", L"//./PhysicalDrive0", L"\\\\.\\C:", L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\", L"\\\\?\\GLOBALROOT\\Device\\Harddisk0\\Partition0", L"\\??\\PhysicalDrive0" }) {
		auto opened = FileImageSource::open(path);
		REQUIRE_FALSE(opened.has_value());
		CHECK(opened.error().code == Errc::unsupported);
	}
}

TEST_CASE("windows: long \\\\?\\ paths to files still open") {
	t_scratch scratch;
	const fs::path path = write_fixture(scratch.dir, "image.img", pattern(10));
	auto opened = FileImageSource::open(L"\\\\?\\" + fs::absolute(path).wstring());
	REQUIRE(opened.has_value());
	CHECK(opened.value()->size_bytes() == 10);
}

TEST_CASE("windows: nobody can open the image for writing while it's open") {
	t_scratch scratch;
	const fs::path path = write_fixture(scratch.dir, "image.img", pattern(100));
	auto opened = FileImageSource::open(path);
	REQUIRE(opened.has_value());
	std::ofstream writer(path, std::ios::binary | std::ios::app);
	CHECK_FALSE(writer.is_open());
}

TEST_CASE("windows: an image another program has open for writing is busy") {
	t_scratch scratch;
	const fs::path path = write_fixture(scratch.dir, "image.img", pattern(100));
	std::ofstream writer(path, std::ios::binary | std::ios::app);
	REQUIRE(writer.is_open());
	auto opened = FileImageSource::open(path);
	REQUIRE_FALSE(opened.has_value());
	CHECK(opened.error().code == Errc::busy);
}
#endif
