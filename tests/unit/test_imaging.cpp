#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "storage/fault_injecting_source.h"
#include "storage/imaging.h"
#include "storage/memory_source.h"

namespace fs = std::filesystem;
using storage::BlockStatus;
using storage::Errc;
using storage::ImageMap;
using storage::ImagingOptions;
using storage::ImagingPhase;
using storage::MapBlock;

namespace {
	struct t_temp_dir {
		fs::path path = fs::temp_directory_path() / ("fenrir_imaging_" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
		t_temp_dir() { fs::create_directories(path); }
		~t_temp_dir() {
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	};

	std::vector<std::byte> pattern(std::size_t size, unsigned seed = 1) {
		std::vector<std::byte> bytes(size);
		std::uint32_t x = seed * 2654435761u + 1;
		for (std::byte& b : bytes) {
			x = x * 1664525u + 1013904223u;
			b = static_cast<std::byte>(x >> 24);
		}
		return bytes;
	}

	// read with sharing, so it works while a copy still has the file open
	std::vector<std::byte> read_file(const fs::path& path) {
		std::ifstream in(path, std::ios::binary);
		REQUIRE(in.is_open());
		std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::vector<std::byte> bytes(chars.size());
		std::memcpy(bytes.data(), chars.data(), chars.size());
		return bytes;
	}

	void write_file(const fs::path& path, std::span<const std::byte> bytes) {
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		REQUIRE(out.good());
	}

	void write_text(const fs::path& path, std::string_view text) { write_file(path, std::as_bytes(std::span(text))); }

	// every finished byte in image is the source's, every other one is still zero
	void check_image(const std::vector<std::byte>& source, const std::vector<std::byte>& image, const ImageMap& map) {
		REQUIRE(image.size() == source.size());
		REQUIRE(map.size() == source.size());
		for (const MapBlock& block : map.blocks())
			for (std::uint64_t i = block.pos; i < block.end(); i++) {
				const bool finished = block.status == BlockStatus::finished;
				if (finished ? image[i] != source[i] : image[i] != std::byte{ 0 }) {
					FAIL("byte " << i << " (" << static_cast<char>(block.status) << ") isn't what it should be");
					return;
				}
			}
	}

	// the map's invariants: from 0 to its size with no gaps, neighbours different, totals adding up
	void check_map(const ImageMap& map) {
		std::uint64_t next = 0, totals[5]{};
		const char order[] = "?*/-+";
		char last = 0;
		for (const MapBlock& block : map.blocks()) {
			CHECK(block.pos == next);
			CHECK(block.size > 0);
			CHECK(static_cast<char>(block.status) != last);
			last = static_cast<char>(block.status);
			totals[std::string_view(order).find(last)] += block.size;
			next = block.end();
		}
		CHECK(next == map.size());
		for (int i = 0; i < 5; i++)
			CHECK(map.bytes(static_cast<BlockStatus>(order[i])) == totals[i]);
	}

	struct t_observer final : storage::ImagingObserver {
		storage::CancellationToken* cancel = nullptr;
		std::uint64_t cancel_after = UINT64_MAX; // updates
		std::uint64_t updates = 0, saves = 0;
		std::function<void()> on_save;
		void update(const storage::ImagingStatus&, const ImageMap&) noexcept override {
			if (++updates >= cancel_after && cancel)
				cancel->cancel();
		}
		void saved(const storage::ImagingStatus&) noexcept override {
			saves++;
			if (on_save)
				on_save();
		}
	};

	ImagingOptions small() { // small enough that a test image has many clusters
		ImagingOptions o;
		o.cluster_size = 4096;
		o.skip_min = 8192;
		o.skip_max = 65536;
		o.save_every = std::chrono::hours(1); // after each pass: a save after every read flushes to disk every time
		return o;
	}

	std::uint64_t round_down(std::uint64_t x, std::uint64_t to) { return x - x % to; }
	std::uint64_t round_up(std::uint64_t x, std::uint64_t to) { return round_down(x + to - 1, to); }
}

TEST_CASE("a map splits and merges its blocks and keeps count") {
	ImageMap map(1000);
	CHECK(map.block_count() == 1);
	CHECK(map.bytes(BlockStatus::non_tried) == 1000);

	map.set(100, 50, BlockStatus::finished);
	map.set(150, 50, BlockStatus::finished); // touches the last one: merged
	CHECK(map.block_count() == 3);
	CHECK(map.block_at(120).pos == 100);
	CHECK(map.block_at(120).size == 100);
	CHECK(map.bytes(BlockStatus::finished) == 100);

	map.set(120, 10, BlockStatus::bad);
	CHECK(map.block_count() == 5);
	CHECK(map.find(BlockStatus::bad)->pos == 120);
	CHECK(map.find(BlockStatus::finished, 125)->pos == 130);
	CHECK(map.find(BlockStatus::finished, 105)->pos == 105); // inside a finished block: from there
	CHECK_FALSE(map.find(BlockStatus::non_scraped).has_value());

	map.set(0, 1000, BlockStatus::finished);
	CHECK(map.block_count() == 1);
	map.set(990, 100, BlockStatus::bad); // cut at the end
	CHECK(map.bytes(BlockStatus::bad) == 10);
	map.set(5000, 10, BlockStatus::bad); // past the end: nothing
	map.set(10, UINT64_MAX, BlockStatus::non_scraped); // its end overflows: to the map's end
	CHECK(map.bytes(BlockStatus::non_scraped) == 990);
	check_map(map);
}

TEST_CASE("a map matches a byte-by-byte model through random changes") {
	std::mt19937 random(7);
	const char statuses[] = "?*/-+";
	ImageMap map(3000);
	std::vector<char> model(3000, '?');
	for (int round = 0; round < 3000; round++) {
		const std::uint64_t pos = random() % 3100, size = 1 + random() % 300;
		const char status = statuses[random() % 5];
		map.set(pos, size, static_cast<BlockStatus>(status));
		for (std::uint64_t i = pos; i < pos + size && i < model.size(); i++)
			model[i] = status;
		if (round % 100 == 0) {
			check_map(map);
			for (std::uint64_t i = 0; i < model.size(); i += 7)
				REQUIRE(static_cast<char>(map.block_at(i).status) == model[i]);
		}
	}
	check_map(map);
	const auto parsed = ImageMap::parse(map.serialize("a comment\nof two lines"));
	REQUIRE(parsed.has_value());
	CHECK(parsed->blocks().size() == map.blocks().size());
	for (std::uint64_t i = 0; i < model.size(); i++)
		REQUIRE(static_cast<char>(parsed->block_at(i).status) == model[i]);
}

TEST_CASE("a map is written and read the way ddrescue does") {
	ImageMap map(0x30000);
	map.set(0, 0x10000, BlockStatus::finished);
	map.set(0x10000, 0x200, BlockStatus::bad);
	map.phase = ImagingPhase::trimming;
	map.current_pos = 0x10200;
	map.pass = 1;
	CHECK(map.serialize() ==
		"# Mapfile. Created by Fenrir\n"
		"# current_pos  current_status  current_pass\n"
		"0x00010200     *               1\n"
		"#      pos        size  status\n"
		"0x00000000  0x00010000  +\n"
		"0x00010000  0x00000200  -\n"
		"0x00010200  0x0001FE00  ?\n");

	// as ddrescue 1.2x writes one, with windows line ends and a status line without the pass, as older ones did
	const auto read = ImageMap::parse(
		"# Mapfile. Created by GNU ddrescue version 1.27\r\n"
		"# Command line: ddrescue /dev/sdb disk.img disk.map\r\n"
		"\r\n"
		"# current_pos  current_status\r\n"
		"0x7E000000     /\r\n"
		"#      pos        size  status\r\n"
		"0x00000000  0x7E000000  +\r\n"
		"2113929216  512  /\r\n"
		"0x7E000200  0x00000200  +\r\n");
	REQUIRE(read.has_value());
	CHECK(read->size() == 0x7E000400);
	CHECK(read->phase == ImagingPhase::scraping);
	CHECK(read->pass == 1);
	CHECK(read->current_pos == 0x7E000000);
	CHECK(read->bytes(BlockStatus::non_scraped) == 512);
	CHECK(read->block_count() == 3);
}

TEST_CASE("a map that isn't one is refused") {
	const char* const bad[] = {
		"",
		"# only comments\n",
		"0x0 ?\n", // a status line, no blocks
		"0x0 ?\n0x0 0x10 +\n0x20 0x10 +\n", // a gap
		"0x0 ?\n0x0 0x10 +\n0x8 0x10 +\n", // overlapping
		"0x0 ?\n0x10 0x10 +\n", // not from 0
		"0x0 ?\n0x0 0x0 +\n", // empty
		"0x0 ?\n0x0 0x10 x\n", // no such status
		"0x0 F\n0x0 0x10 +\n", // ddrescue filling, not a copy
		"0x0 ?\n0x0 010 +\n", // octal to ddrescue
		"0x0 ?\n0x0 0x10\n", // no status
		"0x0 ?\n0x0 0x10 + extra\n",
		"0x0 ?\n0x0 0x1FFFFFFFFFFFFFFFF +\n", // too big for 64 bits
		"0x0 ?\n0x0 0xFFFFFFFFFFFFFFFF +\n0xFFFFFFFFFFFFFFFF 0x2 +\n", // ends past 2^64
		"\x7F" "ELF binary\n",
	};
	for (const char* text : bad) {
		INFO(text);
		const auto parsed = ImageMap::parse(text);
		CHECK_FALSE(parsed.has_value());
	}
}

TEST_CASE("a source that reads cleanly is copied whole") {
	t_temp_dir dir;
	const auto bytes = pattern(300000); // not a multiple of the sector size
	storage::MemorySource source(bytes);
	t_observer observer;
	const auto status = storage::acquire_image(source, dir.path / "disk.img", dir.path / "disk.map", small(), nullptr, &observer);
	REQUIRE(status.has_value());
	CHECK(status->phase == ImagingPhase::finished);
	CHECK(status->finished == bytes.size());
	CHECK(status->bad == 0);
	CHECK(status->unknown == 0);
	CHECK(status->read_errors == 0);
	CHECK_FALSE(status->resumed);
	CHECK(observer.saves > 0);
	CHECK(read_file(dir.path / "disk.img") == bytes);

	const auto map = storage::load_map(dir.path / "disk.map");
	REQUIRE(map.has_value());
	CHECK(map->phase == ImagingPhase::finished);
	CHECK(map->block_count() == 1);
	CHECK_FALSE(fs::exists(dir.path / "disk.map.new"));

	// a finished copy run again reads nothing more and changes nothing
	const auto again = storage::acquire_image(source, dir.path / "disk.img", dir.path / "disk.map", small());
	REQUIRE(again.has_value());
	CHECK(again->resumed);
	CHECK(read_file(dir.path / "disk.img") == bytes);
}

TEST_CASE("unreadable areas end up bad to the sector, everything around them is copied") {
	t_temp_dir dir;
	const auto bytes = pattern(1 << 20);
	storage::MemorySource memory(bytes);
	storage::FaultInjectingSource source(memory);
	struct t_bad {
		std::uint64_t pos, size;
	};
	const t_bad bad[] = { { 100000, 3000 }, { 500000, 512 }, { 700000, 60000 }, { (1 << 20) - 100, 100 } };
	std::uint64_t expected_bad = 0;
	for (const t_bad& b : bad) {
		source.add_bad_region(b.pos, b.size);
		expected_bad += round_up(b.pos + b.size, 512) - round_down(b.pos, 512);
	}

	const auto status = storage::acquire_image(source, dir.path / "disk.img", dir.path / "disk.map", small());
	REQUIRE(status.has_value());
	CHECK(status->bad == expected_bad);
	CHECK(status->unknown == 0);
	CHECK(status->finished == bytes.size() - expected_bad);
	CHECK(status->read_errors > 0);

	const auto map = storage::load_map(dir.path / "disk.map");
	REQUIRE(map.has_value());
	check_map(*map);
	check_image(bytes, read_file(dir.path / "disk.img"), *map);
	for (const t_bad& b : bad)
		CHECK(map->block_at(b.pos).status == BlockStatus::bad);

	SUBCASE("without scraping, what's between the trimmed edges stays unknown, not bad") {
		t_temp_dir again;
		ImagingOptions o = small();
		o.scrape = false;
		storage::FaultInjectingSource source2(memory);
		source2.add_bad_region(700000, 60000);
		const auto s = storage::acquire_image(source2, again.path / "disk.img", again.path / "disk.map", o);
		REQUIRE(s.has_value());
		CHECK(s->unknown > 0);
		CHECK(s->unknown + s->bad == round_up(760000, 512) - round_down(700000, 512));
		const auto m = storage::load_map(again.path / "disk.map");
		REQUIRE(m.has_value());
		CHECK(m->bytes(BlockStatus::non_scraped) == s->unknown);
		check_image(bytes, read_file(again.path / "disk.img"), *m);
	}
}

TEST_CASE("retry passes get back sectors that read on a later try") {
	const auto bytes = pattern(200000);
	storage::MemorySource memory(bytes);

	t_temp_dir dir;
	storage::FaultInjectingSource flaky(memory);
	flaky.add_flaky_region(50000, 10, 3);
	ImagingOptions o = small();
	o.retries = 5;
	const auto status = storage::acquire_image(flaky, dir.path / "disk.img", dir.path / "disk.map", o);
	REQUIRE(status.has_value());
	CHECK(status->finished == bytes.size());
	CHECK(status->read_errors == 3);
	CHECK(read_file(dir.path / "disk.img") == bytes);

	t_temp_dir dir2;
	storage::FaultInjectingSource stubborn(memory);
	stubborn.add_flaky_region(50000, 10, 1000);
	const auto status2 = storage::acquire_image(stubborn, dir2.path / "disk.img", dir2.path / "disk.map", o);
	REQUIRE(status2.has_value());
	CHECK(status2->bad == 512);
}

TEST_CASE("a cancelled copy keeps its map and goes on from it") {
	t_temp_dir dir;
	const auto bytes = pattern(400000);
	storage::MemorySource memory(bytes);
	storage::FaultInjectingSource source(memory);
	source.add_flaky_region(123456, 1000, 1); // fails once, so the second run has some trimming to do too
	const fs::path image = dir.path / "disk.img", mapfile = dir.path / "disk.map";

	storage::CancellationToken cancel;
	t_observer observer;
	observer.cancel = &cancel;
	observer.cancel_after = 40;
	ImagingOptions o = small();
	const auto stopped = storage::acquire_image(source, image, mapfile, o, &cancel, &observer);
	REQUIRE_FALSE(stopped.has_value());
	CHECK(stopped.error().code == Errc::cancelled);

	const auto map = storage::load_map(mapfile);
	REQUIRE(map.has_value());
	CHECK(map->bytes(BlockStatus::finished) > 0);
	CHECK(map->unknown_bytes() > 0);
	check_image(bytes, read_file(image), *map);

	const auto resumed = storage::acquire_image(source, image, mapfile, o);
	REQUIRE(resumed.has_value());
	CHECK(resumed->resumed);
	CHECK(resumed->finished == bytes.size());
	CHECK(read_file(image) == bytes);
}

TEST_CASE("after a crash at any save, the map never claims more than the image has, and the copy goes on from there") {
	t_temp_dir dir;
	const auto bytes = pattern(100000);
	storage::MemorySource memory(bytes);
	storage::FaultInjectingSource source(memory);
	source.add_bad_region(30000, 5000);
	source.add_flaky_region(60000, 700, 2);
	const fs::path image = dir.path / "disk.img", mapfile = dir.path / "disk.map";

	// what's on disk at every save, map first and then the image: a crash right after this save would leave the
	// image with at least as much in it as this
	std::vector<std::pair<std::string, std::vector<std::byte>>> crashes;
	t_observer observer;
	observer.on_save = [&] {
		const auto map = read_file(mapfile);
		crashes.emplace_back(std::string(reinterpret_cast<const char*>(map.data()), map.size()), read_file(image));
	};
	ImagingOptions o = small();
	o.retries = 1;
	o.save_every = std::chrono::milliseconds(0);
	REQUIRE(storage::acquire_image(source, image, mapfile, o, nullptr, &observer).has_value());
	REQUIRE(crashes.size() > 10);
	ImagingOptions resume = o; // the retry gets back the flaky sector a crash may have left bad
	resume.save_every = std::chrono::hours(1);

	for (std::size_t i = 0; i < crashes.size(); i++) {
		const auto map = ImageMap::parse(crashes[i].first);
		REQUIRE(map.has_value());
		check_image(bytes, crashes[i].second, *map);
		if (i % (crashes.size() / 5) != 0)
			continue;

		// and that crashed state, copied out, finishes like a copy that was never interrupted
		t_temp_dir again;
		write_text(again.path / "disk.map", crashes[i].first);
		write_file(again.path / "disk.img", crashes[i].second);
		storage::FaultInjectingSource fresh(memory);
		fresh.add_bad_region(30000, 5000);
		const auto finished = storage::acquire_image(fresh, again.path / "disk.img", again.path / "disk.map", resume);
		REQUIRE(finished.has_value());
		CHECK(finished->bad == round_up(35000, 512) - round_down(30000, 512));
		const auto final_map = storage::load_map(again.path / "disk.map");
		REQUIRE(final_map.has_value());
		check_image(bytes, read_file(again.path / "disk.img"), *final_map);
	}
}

TEST_CASE("a device that stops answering stops the copy once it's had enough errors, with its map saved") {
	t_temp_dir dir;
	const auto bytes = pattern(300000);
	storage::MemorySource memory(bytes);
	storage::FaultInjectingSource source(memory);
	source.fail_after(20);
	ImagingOptions o = small();
	o.max_read_errors = 10;
	const auto status = storage::acquire_image(source, dir.path / "disk.img", dir.path / "disk.map", o);
	REQUIRE_FALSE(status.has_value());
	CHECK(status.error().code == Errc::io_error);
	const auto map = storage::load_map(dir.path / "disk.map");
	REQUIRE(map.has_value());
	CHECK(map->bytes(BlockStatus::finished) == 20 * 4096);
	check_image(bytes, read_file(dir.path / "disk.img"), *map);
}

TEST_CASE("a copy ddrescue started goes on in Fenrir") {
	t_temp_dir dir;
	const auto bytes = pattern(0x20000);
	storage::MemorySource source(bytes);
	std::vector<std::byte> half(bytes.size());
	std::copy_n(bytes.begin(), 0x10000, half.begin());
	write_file(dir.path / "disk.img", half);
	write_text(dir.path / "disk.map",
		"# Mapfile. Created by GNU ddrescue version 1.27\n"
		"# current_pos  current_status  current_pass\n"
		"0x00010000     ?               1\n"
		"#      pos        size  status\n"
		"0x00000000  0x00010000  +\n"
		"0x00010000  0x00010000  ?\n");
	const auto status = storage::acquire_image(source, dir.path / "disk.img", dir.path / "disk.map", small());
	REQUIRE(status.has_value());
	CHECK(status->resumed);
	CHECK(read_file(dir.path / "disk.img") == bytes);
}

TEST_CASE("an image and a map that don't belong together are left alone") {
	t_temp_dir dir;
	const auto bytes = pattern(100000);
	storage::MemorySource source(bytes);
	const fs::path image = dir.path / "disk.img", mapfile = dir.path / "disk.map";

	SUBCASE("an image that's already there, without a map") {
		write_file(image, pattern(10));
		const auto r = storage::acquire_image(source, image, mapfile, small());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().code == Errc::exists);
		CHECK(read_file(image) == pattern(10));
		CHECK_FALSE(fs::exists(mapfile));
	}
	SUBCASE("a map of a source with another size") {
		write_text(mapfile, ImageMap(5000).serialize());
		const auto r = storage::acquire_image(source, image, mapfile, small());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().code == Errc::mismatch);
		CHECK_FALSE(fs::exists(image));
	}
	SUBCASE("a map that says data was copied, without its image") {
		ImageMap map(bytes.size());
		map.set(0, 4096, BlockStatus::finished);
		write_text(mapfile, map.serialize());
		const auto r = storage::acquire_image(source, image, mapfile, small());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().code == Errc::mismatch);
	}
	SUBCASE("a map with nothing copied yet, without its image: the image is made") {
		write_text(mapfile, ImageMap(bytes.size()).serialize());
		REQUIRE(storage::acquire_image(source, image, mapfile, small()).has_value());
		CHECK(read_file(image) == bytes);
	}
	SUBCASE("going on with a different source of the same size") {
		REQUIRE(storage::acquire_image(source, image, mapfile, small()).has_value());
		storage::MemorySource other(pattern(bytes.size(), 2));
		const auto r = storage::acquire_image(other, image, mapfile, small());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().code == Errc::mismatch);
		CHECK(read_file(image) == bytes);
	}
	SUBCASE("a file where the map is saved first, that isn't a map") {
		write_text(dir.path / "disk.map.new", "not a map");
		const auto r = storage::acquire_image(source, image, mapfile, small());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().code == Errc::exists);
		CHECK(fs::exists(dir.path / "disk.map.new"));
	}
	SUBCASE("a map save that was cut off is cleaned up") {
		write_text(dir.path / "disk.map.new", ImageMap(bytes.size()).serialize());
		REQUIRE(storage::acquire_image(source, image, mapfile, small()).has_value());
		CHECK_FALSE(fs::exists(dir.path / "disk.map.new"));
	}
	SUBCASE("the image and the map the same file") {
		const auto r = storage::acquire_image(source, image, dir.path / "." / "disk.img", small());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().code == Errc::invalid_argument);
	}
	SUBCASE("a device as the image") {
		const auto r = storage::acquire_image(source, "\\\\.\\PhysicalDrive9", mapfile, small());
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().code == Errc::unsupported);
	}
	SUBCASE("options that don't make sense") {
		ImagingOptions o = small();
		o.sector_size = 500;
		CHECK(storage::acquire_image(source, image, mapfile, o).error().code == Errc::invalid_argument);
		o = small();
		o.cluster_size = 1000;
		CHECK(storage::acquire_image(source, image, mapfile, o).error().code == Errc::invalid_argument);
		CHECK_FALSE(fs::exists(image));
	}
}
