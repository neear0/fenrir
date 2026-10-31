#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>

#include "make_fixtures/carving.h"
#include "storage/carving.h"
#include "storage/exporter.h"
#include "storage/fat32.h"
#include "storage/fault_injecting_source.h"
#include "storage/memory_source.h"
#include "storage/partition.h"

namespace fs = std::filesystem;
using storage::Integrity;
using storage::RecoveryCandidate;

namespace {
	const fixtures::t_carve_disk& disk() {
		static const fixtures::t_carve_disk d = fixtures::carve_disk();
		return d;
	}

	// the one across the 4 MiB chunk boundary, with a thumbnail inside
	const fixtures::t_carve_expected& camera_jpeg() {
		const auto found = std::ranges::find(disk().expected, 4 * fixtures::mib - 6 * 512, &fixtures::t_carve_expected::offset);
		REQUIRE(found != disk().expected.end());
		return *found;
	}

	fixtures::t_image read_file(const fs::path& path) {
		std::ifstream in(path, std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(in)), {});
		fixtures::t_image bytes(text.size());
		std::ranges::transform(text, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
		return bytes;
	}

	storage::CarveScan carved(storage::IBlockSource& source, const storage::CarveOptions& options = {}) {
		auto scanned = storage::carve(source, options);
		REQUIRE(scanned.has_value());
		return std::move(*scanned);
	}

	std::string reasons(const RecoveryCandidate& c) {
		std::string text;
		for (const std::string& r : c.reasons)
			text += r + "\n";
		return text;
	}

	bool same_bytes(const fixtures::t_image& image, std::uint64_t at, const fixtures::t_image& original, std::uint64_t length) {
		return length <= original.size() && at + length <= image.size()
			&& std::equal(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length), image.begin() + static_cast<std::ptrdiff_t>(at));
	}

	// what the carver found on carve_disk() against what's there, one by one and in order
	void check_disk(const storage::CarveScan& s, bool with_fat32) {
		std::vector<fixtures::t_carve_expected> expected;
		for (const fixtures::t_carve_expected& e : disk().expected)
			if (with_fat32 || !e.in_fat32)
				expected.push_back(e);
		CAPTURE(s.candidates.size());
		REQUIRE(s.candidates.size() == expected.size());
		for (std::size_t i = 0; i < expected.size(); i++) {
			const RecoveryCandidate& c = s.candidates[i];
			const fixtures::t_carve_expected& e = expected[i];
			CAPTURE(e.name());
			CAPTURE(reasons(c));
			CHECK(c.id == i);
			CHECK(c.origin == storage::Origin::carved);
			CHECK(c.name == e.name());
			CHECK(c.path == "/carved/" + e.name());
			CHECK(c.format == e.format);
			CHECK(storage::to_string(c.integrity) == e.integrity);
			CHECK(c.entry_offset == e.offset);
			REQUIRE(c.extents.size() == 1);
			CHECK(c.extents[0].offset == e.offset);
			CHECK(c.extents[0].length == c.recoverable_bytes);
			CHECK(c.reasons.size() >= 2);
			if (e.recoverable != 0) {
				CHECK(c.recoverable_bytes == e.recoverable);
				CHECK(same_bytes(disk().image, e.offset, e.original, e.recoverable));
			} else { // a fragmented jpeg: cut at a sector at or a little after its first part ends, never before
				CHECK(c.recoverable_bytes >= e.fragment);
				CHECK(c.recoverable_bytes <= e.fragment + 1024);
				CHECK(c.recoverable_bytes % 512 == 0);
			}
			if (e.integrity == "intact")
				CHECK(c.size == e.original.size());
		}
	}
}

TEST_CASE("the carving disk, file by file") {
	storage::MemorySource source(disk().image);
	const storage::CarveScan s = carved(source);
	check_disk(s, true);
	CHECK(s.rejected >= 5); // the four bare signatures and the jpeg whose image data was overwritten
	CHECK(s.diagnostics.empty());
}

TEST_CASE("files a fat32 scan already found are left out of a carve of the whole disk") {
	storage::MemorySource source(disk().image);
	storage::CarveScan s = carved(source);

	auto map = storage::scan_partitions(source);
	REQUIRE(map.has_value());
	REQUIRE(map->partitions.size() == 1);
	auto view = storage::open_partition(source, map->partitions[0]);
	REQUIRE(view.has_value());
	auto fat = storage::scan_fat32(**view);
	REQUIRE(fat.has_value());
	CHECK(fat->candidates.size() == 2);
	CHECK(storage::drop_known(s.candidates, fat->candidates, disk().partition_offset) == 2);
	check_disk(s, false);

	// counted from the wrong place, nothing matches
	storage::CarveScan again = carved(source);
	CHECK(storage::drop_known(again.candidates, fat->candidates, 0) == 0);
}

TEST_CASE("the same files are found whatever the chunk size") {
	storage::MemorySource source(disk().image);
	for (std::size_t chunk : { std::size_t(4096), std::size_t(5000), std::size_t(65536), std::size_t(1) << 20, std::size_t(16) << 20 }) {
		CAPTURE(chunk);
		check_disk(carved(source, { .chunk_size = chunk }), true);
	}
}

TEST_CASE("a signature split across two chunks is found") {
	const fixtures::t_image jpeg = fixtures::t_jpeg_encoder::encode(24, 16, 40, true);
	const fixtures::t_image png = fixtures::png(8, 8, 41, 100);
	fixtures::t_image image(32768);
	std::ranges::copy(jpeg, image.begin() + 4094); // FF D8 | FF
	std::ranges::copy(png, image.begin() + 16380); // 4 bytes either side
	storage::MemorySource source(image);

	const storage::CarveScan s = carved(source, { .alignment = 1, .chunk_size = 4096 });
	REQUIRE(s.candidates.size() == 2);
	CHECK(s.candidates[0].entry_offset == 4094);
	CHECK(s.candidates[0].integrity == Integrity::intact);
	CHECK(s.candidates[0].recoverable_bytes == jpeg.size());
	CHECK(s.candidates[1].entry_offset == 16380);
	CHECK(s.candidates[1].integrity == Integrity::intact);
	CHECK(s.candidates[1].recoverable_bytes == png.size());

	// at sector boundaries only, neither is where a file would start
	CHECK(carved(source).candidates.empty());
}

TEST_CASE("looking everywhere finds the same files, and nothing inside them") {
	storage::MemorySource source(disk().image);
	const storage::CarveScan s = carved(source, { .alignment = 1 });
	for (const fixtures::t_carve_expected& e : disk().expected) {
		CAPTURE(e.name());
		const auto found = std::ranges::find(s.candidates, e.offset, &RecoveryCandidate::entry_offset);
		REQUIRE(found != s.candidates.end());
		CHECK(storage::to_string(found->integrity) == e.integrity);
	}
	// the camera jpeg's thumbnail, and anything else, would start inside a file already found
	for (std::size_t i = 1; i < s.candidates.size(); i++)
		CHECK(s.candidates[i].entry_offset >= s.candidates[i - 1].entry_offset + s.candidates[i - 1].recoverable_bytes);
}

TEST_CASE("random bytes and bare signatures aren't files") {
	fixtures::t_image image = fixtures::noise(50, 4 * fixtures::mib);
	std::mt19937 random(51);
	const std::string_view signatures[] = { std::string_view("\xFF\xD8\xFF", 3), "\x89PNG\r\n\x1A\n", "%PDF-1.7", "PK\x03\x04" };
	for (int i = 0; i < 1000; i++) {
		const std::string_view s = signatures[i % 4];
		const std::size_t at = random() % (image.size() - 16);
		for (std::size_t k = 0; k < s.size(); k++)
			image[at + k] = static_cast<std::byte>(s[k]);
	}
	storage::MemorySource source(image);
	const storage::CarveScan s = carved(source, { .alignment = 1 });
	CHECK(s.candidates.empty());
	CHECK(s.rejected >= 990); // a few might overlap each other
}

TEST_CASE("damage inside a file stops it from being intact") {
	auto first = [](const fixtures::t_image& image) {
		storage::MemorySource source(image);
		const storage::CarveScan s = carved(source);
		return s.candidates.empty() ? std::optional<RecoveryCandidate>() : std::optional(s.candidates.front());
	};

	SUBCASE("a jpeg with someone else's bytes in its image data is cut before them") {
		fixtures::t_image jpeg = fixtures::t_jpeg_encoder::encode(160, 120, 60);
		const fixtures::t_image other = fixtures::noise(61, 1024);
		std::ranges::copy(other, jpeg.begin() + 3072);
		const auto c = first(jpeg);
		REQUIRE(c);
		CAPTURE(reasons(*c));
		CHECK(c->integrity == Integrity::partial);
		CHECK(c->recoverable_bytes >= 3072);
		CHECK(c->recoverable_bytes <= 3072 + 1024 + 512);
	}
	SUBCASE("a png with one byte changed is cut at that chunk") {
		std::vector<std::size_t> ends;
		fixtures::t_image png = fixtures::png(64, 48, 62, 2000, &ends);
		png[ends[2] + 100] ^= std::byte{ 0x20 }; // in the 3rd idat chunk
		const auto c = first(png);
		REQUIRE(c);
		CHECK(c->integrity == Integrity::partial);
		CHECK(c->recoverable_bytes == ends[2]);
	}
	SUBCASE("a zip whose stored entry changed loses that entry and what comes after") {
		std::vector<std::size_t> ends;
		fixtures::t_image zip = fixtures::zip({ { "a.txt", fixtures::text("a", 1000) }, { "b.txt", fixtures::text("b", 1000) }, { "c.txt", fixtures::text("c", 1000) } }, &ends);
		zip[ends[1] - 10] ^= std::byte{ 1 };
		const auto c = first(zip);
		REQUIRE(c);
		CHECK(c->integrity == Integrity::partial);
		CHECK(c->recoverable_bytes == ends[0]);
		zip[ends[0] - 10] ^= std::byte{ 1 }; // the first one too: nothing's left to trust
		CHECK_FALSE(first(zip));
	}
	SUBCASE("a pdf whose startxref points at the wrong place is cut before its startxref") {
		std::vector<std::size_t> ends;
		fixtures::t_image pdf = fixtures::pdf("moved", false, &ends);
		fixtures::t_image moved(pdf.begin(), pdf.begin() + static_cast<std::ptrdiff_t>(ends[2]));
		fixtures::append(moved, std::string(37, ' ')); // as if a piece were missing or put in: the offsets after it are off
		moved.insert(moved.end(), pdf.begin() + static_cast<std::ptrdiff_t>(ends[2]), pdf.end());
		const auto c = first(moved);
		REQUIRE(c);
		CAPTURE(reasons(*c));
		CHECK(c->integrity == Integrity::partial);
		CHECK(c->recoverable_bytes > ends.back() + 37);
		CHECK(c->recoverable_bytes < moved.size());
		CHECK(reasons(*c).find("startxref points") != std::string::npos);
	}
	SUBCASE("a pdf whose xref table is off by an object is uncertain") {
		fixtures::t_image pdf = fixtures::pdf("off", false);
		const std::string text(reinterpret_cast<const char*>(pdf.data()), pdf.size());
		const std::size_t entry = text.find(" 00000 n\r\n", text.find("xref"));
		REQUIRE(entry != std::string::npos);
		pdf[entry - 1] = static_cast<std::byte>(static_cast<char>(pdf[entry - 1]) == '9' ? '8' : static_cast<char>(pdf[entry - 1]) + 1);
		const auto c = first(pdf);
		REQUIRE(c);
		CHECK(c->integrity == Integrity::uncertain);
	}
}

TEST_CASE("an unreadable sector inside a file cuts it there") {
	storage::MemorySource memory(disk().image);
	storage::FaultInjectingSource bad(memory);
	const fixtures::t_carve_expected& camera = camera_jpeg();
	REQUIRE(camera.original.size() > 8192);
	bad.add_bad_region(camera.offset + 6000, 1);
	const storage::CarveScan s = carved(bad);
	const auto c = std::ranges::find(s.candidates, camera.offset, &RecoveryCandidate::entry_offset);
	REQUIRE(c != s.candidates.end());
	CAPTURE(reasons(*c));
	CHECK(c->integrity == Integrity::partial);
	CHECK(c->recoverable_bytes == camera.offset + 6000 - (camera.offset + 6000) % 512 - camera.offset);
	CHECK(reasons(*c).find("can't be read") != std::string::npos);
	CHECK(storage::has(s.diagnostics, storage::Diagnostic::Severity::warning));
}

TEST_CASE("short reads change nothing") {
	storage::MemorySource memory(disk().image);
	storage::FaultInjectingSource shorts(memory);
	shorts.set_max_read(1000);
	check_disk(carved(shorts), true);
}

TEST_CASE("a file longer than the limit is cut at it") {
	storage::MemorySource source(disk().image);
	const storage::CarveScan s = carved(source, { .max_file_size = 4096 });
	const fixtures::t_carve_expected& camera = camera_jpeg();
	const auto c = std::ranges::find(s.candidates, camera.offset, &RecoveryCandidate::entry_offset);
	REQUIRE(c != s.candidates.end());
	CHECK(c->integrity == Integrity::partial);
	CHECK(c->recoverable_bytes == 4096);
	CHECK(reasons(*c).find("limit") != std::string::npos);
}

TEST_CASE("carved files export the same as the originals") {
	const fs::path dir = fs::temp_directory_path() / ("fenrir_carve_" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
	storage::MemorySource source(disk().image);
	const storage::CarveScan s = carved(source);
	std::size_t compared = 0;
	for (const fixtures::t_carve_expected& e : disk().expected) {
		if (!e.matches_original())
			continue;
		CAPTURE(e.name());
		const auto c = std::ranges::find(s.candidates, e.offset, &RecoveryCandidate::entry_offset);
		REQUIRE(c != s.candidates.end());
		auto exported = storage::export_candidate(source, *c, dir);
		REQUIRE(exported.has_value());
		CHECK(exported->path.filename().string() == e.name() + (e.integrity == "partial" ? ".partial" : ""));
		CHECK(exported->complete == (e.integrity != "partial"));
		const fixtures::t_image written = read_file(exported->path);
		CHECK(written.size() == e.recoverable);
		CHECK(same_bytes(written, 0, e.original, e.recoverable));
		CHECK(storage::to_hex(exported->sha256) == storage::to_hex(storage::Sha256::of(std::span(e.original).first(static_cast<std::size_t>(e.recoverable)))));
		compared++;
	}
	CHECK(compared >= 12);
	std::error_code ignored;
	fs::remove_all(dir, ignored);
}

TEST_CASE("only the types asked for") {
	storage::MemorySource source(disk().image);
	const storage::CarveScan s = carved(source, { .types = { storage::FileType::png, storage::FileType::pdf } });
	CHECK(s.candidates.size() == 6);
	for (const RecoveryCandidate& c : s.candidates)
		CHECK((c.format == "PNG" || c.format == "PDF"));
	CHECK(storage::carve_formats().size() == 4);
}

TEST_CASE("bad options are refused, and a cancelled carve stops") {
	storage::MemorySource source(disk().image);
	CHECK(storage::carve(source, { .alignment = 0 }).error().code == storage::Errc::invalid_argument);
	CHECK(storage::carve(source, { .chunk_size = 100 }).error().code == storage::Errc::invalid_argument);
	CHECK(storage::carve(source, { .max_file_size = 0 }).error().code == storage::Errc::invalid_argument);
	storage::CancellationToken cancel;
	cancel.cancel();
	CHECK(storage::carve(source, {}, &cancel).error().code == storage::Errc::cancelled);
}

TEST_CASE("random damage never crashes the carver or puts a file outside the source") {
	std::mt19937 random(70);
	for (int round = 0; round < 12; round++) {
		fixtures::t_image image = disk().image;
		for (int i = 0; i < 400; i++) {
			const std::size_t at = 3 * fixtures::mib + random() % (image.size() - 3 * fixtures::mib);
			image[at] = static_cast<std::byte>(random());
		}
		storage::MemorySource source(image);
		const storage::CarveScan s = carved(source, { .alignment = round % 4 == 0 ? 1u : 512u });
		for (const RecoveryCandidate& c : s.candidates) {
			REQUIRE(c.extents.size() == 1);
			CHECK(c.recoverable_bytes > 0);
			CHECK(c.extents[0].offset + c.extents[0].length <= image.size());
			CHECK_FALSE(c.reasons.empty());
		}
	}
}
