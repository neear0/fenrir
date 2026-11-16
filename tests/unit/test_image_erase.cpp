#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <variant>
#include <vector>

#include "storage/file_image_source.h"
#include "storage/image_erase.h"
#include "storage/sanitize.h"
#include "storage/simulated_drive.h"

namespace fs = std::filesystem;
using storage::Errc;
using storage::SanitizeStatus;

namespace {
	struct t_temp_dir {
		fs::path path = fs::temp_directory_path() / ("fenrir_erase_" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
		t_temp_dir() { fs::create_directories(path); }
		~t_temp_dir() {
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	};

	std::vector<std::byte> data(std::size_t size) {
		std::vector<std::byte> bytes(size);
		storage::sanitation::pattern(bytes, 0, { .zeros = false, .seed = 77 });
		return bytes;
	}

	void write_file(const fs::path& path, const std::vector<std::byte>& bytes) {
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	std::vector<std::byte> read_file(const fs::path& path) {
		std::ifstream in(path, std::ios::binary);
		std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::vector<std::byte> out(chars.size());
		std::ranges::transform(chars, out.begin(), [](char c) { return static_cast<std::byte>(c); });
		return out;
	}

	// inspect, plan, approve: what the cli and the window do before running
	storage::Authorization approve(storage::TestImageBackend& backend, storage::SanitizeRequest request = {}) {
		auto target = backend.inspect();
		REQUIRE(target.has_value());
		const auto result = storage::plan_sanitize(*target, request);
		REQUIRE(std::holds_alternative<storage::SanitizePlan>(result));
		const auto& plan = std::get<storage::SanitizePlan>(result);
		auto approved = storage::authorize(plan, target->identity, plan.confirmation);
		REQUIRE(approved.has_value());
		return *approved;
	}
}

TEST_CASE("a test image is overwritten, flushed and read back") {
	t_temp_dir dir;
	const fs::path image = dir.path / "disposable.img";
	write_file(image, data(300000)); // not whole 512 byte sectors

	storage::TestImageBackend backend(image);
	const auto approved = approve(backend);
	CHECK(approved.plan().confirmation == "ERASE disposable.img");
	CHECK(approved.plan().category == storage::Category::none);
	const auto report = backend.execute(approved);
	REQUIRE(report.has_value());
	CHECK(report->status == SanitizeStatus::verified);
	CHECK(report->bytes_done == 300000);
	CHECK(report->verification.bytes_checked == 300000);
	const auto after = read_file(image);
	REQUIRE(after.size() == 300000);
	CHECK(std::ranges::all_of(after, [](std::byte b) { return b == std::byte{ 0 }; }));
	const std::string json = storage::to_json(*report);
	CHECK(json.find("\"media\": \"test image file\"") != std::string::npos);
	CHECK(json.find("not for sanitizing anything") != std::string::npos);

	SUBCASE("with the pattern, and without reading back") {
		storage::TestImageBackend again(image);
		const auto r = again.execute(approve(again, { .zeros = false, .seed = 5, .verify = false }));
		REQUIRE(r.has_value());
		CHECK(r->status == SanitizeStatus::completed);
		std::vector<std::byte> expected(300000);
		storage::sanitation::pattern(expected, 0, { .zeros = false, .seed = 5 });
		CHECK(read_file(image) == expected);
	}
}

TEST_CASE("a test image that changed since it was approved isn't touched") {
	t_temp_dir dir;
	const fs::path image = dir.path / "disposable.img";
	write_file(image, data(65536));
	storage::TestImageBackend backend(image);
	const auto approved = approve(backend);

	const auto changed = data(65536 + 512);
	write_file(image, changed);
	fs::last_write_time(image, fs::last_write_time(image) + std::chrono::hours(1));
	const auto refused = backend.execute(approved);
	REQUIRE_FALSE(refused.has_value());
	CHECK(refused.error().code == Errc::mismatch);
	CHECK(read_file(image) == changed);
}

TEST_CASE("a test image another program is reading isn't touched") {
	t_temp_dir dir;
	const fs::path image = dir.path / "disposable.img";
	write_file(image, data(65536));
	storage::TestImageBackend backend(image);
	const auto approved = approve(backend);
	{
		auto reader = storage::FileImageSource::open(image); // lets nobody write while it's open
		REQUIRE(reader.has_value());
		const auto refused = backend.execute(approved);
		REQUIRE_FALSE(refused.has_value());
		CHECK(refused.error().code == Errc::busy);
	}
	CHECK(read_file(image) == data(65536));
}

TEST_CASE("an erase of a test image stops when asked, and says how far it got") {
	t_temp_dir dir;
	const fs::path image = dir.path / "disposable.img";
	write_file(image, data(65536));
	storage::TestImageBackend backend(image);
	storage::CancellationToken cancel;
	cancel.cancel();
	const auto report = backend.execute(approve(backend), &cancel);
	REQUIRE(report.has_value());
	CHECK(report->status == SanitizeStatus::interrupted);
	CHECK(report->bytes_done == 0);
	CHECK(read_file(image) == data(65536));
}

TEST_CASE("only regular files can be test images: drives, directories and missing files are refused") {
	t_temp_dir dir;
	CHECK(storage::TestImageBackend("\\\\.\\PhysicalDrive0").inspect().error().code == Errc::unsupported);
	CHECK(storage::TestImageBackend("\\\\.\\C:").inspect().error().code == Errc::unsupported);
	CHECK(storage::TestImageBackend(dir.path).inspect().error().code == Errc::unsupported);
	CHECK(storage::TestImageBackend(dir.path / "missing.img").inspect().error().code == Errc::not_found);

	// a plan for a drive can't be run on a test image backend either
	auto drive = storage::SimulatedDrive::make("hdd");
	auto target = drive->inspect();
	const auto plan = std::get<storage::SanitizePlan>(storage::plan_sanitize(*target, {}));
	auto approved = storage::authorize(plan, target->identity, plan.confirmation);
	REQUIRE(approved.has_value());
	write_file(dir.path / "x.img", data(4096));
	CHECK(storage::TestImageBackend(dir.path / "x.img").execute(*approved).error().code == Errc::unsupported);
}
