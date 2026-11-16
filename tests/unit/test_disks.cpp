#include <doctest/doctest.h>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "storage/disks.h"
#include "storage/exporter.h"
#include "storage/imaging.h"
#include "storage/memory_source.h"
#include "storage/partition.h"

namespace fs = std::filesystem;
using storage::Errc;

namespace {
	struct t_temp_dir {
		fs::path path = fs::temp_directory_path() / ("fenrir_disks_" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
		t_temp_dir() { fs::create_directories(path); }
		~t_temp_dir() {
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	};

	// memory that says it's a drive, to see what refuses to write onto that drive
	class t_pretend_disk final : public storage::IBlockSource {
	public:
		t_pretend_disk(std::vector<std::byte> bytes, std::uint32_t disk) : memory_(std::move(bytes)), disk_(disk) {}
		std::uint64_t size_bytes() const noexcept override { return memory_.size_bytes(); }
		storage::Result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) noexcept override { return memory_.read_at(offset, output); }
		std::optional<std::uint32_t> physical_disk() const noexcept override { return disk_; }

	private:
		storage::MemorySource memory_;
		std::uint32_t disk_;
	};
}

TEST_CASE("drive paths are told apart from everything else") {
#ifdef _WIN32
	CHECK(storage::physical_drive_number("\\\\.\\PhysicalDrive0") == 0u);
	CHECK(storage::physical_drive_number("\\\\.\\physicaldrive12") == 12u);
	CHECK(storage::physical_drive_number("//./PHYSICALDRIVE3") == 3u);
	CHECK_FALSE(storage::physical_drive_number("\\\\.\\PhysicalDrive").has_value());
	CHECK_FALSE(storage::physical_drive_number("\\\\.\\PhysicalDrive1a").has_value());
	CHECK_FALSE(storage::physical_drive_number("\\\\.\\PhysicalDrive12345").has_value());
	CHECK_FALSE(storage::physical_drive_number("\\\\.\\C:").has_value());
#endif
	CHECK_FALSE(storage::physical_drive_number("disk.img").has_value());
	CHECK_FALSE(storage::physical_drive_number("PhysicalDrive0").has_value());
}

#ifdef _WIN32
TEST_CASE("the drives are listed without administrator rights, with the one the temp directory is on") {
	auto disks = storage::list_disks();
	REQUIRE(disks.has_value());
	REQUIRE_FALSE(disks->empty());
	for (const storage::DiskInfo& d : *disks) {
		CAPTURE(d.number);
		CHECK(d.path == fs::path(L"\\\\.\\PhysicalDrive" + std::to_wstring(d.number)));
	}

	auto temp = storage::disks_of(fs::temp_directory_path());
	REQUIRE(temp.has_value());
	REQUIRE_FALSE(temp->empty());
	bool listed = false;
	for (const storage::DiskInfo& d : *disks)
		listed |= d.number == temp->front() && d.size > 0;
	CHECK(listed);
	// a path that isn't there yet is on the drive of the directory it would be in
	auto missing = storage::disks_of(fs::temp_directory_path() / "not" / "there" / "yet.img");
	REQUIRE(missing.has_value());
	CHECK(*missing == *temp);
}

TEST_CASE("nothing writes onto the drive being read") {
	auto temp = storage::disks_of(fs::temp_directory_path());
	REQUIRE(temp.has_value());
	REQUIRE_FALSE(temp->empty());
	const std::uint32_t here = temp->front();
	t_temp_dir dir;

	CHECK(storage::check_destination(dir.path, here).error().code == Errc::invalid_argument);
	CHECK(storage::check_destination(dir.path, 999).has_value());

	t_pretend_disk disk(std::vector<std::byte>(65536, std::byte{ 7 }), here);
	const auto imaged = storage::acquire_image(disk, dir.path / "disk.img", dir.path / "disk.map");
	REQUIRE_FALSE(imaged.has_value());
	CHECK(imaged.error().code == Errc::invalid_argument);
	CHECK_FALSE(fs::exists(dir.path / "disk.img"));
	CHECK_FALSE(fs::exists(dir.path / "disk.map"));

	// a part of that drive is still that drive
	auto partition = storage::PartitionView::open(disk, 4096, 8192);
	REQUIRE(partition.has_value());
	CHECK((*partition)->physical_disk() == here);
	storage::RecoveryCandidate file;
	file.path = "/a.bin";
	file.size = file.recoverable_bytes = 100;
	file.integrity = storage::Integrity::intact;
	file.extents = { { 0, 100 } };
	const auto exported = storage::export_candidate(**partition, file, dir.path / "out");
	REQUIRE_FALSE(exported.has_value());
	CHECK(exported.error().code == Errc::invalid_argument);
	CHECK_FALSE(fs::exists(dir.path / "out"));

	// another drive is fine
	t_pretend_disk elsewhere(std::vector<std::byte>(65536, std::byte{ 7 }), here + 100);
	CHECK(storage::acquire_image(elsewhere, dir.path / "disk.img", dir.path / "disk.map").has_value());
	CHECK(storage::export_candidate(elsewhere, file, dir.path / "out").has_value());
}

TEST_CASE("a drive reads at any offset and length, as whole sectors underneath") {
	auto temp = storage::disks_of(fs::temp_directory_path());
	REQUIRE(temp.has_value());
	auto disk = storage::DiskSource::open(temp->front());
	if (!disk && disk.error().code == Errc::access_denied) {
		MESSAGE("not run: reading a drive needs administrator rights");
		return;
	}
	REQUIRE(disk.has_value());
	storage::DiskSource& d = **disk;
	CHECK(d.physical_disk() == temp->front());
	CHECK(d.size_bytes() > 0);
	CHECK(d.size_bytes() % d.info().sector_size == 0);

	// the first 8 KiB as whole sectors, then pieces of them at odd offsets and lengths
	std::vector<std::byte> whole(8192);
	REQUIRE(storage::read_exact(d, 0, whole).has_value());
	for (const auto [offset, length] : { std::pair<std::size_t, std::size_t>{ 3, 100 }, { 511, 2 }, { 1000, 5000 }, { 4095, 4097 } }) {
		CAPTURE(offset);
		std::vector<std::byte> part(length);
		REQUIRE(storage::read_exact(d, offset, part).has_value());
		CHECK(std::equal(part.begin(), part.end(), whole.begin() + static_cast<std::ptrdiff_t>(offset)));
	}
	// the last bytes, and past the end
	std::vector<std::byte> last(700);
	CHECK(storage::read_exact(d, d.size_bytes() - 700, last).has_value());
	CHECK(d.read_at(d.size_bytes(), last) == 0u);
	CHECK(d.read_at(d.size_bytes() + 1, last).error().code == Errc::out_of_range);

	// opened again as what was listed, and refused as something it isn't
	storage::DiskInfo listed = d.info();
	CHECK(storage::DiskSource::open(temp->front(), &listed).has_value());
	listed.size += 512;
	CHECK(storage::DiskSource::open(temp->front(), &listed).error().code == Errc::mismatch);
}
#endif
