#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "make_fixtures/fat32.h"
#include "storage/exporter.h"
#include "storage/fat32.h"
#include "storage/fault_injecting_source.h"
#include "storage/file_image_source.h"
#include "storage/memory_source.h"
#include "storage/partition.h"

namespace fs = std::filesystem;
using storage::Integrity;
using storage::RecoveryCandidate;

namespace {
	struct t_temp_dir {
		fs::path path = fs::temp_directory_path() / ("fenrir_fat32_" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
		t_temp_dir() { fs::create_directories(path); }
		~t_temp_dir() {
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	};

	storage::Fat32Scan scan(storage::IBlockSource& source) {
		auto scanned = storage::scan_fat32(source);
		REQUIRE(scanned.has_value());
		return std::move(*scanned);
	}

	std::string reasons(const RecoveryCandidate& c) {
		std::string text;
		for (const std::string& r : c.reasons)
			text += r + "\n";
		return text;
	}

	const RecoveryCandidate& find(const storage::Fat32Scan& s, std::string_view path) {
		const auto found = std::ranges::find(s.candidates, path, &RecoveryCandidate::path);
		CAPTURE(path);
		REQUIRE(found != s.candidates.end());
		return *found;
	}

	// what the scan found against what fat32_scenario() put there, one by one and in order
	void check_scenario(const storage::Fat32Scan& s) {
		const auto expected = fixtures::fat32_scenario().expected;
		REQUIRE(s.candidates.size() == expected.size());
		for (std::size_t i = 0; i < expected.size(); i++) {
			const RecoveryCandidate& c = s.candidates[i];
			const fixtures::t_fat32_expected& e = expected[i];
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
}

TEST_CASE("the fat32 scenario volume, entry by entry") {
	storage::MemorySource volume(fixtures::fat32_scenario().volume);
	const storage::Fat32Scan s = scan(volume);
	CHECK(s.volume.cluster_size == 512);
	CHECK(s.volume.root_cluster == 2);
	CHECK(s.volume.cluster_count == fixtures::t_fat32(8 * fixtures::mib).cluster_count());
	CHECK(s.volume.label == "FENRIR TEST");
	CHECK(s.volume.volume_id == 0x2026F3E4u);
	CHECK_FALSE(s.volume.from_backup_boot_sector);
	CHECK_FALSE(storage::has(s.diagnostics, storage::Diagnostic::Severity::warning));
	check_scenario(s);

	// details the table above doesn't cover
	const RecoveryCandidate& notes = find(s, "/_otes.txt");
	CHECK(notes.name_damaged);
	CHECK(notes.short_name == "_otes.txt");
	const RecoveryCandidate& photo = find(s, "/DCIM/Holiday photo 2026.jpg");
	CHECK_FALSE(photo.name_damaged);
	CHECK(photo.short_name == "HOLIDA~1.JPG"); // its first letter recovered from the long name's checksum
	CHECK(photo.parent == find(s, "/DCIM").id);
	CHECK(photo.modified.valid);
	CHECK(photo.modified.year == 2026);
	CHECK(photo.modified.month == 10);
	CHECK(photo.modified.day == 30);
	CHECK(photo.modified.hour == 12);
	CHECK(photo.modified.second == 56);
	REQUIRE(photo.extents.size() == 1);
	CHECK(photo.extents[0].offset == fixtures::t_fat32(8 * fixtures::mib).cluster_offset(50));
	CHECK(find(s, "/keep.txt").extents.size() == 1);
	const RecoveryCandidate& letter = find(s, "/old stuff/a letter.txt");
	CHECK(letter.parent == find(s, "/old stuff").id);
	CHECK(find(s, "/_irst.log").reasons.back().find("also claimed by deleted entry") != std::string::npos);
}

TEST_CASE("exported files hash the same as what was written to the volume") {
	const auto scenario = fixtures::fat32_scenario();
	storage::MemorySource volume(scenario.volume);
	const storage::Fat32Scan s = scan(volume);
	t_temp_dir out;
	for (std::size_t i = 0; i < s.candidates.size(); i++) {
		const RecoveryCandidate& c = s.candidates[i];
		const fixtures::t_fat32_expected& e = scenario.expected[i];
		CAPTURE(e.path);
		auto exported = storage::export_candidate(volume, c, out.path);
		if (c.integrity == Integrity::unrecoverable) {
			REQUIRE_FALSE(exported.has_value());
			CHECK(exported.error().code == storage::Errc::unsupported);
			continue;
		}
		REQUIRE(exported.has_value());
		CHECK(fs::exists(exported->path));
		if (c.directory) {
			CHECK(fs::is_directory(exported->path));
			continue;
		}
		CHECK(exported->bytes == e.recoverable);
		CHECK(fs::file_size(exported->path) == e.recoverable);
		CHECK(exported->complete == (e.recoverable == e.original.size()));
		const auto original = storage::Sha256::of(std::span(e.original).first(static_cast<std::size_t>(e.recoverable)));
		CHECK((exported->sha256 == original) == e.matches_original);
	}
	CHECK(fs::exists(out.path / "fragmented.dat.partial"));
	CHECK(fs::exists(out.path / "DCIM" / "Holiday photo 2026.jpg"));
	CHECK(fs::exists(out.path / fs::path(u8"Fenrir – ᚠ.txt")));
	CHECK(fs::exists(out.path / "old stuff" / "a letter.txt"));
}

TEST_CASE("the fat32 disk: found through its partition table, the same as the volume alone") {
	storage::MemorySource disk(fixtures::fat32_disk());
	auto map = storage::scan_partitions(disk);
	REQUIRE(map.has_value());
	REQUIRE(map->partitions.size() == 1);
	CHECK(map->partitions[0].filesystem == storage::FileSystem::fat32);
	auto view = storage::open_partition(disk, map->partitions[0]);
	REQUIRE(view.has_value());
	check_scenario(scan(**view));
}

TEST_CASE("scanning and exporting from an image file leaves it exactly as it was") {
	t_temp_dir dir;
	const fs::path image = dir.path / "fat32.img";
	{
		const auto disk = fixtures::fat32_disk();
		std::ofstream out(image, std::ios::binary);
		out.write(reinterpret_cast<const char*>(disk.data()), static_cast<std::streamsize>(disk.size()));
	}
	const auto written = fs::last_write_time(image);
	auto hash = [&] {
		auto opened = storage::FileImageSource::open(image);
		REQUIRE(opened.has_value());
		return storage::sha256_source(**opened).value();
	};
	const auto before = hash();
	{
		auto opened = storage::FileImageSource::open(image);
		REQUIRE(opened.has_value());
		auto map = storage::scan_partitions(**opened);
		REQUIRE(map.has_value());
		auto view = storage::open_partition(**opened, map->partitions.at(0));
		REQUIRE(view.has_value());
		const storage::Fat32Scan s = scan(**view);
		for (const RecoveryCandidate& c : s.candidates)
			if (c.integrity != Integrity::unrecoverable)
				REQUIRE(storage::export_candidate(**view, c, dir.path / "out").has_value());
	}
	CHECK(hash() == before);
	CHECK(fs::last_write_time(image) == written);
}

TEST_CASE("an export never replaces a file, it picks another name") {
	storage::MemorySource volume(fixtures::fat32_scenario().volume);
	const storage::Fat32Scan s = scan(volume);
	const RecoveryCandidate& rune = find(s, "/Fenrir \xE2\x80\x93 \xE1\x9A\xA0.txt");
	t_temp_dir out;
	{
		std::ofstream taken(out.path / fs::path(u8"Fenrir – ᚠ.txt"));
		taken << "already here";
	}
	auto second = storage::export_candidate(volume, rune, out.path);
	REQUIRE(second.has_value());
	CHECK(second->path.filename() == fs::path(u8"Fenrir – ᚠ (2).txt"));
	auto third = storage::export_candidate(volume, rune, out.path);
	REQUIRE(third.has_value());
	CHECK(third->path.filename() == fs::path(u8"Fenrir – ᚠ (3).txt"));
	std::ifstream untouched(out.path / fs::path(u8"Fenrir – ᚠ.txt"));
	std::string text;
	std::getline(untouched, text);
	CHECK(text == "already here");
}

TEST_CASE("names from the disk can't escape the destination or name a device") {
	CHECK(storage::safe_name("notes.txt") == "notes.txt");
	CHECK(storage::safe_name("..") == "_");
	CHECK(storage::safe_name(".") == "_");
	CHECK(storage::safe_name("a/b\\c") == "a_b_c");
	CHECK(storage::safe_name("C:evil") == "C_evil");
	CHECK(storage::safe_name("file.txt:stream") == "file.txt_stream");
	CHECK(storage::safe_name("what?*<>|\"") == "what______");
	CHECK(storage::safe_name("trailing. . ") == "trailing");
	CHECK(storage::safe_name("con") == "_con");
	CHECK(storage::safe_name("NUL.txt") == "_NUL.txt");
	CHECK(storage::safe_name("com1.log") == "_com1.log");
	CHECK(storage::safe_name("lpt\xC2\xB9") == "_lpt\xC2\xB9");
	CHECK(storage::safe_name("console") == "console");
	CHECK(storage::safe_name("tab\there") == "tab_here");
	CHECK(storage::safe_name("") == "_");
	const std::string long_name = std::string(300, 'x') + ".jpeg";
	CHECK(storage::safe_name(long_name) == std::string(195, 'x') + ".jpeg");
	const std::string long_utf8 = std::string(199, 'x') + "\xE1\x9A\xA0"; // a 3 byte character across the limit
	CHECK(storage::safe_name(long_utf8) == std::string(199, 'x'));

	// a candidate whose path tries to climb out lands inside the destination anyway
	storage::MemorySource volume(fixtures::fat32_scenario().volume);
	const storage::Fat32Scan s = scan(volume);
	RecoveryCandidate evil = find(s, "/keep.txt");
	evil.path = "/../../escaped.txt";
	t_temp_dir out;
	auto exported = storage::export_candidate(volume, evil, out.path / "inside");
	REQUIRE(exported.has_value());
	CHECK(exported->path == out.path / "inside" / "_" / "_" / "escaped.txt");
}

TEST_CASE("an export refuses device destinations and unrecoverable files") {
	storage::MemorySource volume(fixtures::fat32_scenario().volume);
	const storage::Fat32Scan s = scan(volume);
	auto device = storage::export_candidate(volume, find(s, "/keep.txt"), R"(\\.\PhysicalDrive0)");
	REQUIRE_FALSE(device.has_value());
	CHECK(device.error().code == storage::Errc::unsupported);
	t_temp_dir out;
	auto gone = storage::export_candidate(volume, find(s, "/overwritten.bin"), out.path);
	REQUIRE_FALSE(gone.has_value());
	CHECK(fs::is_empty(out.path));
}

TEST_CASE("a failed read during an export leaves no half written file") {
	storage::MemorySource memory(fixtures::fat32_scenario().volume);
	const storage::Fat32Scan s = scan(memory);
	const RecoveryCandidate& photo = find(s, "/DCIM/Holiday photo 2026.jpg");
	storage::FaultInjectingSource bad(memory);
	bad.add_bad_region(photo.extents[0].offset + 10000, 1);
	t_temp_dir out;
	auto exported = storage::export_candidate(bad, photo, out.path);
	REQUIRE_FALSE(exported.has_value());
	CHECK(exported.error().code == storage::Errc::io_error);
	CHECK_FALSE(fs::exists(out.path / "DCIM" / "Holiday photo 2026.jpg"));
}

TEST_CASE("a damaged boot sector falls back to the backup in sector 6") {
	auto image = fixtures::fat32_scenario().volume;
	image[11] = std::byte{ 0x33 }; // bytes per sector
	storage::MemorySource volume(image);
	const storage::Fat32Scan s = scan(volume);
	CHECK(s.volume.from_backup_boot_sector);
	CHECK(storage::has(s.diagnostics, storage::Diagnostic::Severity::warning));
	check_scenario(s);
}

TEST_CASE("what isn't fat32 is refused") {
	storage::MemorySource blank(fixtures::t_image(1 * fixtures::mib));
	auto none = storage::scan_fat32(blank);
	REQUIRE_FALSE(none.has_value());
	CHECK(none.error().code == storage::Errc::unsupported);

	fixtures::t_image ntfs(1 * fixtures::mib);
	fixtures::put_ntfs_boot(ntfs, 0);
	storage::MemorySource other(ntfs);
	CHECK_FALSE(storage::scan_fat32(other).has_value());

	storage::MemorySource tiny(fixtures::t_image(100));
	CHECK_FALSE(storage::scan_fat32(tiny).has_value());
}

TEST_CASE("a directory linked back into its parent is read once") {
	auto image = fixtures::fat32_scenario().volume;
	const fixtures::t_fat32 layout(8 * fixtures::mib);
	// DCIM's chain (cluster 3) continues into the root's first cluster, in both fats
	const std::size_t fat_sectors = (layout.cluster_offset(2) / 512 - fixtures::t_fat32::reserved) / 2;
	for (std::size_t copy = 0; copy < 2; copy++)
		fixtures::put32(image, (fixtures::t_fat32::reserved + copy * fat_sectors) * 512 + 3 * 4, 2);
	storage::MemorySource volume(image);
	const storage::Fat32Scan s = scan(volume);
	CHECK(s.candidates.size() == fixtures::fat32_scenario().expected.size());
	CHECK(find(s, "/DCIM").integrity == Integrity::partial);
}

TEST_CASE("an unreadable directory cluster is noted and the rest is scanned") {
	storage::MemorySource memory(fixtures::fat32_scenario().volume);
	storage::FaultInjectingSource bad(memory);
	bad.add_bad_region(fixtures::t_fat32(8 * fixtures::mib).cluster_offset(3), 512); // DCIM
	const storage::Fat32Scan s = scan(bad);
	CHECK(storage::has(s.diagnostics, storage::Diagnostic::Severity::error));
	CHECK(s.candidates.size() == fixtures::fat32_scenario().expected.size() - 1); // all but the photo
}

TEST_CASE("a cancelled fat32 scan stops") {
	storage::MemorySource volume(fixtures::fat32_scenario().volume);
	storage::CancellationToken cancel;
	cancel.cancel();
	auto scanned = storage::scan_fat32(volume, &cancel);
	REQUIRE_FALSE(scanned.has_value());
	CHECK(scanned.error().code == storage::Errc::cancelled);
}

TEST_CASE("random damage to the fat and the directories never crashes the scanner") {
	const auto original = fixtures::fat32_scenario().volume;
	const fixtures::t_fat32 layout(8 * fixtures::mib);
	const std::size_t data = layout.cluster_offset(2);
	std::mt19937_64 random(0xFA732);
	for (int round = 0; round < 300; round++) {
		auto image = original;
		const int flips = 1 + static_cast<int>(random() % 32);
		for (int i = 0; i < flips; i++) {
			const std::size_t region = random() % 3;
			const std::size_t at = region == 0 ? random() % 512                                                // the boot sector
				: region == 1 ? fixtures::t_fat32::reserved * 512 + random() % (data - fixtures::t_fat32::reserved * 512) // the fats
				: data + random() % (5 * 512);                                                                        // the first directories
			image[at] = static_cast<std::byte>(random());
		}
		storage::MemorySource volume(image);
		auto scanned = storage::scan_fat32(volume);
		if (!scanned)
			continue; // a boot sector damaged past use is a fine answer
		for (const RecoveryCandidate& c : scanned->candidates) {
			std::uint64_t total = 0;
			for (const storage::Extent& e : c.extents) {
				REQUIRE(e.offset + e.length <= image.size());
				total += e.length;
			}
			REQUIRE(total == c.recoverable_bytes);
			REQUIRE(c.recoverable_bytes <= c.size);
		}
	}
}
