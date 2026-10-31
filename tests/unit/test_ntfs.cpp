#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <random>
#include <string>

#include "make_fixtures/ntfs.h"
#include "storage/exporter.h"
#include "storage/fault_injecting_source.h"
#include "storage/filesystem.h"
#include "storage/memory_source.h"
#include "storage/ntfs.h"
#include "storage/partition.h"

namespace fs = std::filesystem;
using storage::Integrity;
using storage::RecoveryCandidate;

namespace {
	struct t_temp_dir {
		fs::path path = fs::temp_directory_path() / ("fenrir_ntfs_" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
		t_temp_dir() { fs::create_directories(path); }
		~t_temp_dir() {
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	};

	const fixtures::t_ntfs_scenario& scenario() {
		static const fixtures::t_ntfs_scenario s = fixtures::ntfs_scenario();
		return s;
	}

	storage::NtfsScan scan(storage::IBlockSource& source) {
		auto scanned = storage::scan_ntfs(source);
		REQUIRE(scanned.has_value());
		return std::move(*scanned);
	}

	std::string reasons(const RecoveryCandidate& c) {
		std::string text;
		for (const std::string& r : c.reasons)
			text += r + "\n";
		return text;
	}

	std::string notes(const storage::NtfsScan& s) {
		std::string text;
		for (const storage::Diagnostic& d : s.diagnostics)
			text += d.message + "\n";
		return text;
	}

	const RecoveryCandidate& find(const storage::NtfsScan& s, std::string_view path) {
		const auto found = std::ranges::find(s.candidates, path, &RecoveryCandidate::path);
		CAPTURE(path);
		REQUIRE(found != s.candidates.end());
		return *found;
	}

	void check_scenario(const std::vector<RecoveryCandidate>& candidates) {
		const auto& expected = scenario().expected;
		REQUIRE(candidates.size() == expected.size());
		for (std::size_t i = 0; i < expected.size(); i++) {
			const RecoveryCandidate& c = candidates[i];
			const fixtures::t_ntfs_expected& e = expected[i];
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

	std::size_t record_of(const storage::NtfsScan& s, std::string_view path) { return static_cast<std::size_t>(find(s, path).entry_offset); }
}

TEST_CASE("the ntfs scenario volume, record by record") {
	storage::MemorySource volume(scenario().volume);
	const storage::NtfsScan s = scan(volume);
	CAPTURE(notes(s));
	CHECK(s.volume.cluster_size == 4096);
	CHECK(s.volume.record_size == 1024);
	CHECK(s.volume.cluster_count == fixtures::t_ntfs(16 * fixtures::mib).cluster_count());
	CHECK(s.volume.mft_cluster == fixtures::t_ntfs::mft_lcn);
	CHECK(s.volume.label == "FENRIR NT");
	CHECK(s.volume.major_version == 3);
	CHECK(s.volume.minor_version == 1);
	CHECK(s.volume.serial == 0x2026A5F1C0DE4E7Full);
	CHECK(s.volume.records == fixtures::t_ntfs::mft_records);
	CHECK_FALSE(s.volume.from_backup_boot_sector);
	CHECK_FALSE(s.volume.mft_from_mirror);
	CHECK(s.diagnostics.empty());
	check_scenario(s.candidates);

	// $Extend's own files stay out
	CHECK(std::ranges::none_of(s.candidates, [](const RecoveryCandidate& c) { return c.name.starts_with("$") && c.path != "/$OrphanFiles/lost.txt"; }));

	// what each rating rests on
	CHECK(reasons(find(s, "/fragmented.bin")).find("each of its 3 pieces") != std::string::npos);
	CHECK(find(s, "/fragmented.bin").extents.size() == 3); // clusters 120-121, 130, then 125
	CHECK(reasons(find(s, "/interrupted.bin")).find("cluster 151 is in use") != std::string::npos);
	CHECK(reasons(find(s, "/overwritten.bin")).find("its first cluster (160) is in use") != std::string::npos);
	CHECK(reasons(find(s, "/compressed.log")).find("NTFS compression") != std::string::npos);
	CHECK(reasons(find(s, "/secret.doc")).find("EFS") != std::string::npos);
	CHECK(reasons(find(s, "/$OrphanFiles/lost.txt")).find("can't be found any more") != std::string::npos);
	CHECK(reasons(find(s, "/first.log")).find("also claimed by deleted entry") != std::string::npos);
	CHECK(find(s, "/Fenrir \xE2\x80\x93 \xE1\x9A\xA0.txt").short_name == "FENRIR~1.TXT");
	CHECK(find(s, "/download.exe:Zone.Identifier").parent == RecoveryCandidate::no_parent);
	CHECK(find(s, "/Photos/IMG_0001.jpg").parent == find(s, "/Photos").id);
	CHECK(find(s, "/old stuff/a letter.txt").parent == find(s, "/old stuff").id);

	// resident data comes from the record, fixed up: tiny.txt runs over the bytes the update sequence protects
	const RecoveryCandidate& tiny = find(s, "/tiny.txt");
	CHECK(tiny.extents.empty());
	CHECK(tiny.resident.size() == 300);

	// sparse runs and past the initialized size: zeros, not the disk
	const RecoveryCandidate& vhd = find(s, "/sparse.vhd");
	REQUIRE(vhd.extents.size() == 3);
	CHECK_FALSE(vhd.extents[0].zeros);
	CHECK(vhd.extents[1].zeros);
	CHECK(vhd.extents[1].length == 8192);
	CHECK(reasons(vhd).find("sparse") != std::string::npos);
	const RecoveryCandidate& pre = find(s, "/preallocated.dat");
	REQUIRE(pre.extents.size() == 2);
	CHECK(pre.extents[0].length == 5000);
	CHECK(pre.extents[1].zeros);

	const RecoveryCandidate& photo = find(s, "/Photos/IMG_0001.jpg");
	CHECK(photo.first_cluster == 170);
	CHECK(photo.modified.valid);
	CHECK(photo.modified.utc);
	CHECK(photo.modified.year == 2026);
	CHECK(photo.modified.month == 10);
	CHECK(photo.modified.day == 31);
	CHECK(photo.modified.hour == 12);
	CHECK(photo.modified.minute == 34);
	CHECK(photo.modified.second == 56);
	CHECK(photo.modified.millisecond == 789);
}

TEST_CASE("exported ntfs files hash the same as what was written") {
	storage::MemorySource volume(scenario().volume);
	const storage::NtfsScan s = scan(volume);
	t_temp_dir out;
	for (std::size_t i = 0; i < s.candidates.size(); i++) {
		const RecoveryCandidate& c = s.candidates[i];
		const fixtures::t_ntfs_expected& e = scenario().expected[i];
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
	CHECK(fs::exists(out.path / "tiny.txt"));
	CHECK(fs::exists(out.path / "interrupted.bin.partial"));
	CHECK(fs::exists(out.path / "download.exe_Zone.Identifier"));
	CHECK(fs::exists(out.path / "$OrphanFiles" / "lost.txt"));
	CHECK(fs::exists(out.path / "old stuff" / "a letter.txt"));
}

TEST_CASE("the ntfs disk: found through its partition table and scan_volume") {
	storage::MemorySource disk(fixtures::ntfs_disk());
	auto map = storage::scan_partitions(disk);
	REQUIRE(map.has_value());
	REQUIRE(map->partitions.size() == 1);
	CHECK(map->partitions[0].filesystem == storage::FileSystem::ntfs);
	CHECK(storage::can_scan(storage::FileSystem::ntfs));
	auto view = storage::open_partition(disk, map->partitions[0]);
	REQUIRE(view.has_value());
	auto scanned = storage::scan_volume(**view, storage::FileSystem::ntfs);
	REQUIRE(scanned.has_value());
	CHECK(scanned->summary.starts_with("NTFS 3.1, 4096 byte clusters"));
	CHECK(scanned->summary.ends_with("label FENRIR NT"));
	check_scenario(scanned->candidates);
}

TEST_CASE("a torn mft record is left out and counted") {
	auto image = scenario().volume;
	storage::MemorySource clean(image);
	const std::size_t at = record_of(scan(clean), "/report.pdf");
	image[at + 510] ^= std::byte{ 0xFF }; // the update sequence number at the end of its first 512 bytes
	storage::MemorySource volume(image);
	const storage::NtfsScan s = scan(volume);
	CHECK(std::ranges::find(s.candidates, "/report.pdf", &RecoveryCandidate::path) == s.candidates.end());
	CHECK(s.candidates.size() == scenario().expected.size() - 1);
	CHECK(notes(s).find("1 MFT record failed its update sequence check") != std::string::npos);
}

TEST_CASE("a damaged $MFT record is read from $MFTMirr") {
	auto image = scenario().volume;
	image[fixtures::t_ntfs::record_offset(0) + 1022] ^= std::byte{ 0xFF };
	storage::MemorySource volume(image);
	const storage::NtfsScan s = scan(volume);
	CHECK(s.volume.mft_from_mirror);
	CHECK(notes(s).find("the copy in $MFTMirr is used") != std::string::npos);
	check_scenario(s.candidates);
}

TEST_CASE("a damaged ntfs boot sector falls back to the backup in the last sector") {
	auto image = scenario().volume;
	image[13] = std::byte{ 3 }; // sectors per cluster, not a power of 2
	storage::MemorySource volume(image);
	const storage::NtfsScan s = scan(volume);
	CHECK(s.volume.from_backup_boot_sector);
	check_scenario(s.candidates);
}

TEST_CASE("what isn't ntfs is refused") {
	storage::MemorySource blank(fixtures::t_image(1 * fixtures::mib));
	auto none = storage::scan_ntfs(blank);
	REQUIRE_FALSE(none.has_value());
	CHECK(none.error().code == storage::Errc::unsupported);
	fixtures::t_image signature_only(1 * fixtures::mib);
	fixtures::put_ntfs_boot(signature_only, 0);
	storage::MemorySource stub(signature_only);
	CHECK_FALSE(storage::scan_ntfs(stub).has_value());
	storage::MemorySource fat32(fixtures::fat32_scenario().volume);
	CHECK_FALSE(storage::scan_ntfs(fat32).has_value());
}

TEST_CASE("an unreadable mft record costs that record, not the ones around it") {
	storage::MemorySource clean(scenario().volume);
	const std::size_t at = record_of(scan(clean), "/Photos/IMG_0001.jpg");
	storage::MemorySource memory(scenario().volume);
	storage::FaultInjectingSource bad(memory);
	bad.add_bad_region(at, 512);
	const storage::NtfsScan s = scan(bad);
	CHECK(notes(s).find("1 MFT record couldn't be read") != std::string::npos);
	CHECK(s.candidates.size() == scenario().expected.size() - 1);
}

TEST_CASE("directories whose parents point at each other are listed once, under $OrphanFiles") {
	fixtures::t_ntfs fs(16 * fixtures::mib);
	const std::uint64_t a = fs.add_dir(fixtures::t_ntfs::root, u"a");
	const std::uint64_t b = fs.add_dir(a, u"b");
	fs.add_file(b, u"inside.txt", fixtures::text("inside", 100), {}, { .resident = true });
	fs.reparent(a, b);
	storage::MemorySource volume(fs.finish(0));
	const storage::NtfsScan s = scan(volume);
	REQUIRE(s.candidates.size() == 3);
	CHECK(s.candidates[0].path == "/$OrphanFiles/a");
	CHECK(s.candidates[1].path == "/$OrphanFiles/a/b");
	CHECK(s.candidates[2].path == "/$OrphanFiles/a/b/inside.txt");
	CHECK(s.candidates[0].reasons.at(0).find("point at each other") != std::string::npos);
}

TEST_CASE("a cancelled ntfs scan stops") {
	storage::MemorySource volume(scenario().volume);
	storage::CancellationToken cancel;
	cancel.cancel();
	auto scanned = storage::scan_ntfs(volume, &cancel);
	REQUIRE_FALSE(scanned.has_value());
	CHECK(scanned.error().code == storage::Errc::cancelled);
}

TEST_CASE("random damage to ntfs's boot sector, mft and bitmap never crashes the scanner") {
	const auto& original = scenario().volume;
	const std::size_t mft = fixtures::t_ntfs::record_offset(0), bitmap = fixtures::t_ntfs::cluster_offset(fixtures::t_ntfs::bitmap_lcn);
	std::mt19937_64 random(0x47F5);
	for (int round = 0; round < 300; round++) {
		auto image = original;
		const int flips = 1 + static_cast<int>(random() % 48);
		for (int i = 0; i < flips; i++) {
			const std::size_t region = random() % 3;
			const std::size_t at = region == 0 ? random() % 512
				: region == 1 ? mft + random() % (fixtures::t_ntfs::mft_records * fixtures::t_ntfs::record_size)
				: bitmap + random() % 512;
			image[at] = static_cast<std::byte>(random());
		}
		storage::MemorySource volume(image);
		auto scanned = storage::scan_ntfs(volume);
		if (!scanned)
			continue;
		for (const RecoveryCandidate& c : scanned->candidates) {
			std::uint64_t total = c.resident.size();
			for (const storage::Extent& e : c.extents) {
				REQUIRE((e.zeros || e.offset + e.length <= image.size()));
				total += e.length;
			}
			REQUIRE(total == c.recoverable_bytes);
			REQUIRE(c.recoverable_bytes <= c.size);
		}
	}
}
