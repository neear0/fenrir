// fenrir-make-fixtures <dir>: writes the test disk images from fixtures.h, fat32.h, exfat.h, ntfs.h and carving.h into dir,
// and into dir/<name>_originals what each recoverable file in fat32.img, exfat.img, ntfs.img and carve.img has to come back as,
// at the path `fenrir-cli recover` gives it. run by ctest before the cli tests, it only ever writes into the build
// directory it's given

#include <filesystem>
#include <fstream>
#include <print>

#include "carving.h"
#include "exfat.h"
#include "ntfs.h"
#include "fat32.h"
#include "fixtures.h"

namespace {
	bool write(const std::filesystem::path& path, const fixtures::t_image& image) {
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
		out.close();
		if (!out) {
			std::println(stderr, "couldn't write {}", path.string());
			return false;
		}
		return true;
	}

	std::filesystem::path utf8_path(const std::string& text) { return std::u8string(text.begin(), text.end()); }
}

int main(int argc, char** argv) {
	if (argc != 2) {
		std::println(stderr, "usage: fenrir-make-fixtures <dir>");
		return 2;
	}
	const std::filesystem::path dir = argv[1];
	std::error_code error;
	std::filesystem::create_directories(dir, error);

	fixtures::t_image bad_primary = fixtures::gpt_disk();
	bad_primary.at(512 + 48) ^= std::byte{ 0x01 }; // the primary header's last usable lba, so its crc no longer matches
	fixtures::t_image truncated = fixtures::gpt_disk();
	truncated.resize(7 * fixtures::mib); // the backup gpt and the end of partition 3 are gone

	bool ok = true;
	ok &= write(dir / "mbr.img", fixtures::mbr_disk());
	ok &= write(dir / "gpt.img", fixtures::gpt_disk());
	ok &= write(dir / "gpt_4k.img", fixtures::gpt_disk(4096));
	ok &= write(dir / "gpt_bad_primary.img", bad_primary);
	ok &= write(dir / "gpt_truncated.img", truncated);
	ok &= write(dir / "superfloppy.img", fixtures::superfloppy_disk());
	ok &= write(dir / "blank.img", fixtures::t_image(64 * 1024));
	ok &= write(dir / "fat32.img", fixtures::fat32_disk());

	// the deleted files an export must reproduce exactly, partial ones as far as they're recoverable
	auto originals = [&](const std::string& name, const std::vector<fixtures::t_fat32_expected>& expected) {
		std::filesystem::remove_all(dir / name, error);
		for (const fixtures::t_fat32_expected& file : expected) {
			if (!file.deleted || file.directory || !file.matches_original || file.integrity == "unrecoverable")
				continue;
			const bool partial = file.recoverable < file.original.size();
			const fixtures::t_image content(file.original.begin(), file.original.begin() + static_cast<std::ptrdiff_t>(file.recoverable));
			ok &= write(dir / name / utf8_path(file.path.substr(1) + (partial ? ".partial" : "")), content);
		}
	};
	originals("fat32_originals", fixtures::fat32_scenario().expected);
	ok &= write(dir / "exfat.img", fixtures::exfat_disk());
	originals("exfat_originals", fixtures::exfat_scenario().expected);
	ok &= write(dir / "ntfs.img", fixtures::ntfs_disk());
	originals("ntfs_originals", fixtures::ntfs_scenario().expected);

	// the carved files `fenrir-cli recover --carved` must reproduce, under carved/ like it writes them. the ones the
	// fat32 scan finds too are left out of a carve, and a fragmented jpeg's cut isn't known in advance
	const fixtures::t_carve_disk carve = fixtures::carve_disk();
	ok &= write(dir / "carve.img", carve.image);
	std::filesystem::remove_all(dir / "carve_originals", error);
	for (const fixtures::t_carve_expected& file : carve.expected) {
		if (file.in_fat32 || !file.matches_original())
			continue;
		const bool partial = file.integrity == "partial";
		const fixtures::t_image content(file.original.begin(), file.original.begin() + static_cast<std::ptrdiff_t>(file.recoverable));
		ok &= write(dir / "carve_originals" / "carved" / (file.name() + (partial ? ".partial" : "")), content);
	}

	if (ok)
		std::println("fixtures written to {}", dir.string());
	return ok ? 0 : 1;
}
