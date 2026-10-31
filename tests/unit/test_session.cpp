#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include "make_fixtures/carving.h"
#include "storage/carving.h"
#include "storage/memory_source.h"
#include "storage/session.h"

namespace fs = std::filesystem;
using storage::Integrity;
using storage::Origin;

namespace {
	struct t_temp_dir {
		fs::path path = fs::temp_directory_path() / ("fenrir_session_" + std::to_string(std::random_device{}()) + std::to_string(std::random_device{}()));
		t_temp_dir() { fs::create_directories(path); }
		~t_temp_dir() {
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	};

	fs::path write_image(const fs::path& dir, const fixtures::t_image& image, std::size_t zeros_after = 0) {
		const fs::path path = dir / "disk.img";
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
		const std::vector<char> zeros(std::size_t(1) << 20);
		for (std::size_t done = 0; done < zeros_after; done += zeros.size())
			out.write(zeros.data(), static_cast<std::streamsize>(std::min(zeros.size(), zeros_after - done)));
		return path;
	}

	fixtures::t_image read_file(const fs::path& path) {
		std::ifstream in(path, std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(in)), {});
		fixtures::t_image bytes(text.size());
		std::ranges::transform(text, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
		return bytes;
	}

	const fixtures::t_carve_disk& disk() {
		static const fixtures::t_carve_disk d = fixtures::carve_disk();
		return d;
	}

	// what a session found, as names and statuses in order, to compare two scans by
	std::vector<std::string> summary(const storage::SessionSnapshot& s) {
		std::vector<std::string> out;
		for (const storage::FoundFile& f : *s.files)
			out.push_back(std::format("{} {} {} {} {}", f.volume, f.file.path, storage::to_string(f.file.integrity), f.file.recoverable_bytes, f.file.id));
		return out;
	}
}

TEST_CASE("a session scans an image: its partition table, its FAT32 volume, then the deep scan") {
	t_temp_dir dir;
	const fs::path image = write_image(dir.path, disk().image);
	storage::RecoverySession session;
	REQUIRE(session.scan(image));
	session.wait();
	const auto s = session.snapshot();
	CAPTURE(s->status);
	CHECK_FALSE(s->error);
	CHECK(s->open);
	CHECK(s->phase == storage::SessionPhase::idle);
	CHECK(s->image_size == disk().image.size());
	REQUIRE(s->volumes.size() == 2);
	CHECK(s->volumes[0].name == "the whole image");
	CHECK(s->volumes[1].name == "partition 1");
	CHECK(s->volumes[1].offset == disk().partition_offset);
	CHECK(s->volumes[1].scanned);
	CHECK(s->volumes[1].files == 2);
	CHECK(s->carve_dropped == 2); // the jpeg and the png in partition 1, found by both
	CHECK(s->carved_to == disk().image.size());
	CHECK_FALSE(s->can_resume);

	std::size_t carved = 0;
	for (std::size_t i = 0; i < s->files->size(); i++) {
		const storage::FoundFile& f = (*s->files)[i];
		CHECK(f.file.id == i);
		carved += f.file.origin == Origin::carved;
		CHECK((f.file.origin == Origin::carved) == (f.volume == 0));
	}
	CHECK(carved == disk().expected.size() - 2);
	CHECK(s->files->size() == disk().expected.size());
	CHECK(s->status.starts_with("done"));
}

TEST_CASE("carving in slices finds what one carve does") {
	storage::MemorySource source(disk().image);
	auto whole = storage::carve(source);
	REQUIRE(whole.has_value());
	std::vector<storage::RecoveryCandidate> sliced;
	for (std::uint64_t pos = 0; pos < disk().image.size();) {
		auto part = storage::carve(source, { .from = pos, .to = pos + 700 * 1024 }); // not a multiple of anything
		REQUIRE(part.has_value());
		CHECK(part->next > pos);
		std::ranges::move(part->candidates, std::back_inserter(sliced));
		pos = part->next;
	}
	REQUIRE(sliced.size() == whole->candidates.size());
	for (std::size_t i = 0; i < sliced.size(); i++) {
		CAPTURE(sliced[i].name);
		CHECK(sliced[i].name == whole->candidates[i].name);
		CHECK(sliced[i].integrity == whole->candidates[i].integrity);
		CHECK(sliced[i].recoverable_bytes == whole->candidates[i].recoverable_bytes);
	}
}

TEST_CASE("an export writes every chosen file and a report, under the partition's name or carved/") {
	t_temp_dir dir;
	const fs::path image = write_image(dir.path, disk().image), out = dir.path / "out";
	storage::RecoverySession session;
	REQUIRE(session.scan(image));
	session.wait();
	auto s = session.snapshot();
	std::vector<std::uint64_t> all;
	for (const storage::FoundFile& f : *s->files)
		all.push_back(f.file.id);
	REQUIRE(session.export_files(all, out));
	session.wait();
	s = session.snapshot();
	CAPTURE(s->status);
	CHECK_FALSE(s->error);
	REQUIRE(s->exported->size() == s->files->size()); // none of them is a directory or unrecoverable
	for (const storage::ExportRecord& r : *s->exported)
		CHECK(r.error.empty());

	// the fat32 ones keep their names, under the partition
	CHECK(read_file(out / "partition 1" / "photo.jpg") == disk().expected[0].original);
	CHECK(read_file(out / "partition 1" / "chart.png") == disk().expected[1].original);
	for (const fixtures::t_carve_expected& e : disk().expected) {
		if (e.in_fat32 || !e.matches_original())
			continue;
		CAPTURE(e.name());
		const fixtures::t_image written = read_file(out / "carved" / (e.name() + (e.integrity == "partial" ? ".partial" : "")));
		CHECK(written == fixtures::t_image(e.original.begin(), e.original.begin() + static_cast<std::ptrdiff_t>(e.recoverable)));
	}

	REQUIRE(fs::exists(s->report));
	CHECK(s->report.filename() == "fenrir report.txt");
	const fixtures::t_image report = read_file(s->report);
	const std::string text(reinterpret_cast<const char*>(report.data()), report.size());
	CHECK(text.find("files:        16 of 16 written") != std::string::npos);
	CHECK(text.find("sha-256:") != std::string::npos);
	CHECK(text.find("partial  /carved/") != std::string::npos);

	// again into the same folder: nothing's replaced, every name gets a (2), the report too
	REQUIRE(session.export_files(all, out));
	session.wait();
	s = session.snapshot();
	CHECK(s->report.filename() == "fenrir report (2).txt");
	CHECK(fs::exists(out / "partition 1" / "photo (2).jpg"));
	CHECK(read_file(out / "partition 1" / "photo.jpg") == disk().expected[0].original);
}

TEST_CASE("a preview reads a found file's first bytes") {
	t_temp_dir dir;
	const fs::path image = write_image(dir.path, disk().image);
	storage::RecoverySession session;
	REQUIRE(session.scan(image));
	session.wait();
	const auto s = session.snapshot();
	const auto jpeg = std::ranges::find_if(*s->files, [](const storage::FoundFile& f) { return f.file.origin == Origin::carved; });
	REQUIRE(jpeg != s->files->end());
	session.preview(jpeg->file.id);
	storage::FilePreview p;
	for (int i = 0; i < 2000 && !p.ready; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		p = session.preview_result();
	}
	REQUIRE(p.ready);
	CHECK(p.id == jpeg->file.id);
	CHECK(p.error.empty());
	CHECK(p.offset == jpeg->file.entry_offset);
	REQUIRE(p.bytes.size() == 256);
	CHECK(p.bytes[0] == std::byte{ 0xFF });
	CHECK(p.bytes[1] == std::byte{ 0xD8 });

	// the fat32 photo, through its partition
	session.preview(0);
	p = {};
	for (int i = 0; i < 2000 && !p.ready; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		p = session.preview_result();
	}
	REQUIRE(p.ready);
	CHECK(p.offset == disk().expected[0].offset);
	CHECK(p.bytes[0] == std::byte{ 0xFF });
}

TEST_CASE("an image that isn't there fails the scan, and says why") {
	t_temp_dir dir;
	storage::RecoverySession session;
	REQUIRE(session.scan(dir.path / "missing.img"));
	session.wait();
	const auto s = session.snapshot();
	REQUIRE(s->error);
	CHECK(s->error->code == storage::Errc::not_found);
	CHECK_FALSE(s->open);
	CHECK(s->status.starts_with("it couldn't be opened"));
	CHECK(s->files->empty());
}

TEST_CASE("a stopped deep scan goes on where it stopped and finds the same files") {
	t_temp_dir dir;
	const fs::path image = write_image(dir.path, disk().image, std::size_t(400) << 20); // slices of 64 MiB, so there are 7
	storage::RecoverySession full;
	REQUIRE(full.scan(image));
	full.wait();
	const std::vector<std::string> expected = summary(*full.snapshot());

	bool resumed = false;
	for (int attempt = 0; attempt < 5 && !resumed; attempt++) {
		storage::RecoverySession session;
		REQUIRE(session.scan(image));
		CHECK_FALSE(session.scan(image)); // one job at a time
		CHECK_FALSE(session.export_files({ 0 }, dir.path / "out"));
		while (session.busy() && !(session.snapshot()->phase == storage::SessionPhase::carving && session.snapshot()->carved_to > 0))
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		session.cancel();
		session.wait();
		auto s = session.snapshot();
		if (!s->can_resume)
			continue; // it got to the end before it saw the cancel: try again
		CHECK(s->error);
		CHECK(s->error->code == storage::Errc::cancelled);
		CHECK(s->carved_to < s->image_size);
		CHECK(s->carved_to > 0);
		REQUIRE(session.resume());
		session.wait();
		s = session.snapshot();
		CHECK_FALSE(s->error);
		CHECK_FALSE(s->can_resume);
		CHECK(s->carved_to == s->image_size);
		CHECK(summary(*s) == expected);
		resumed = true;
	}
	CHECK(resumed);
}
