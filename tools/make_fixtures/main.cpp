// fenrir-make-fixtures <dir>: writes the test disk images from fixtures.h into dir. run by ctest before the cli tests,
// it only ever writes into the build directory it's given

#include <filesystem>
#include <fstream>
#include <print>

#include "fixtures.h"

namespace {
	bool write(const std::filesystem::path& path, const fixtures::t_image& image) {
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
		out.close();
		if (!out) {
			std::println(stderr, "couldn't write {}", path.string());
			return false;
		}
		return true;
	}
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
	if (ok)
		std::println("fixtures written to {}", dir.string());
	return ok ? 0 : 1;
}
