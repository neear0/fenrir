// fenrir-make-fixtures <dir>: writes the test disk images from fixtures.h and fat32.h into dir, and into dir/fat32_originals
// what each recoverable file in fat32.img has to come back as, at the path `fenrir-cli recover` gives it. run by ctest
// before the cli tests, it only ever writes into the build directory it's given

#include <filesystem>
#include <fstream>
#include <print>

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
	std::filesystem::remove_all(dir / "fat32_originals", error);
	for (const fixtures::t_fat32_expected& file : fixtures::fat32_scenario().expected) {
		if (!file.deleted || file.directory || !file.matches_original || file.integrity == "unrecoverable")
			continue;
		const bool partial = file.recoverable < file.original.size();
		const fixtures::t_image content(file.original.begin(), file.original.begin() + static_cast<std::ptrdiff_t>(file.recoverable));
		ok &= write(dir / "fat32_originals" / utf8_path(file.path.substr(1) + (partial ? ".partial" : "")), content);
	}
	if (ok)
		std::println("fixtures written to {}", dir.string());
	return ok ? 0 : 1;
}
