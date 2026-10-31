#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "make_fixtures/exfat.h"
#include "storage/exfat.h"
#include "storage/exporter.h"
#include "storage/fault_injecting_source.h"
#include "storage/filesystem.h"
#include "storage/memory_source.h"
#include "storage/partition.h"

namespace fs = std::filesystem;
using storage::Integrity;
using storage::RecoveryCandidate;

namespace {
	struct t_temp_dir {
		fs::path path = fs::temp_directory_path() / ("fenrir_exfat_" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
		t_temp_dir() { fs::create_directories(path); }
		~t_temp_dir() {
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	};

	storage::ExfatScan scan(storage::IBlockSource& source) {
		auto scanned = storage::scan_exfat(source);
		REQUIRE(scanned.has_value());
		return std::move(*scanned);
	}

	std::string reasons(const RecoveryCandidate& c) {
		std::string text;
		for (const std::string& r : c.reasons)
			text += r + "\n";
		return text;
	}

	const RecoveryCandidate& find(const storage::ExfatScan& s, std::string_view path) {
		const auto found = std::ranges::find(s.candidates, path, &RecoveryCandidate::path);
		CAPTURE(path);
		REQUIRE(found != s.candidates.end());
		return *found;
	}

	void check_scenario(const std::vector<RecoveryCandidate>& candidates) {
		const auto expected = fixtures::exfat_scenario().expected;
		REQUIRE(candidates.size() == expected.size());
		for (std::size_t i = 0; i < expected.size(); i++) {
			const RecoveryCandidate& c = candidates[i];
			const fixtures::t_exfat_expected& e = expected[i];
			CAPTURE(e.path);
			CAPTURE(reasons(c));
			CHECK(c.id == i);
			CHECK(c.path == e.path);
			CHECK(c.deleted == e.deleted);
			CHECK(c.directory == e.directory);
			CHECK(storage::to_string(c.integrity) == e.integrity);
			if (!c.directory) {
				CHECK(c.size == e.original.size());
				CHECK(c.recoverable_bytes == e.recoverable);
			}
		}
	}

	std::size_t root_offset() { return fixtures::t_exfat(8 * fixtures::mib).cluster_offset(4); }
}

TEST_CASE("the exfat scenario volume, entry by entry") {
	storage::MemorySource volume(fixtures::exfat_scenario().volume);
	const storage::ExfatScan s = scan(volume);
	const fixtures::t_exfat layout(8 * fixtures::mib);
	CHECK(s.volume.cluster_size == 4096);
	CHECK(s.volume.cluster_count == layout.cluster_count());
	CHECK(s.volume.root_cluster == 4);
	CHECK(s.volume.label == "FENRIR EX");
	CHECK(s.volume.serial == 0x2026E7FAu);
	CHECK(s.volume.revision == 0x0100);
	CHECK_FALSE(s.volume.from_backup_boot_region);
	CHECK(s.diagnostics.empty());
	check_scenario(s.candidates);

	// what each rating rests on
	CHECK(reasons(find(s, "/report.pdf")).find("stored in one piece") != std::string::npos);
	CHECK(reasons(find(s, "/fragmented.bin")).find("FAT chain is still there") != std::string::npos);
	CHECK(find(s, "/fragmented.bin").extents.size() == 2);
	CHECK(reasons(find(s, "/interrupted.bin")).find("cluster 70 is in use") != std::string::npos);
	CHECK(reasons(find(s, "/cleared.bin")).find("was cleared") != std::string::npos);
	CHECK(reasons(find(s, "/first.log")).find("also claimed by deleted entry") != std::string::npos);

	// past the valid data length: zeros, not what's left in the clusters
	const RecoveryCandidate& pre = find(s, "/preallocated.dat");
	REQUIRE(pre.extents.size() == 2);
	CHECK(pre.extents[0].length == 5000);
	CHECK(pre.extents[1].zeros);
	CHECK(pre.extents[1].length == 12288 - 5000);

	const RecoveryCandidate& photo = find(s, "/Photos/IMG_0001.jpg");
	CHECK(photo.parent == find(s, "/Photos").id);
	CHECK(photo.first_cluster == 90);
	CHECK(photo.extents.at(0).offset == layout.cluster_offset(90));
	CHECK(photo.created.valid);
	CHECK(photo.created.year == 2026);
	CHECK(photo.created.month == 10);
	CHECK(photo.created.day == 31);
	CHECK(photo.created.hour == 12);
	CHECK(photo.created.minute == 34);
	CHECK(photo.created.second == 57); // 56, and 150 tens of milliseconds
	CHECK(photo.created.millisecond == 500);
	CHECK(photo.modified.second == 56);
}

TEST_CASE("exported exfat files hash the same as what was written") {
	const auto scenario = fixtures::exfat_scenario();
	storage::MemorySource volume(scenario.volume);
	const storage::ExfatScan s = scan(volume);
	t_temp_dir out;
	for (std::size_t i = 0; i < s.candidates.size(); i++) {
		const RecoveryCandidate& c = s.candidates[i];
		const fixtures::t_exfat_expected& e = scenario.expected[i];
		CAPTURE(e.path);
		auto exported = storage::export_candidate(volume, c, out.path);
		if (c.integrity == Integrity::unrecoverable) {
			REQUIRE_FALSE(exported.has_value());
			continue;
		}
		REQUIRE(exported.has_value());
		if (c.directory)
			continue;
		CHECK(exported->bytes == e.recoverable);
		const auto original = storage::Sha256::of(std::span(e.original).first(static_cast<std::size_t>(e.recoverable)));
		CHECK((exported->sha256 == original) == e.matches_original);
	}
	CHECK(fs::exists(out.path / "fragmented.bin"));
	CHECK(fs::exists(out.path / "interrupted.bin.partial"));
	CHECK(fs::exists(out.path / "Photos" / "IMG_0001.jpg"));
	CHECK(fs::exists(out.path / "old stuff" / "a letter.txt"));
	CHECK(fs::exists(out.path / fs::path(u8"Ein ziemlich langer Dateiname – ᚠ.txt")));
}

TEST_CASE("the exfat disk: found through its partition table and scan_volume") {
	storage::MemorySource disk(fixtures::exfat_disk());
	auto map = storage::scan_partitions(disk);
	REQUIRE(map.has_value());
	REQUIRE(map->partitions.size() == 1);
	CHECK(map->partitions[0].filesystem == storage::FileSystem::exfat);
	CHECK(storage::can_scan(storage::FileSystem::exfat));
	auto view = storage::open_partition(disk, map->partitions[0]);
	REQUIRE(view.has_value());
	auto scanned = storage::scan_volume(**view, storage::FileSystem::exfat);
	REQUIRE(scanned.has_value());
	CHECK(scanned->filesystem == storage::FileSystem::exfat);
	CHECK(scanned->summary.starts_with("exFAT, 4096 byte clusters"));
	CHECK(scanned->summary.ends_with("label FENRIR EX"));
	check_scenario(scanned->candidates);
}

TEST_CASE("a deleted entry set partly written over is left out and counted") {
	auto image = fixtures::exfat_scenario().volume;
	storage::MemorySource clean(image);
	const std::uint64_t at = find(scan(clean), "/report.pdf").entry_offset;
	image[static_cast<std::size_t>(at + 64 + 2)] = std::byte{ 'X' }; // a letter of its name, so the checksum no longer holds
	storage::MemorySource volume(image);
	const storage::ExfatScan s = scan(volume);
	CHECK(std::ranges::find(s.candidates, "/report.pdf", &RecoveryCandidate::path) == s.candidates.end());
	CHECK(s.candidates.size() == fixtures::exfat_scenario().expected.size() - 1);
	REQUIRE(s.diagnostics.size() == 1);
	CHECK(s.diagnostics[0].message.find("1 deleted entry set no longer matched its checksum") != std::string::npos);
}

TEST_CASE("a live entry set that fails its checksum is uncertain") {
	auto image = fixtures::exfat_scenario().volume;
	storage::MemorySource clean(image);
	const std::uint64_t at = find(scan(clean), "/readme.txt").entry_offset;
	image[static_cast<std::size_t>(at + 64 + 2)] = std::byte{ 'R' };
	storage::MemorySource volume(image);
	const storage::ExfatScan s = scan(volume);
	const RecoveryCandidate& c = find(s, "/Readme.txt");
	CHECK(c.integrity == Integrity::uncertain);
	CHECK(reasons(c).find("checksum") != std::string::npos);
}

TEST_CASE("a damaged exfat boot region falls back to the backup") {
	auto image = fixtures::exfat_scenario().volume;
	image[92] ^= std::byte{ 0x01 }; // the cluster count, so the checksum fails
	storage::MemorySource volume(image);
	const storage::ExfatScan s = scan(volume);
	CHECK(s.volume.from_backup_boot_region);
	CHECK(storage::has(s.diagnostics, storage::Diagnostic::Severity::warning));
	check_scenario(s.candidates);
}

TEST_CASE("what isn't exfat is refused") {
	storage::MemorySource blank(fixtures::t_image(1 * fixtures::mib));
	auto none = storage::scan_exfat(blank);
	REQUIRE_FALSE(none.has_value());
	CHECK(none.error().code == storage::Errc::unsupported);
	storage::MemorySource fat32(fixtures::fat32_scenario().volume);
	CHECK_FALSE(storage::scan_exfat(fat32).has_value());
	storage::MemorySource tiny(fixtures::t_image(100));
	CHECK_FALSE(storage::scan_exfat(tiny).has_value());
	CHECK_FALSE(storage::scan_volume(blank, storage::FileSystem::ext).has_value());
}

TEST_CASE("an unreadable exfat directory cluster is noted and the rest is scanned") {
	storage::MemorySource memory(fixtures::exfat_scenario().volume);
	storage::FaultInjectingSource bad(memory);
	bad.add_bad_region(fixtures::t_exfat(8 * fixtures::mib).cluster_offset(5), 512); // Photos
	const storage::ExfatScan s = scan(bad);
	CHECK(storage::has(s.diagnostics, storage::Diagnostic::Severity::error));
	CHECK(s.candidates.size() == fixtures::exfat_scenario().expected.size() - 1);
}

TEST_CASE("a cancelled exfat scan stops") {
	storage::MemorySource volume(fixtures::exfat_scenario().volume);
	storage::CancellationToken cancel;
	cancel.cancel();
	auto scanned = storage::scan_exfat(volume, &cancel);
	REQUIRE_FALSE(scanned.has_value());
	CHECK(scanned.error().code == storage::Errc::cancelled);
}

TEST_CASE("random damage to exfat's boot region, fat and directories never crashes the scanner") {
	const auto original = fixtures::exfat_scenario().volume;
	const std::size_t root = root_offset();
	std::mt19937_64 random(0xE7FA7);
	for (int round = 0; round < 300; round++) {
		auto image = original;
		const int flips = 1 + static_cast<int>(random() % 32);
		for (int i = 0; i < flips; i++) {
			const std::size_t region = random() % 4;
			const std::size_t at = region == 0 ? random() % 512                                  // the boot sector
				: region == 1 ? 128 * 512 + random() % (16 * 512)                                  // the fat
				: region == 2 ? fixtures::t_exfat(8 * fixtures::mib).cluster_offset(2) + random() % 64 // the bitmap
				: root + random() % (3 * 4096);                                                    // the root and the directories after it
			image[at] = static_cast<std::byte>(random());
		}
		storage::MemorySource volume(image);
		auto scanned = storage::scan_exfat(volume);
		if (!scanned)
			continue;
		for (const RecoveryCandidate& c : scanned->candidates) {
			std::uint64_t total = 0;
			for (const storage::Extent& e : c.extents) {
				REQUIRE((e.zeros || e.offset + e.length <= image.size()));
				total += e.length;
			}
			REQUIRE(total == c.recoverable_bytes);
			REQUIRE(c.recoverable_bytes <= c.size);
		}
	}
}
