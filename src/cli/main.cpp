// fenrir-cli: the read-only debug and test interface to the storage engine. it opens disk images and says what's in them,
// it has no command that changes anything

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "storage/file_image_source.h"

namespace {
	constexpr std::string_view version = "0.0.1";

	void usage() {
		std::cout <<
			"usage: fenrir-cli <command>\n"
			"\n"
			"  info <image>   open a raw disk image read-only and report its size\n"
			"  --version      print the version\n";
	}

	std::string human_size(std::uint64_t bytes) {
		const char* units[] = { "bytes", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
		double value = static_cast<double>(bytes);
		int unit = 0;
		while (value >= 1024.0 && unit < 6) {
			value /= 1024.0;
			unit++;
		}
		char text[64];
		if (unit == 0)
			std::snprintf(text, sizeof(text), "%llu bytes", static_cast<unsigned long long>(bytes));
		else
			std::snprintf(text, sizeof(text), "%.2f %s", value, units[unit]);
		return text;
	}

	void print_error(const storage::Error& error) {
		std::cerr << "error: " << error.context << ": " << storage::to_string(error.code);
		if (error.os_error != 0)
			std::cerr << " (os error " << error.os_error << ")";
		std::cerr << "\n";
	}

	int info(const std::filesystem::path& path) {
		auto opened = storage::FileImageSource::open(path);
		if (!opened) {
			print_error(opened.error());
			return 1;
		}
		const storage::FileImageSource& image = *opened.value();
		const std::uint64_t size = image.size_bytes();

		const std::u8string name = path.u8string();
		std::cout << "image:   " << std::string_view(reinterpret_cast<const char*>(name.data()), name.size()) << "\n";
		std::cout << "size:    " << size << " bytes (" << human_size(size) << ")\n";
		if (size % 512 == 0)
			std::cout << "sectors: " << size / 512 << " x 512 bytes\n";
		else
			std::cout << "sectors: not a whole number of 512 byte sectors (" << size % 512 << " bytes left over)\n";
#ifdef _WIN32
		std::cout << "access:  read-only, other programs can't open it for writing while it's open\n";
#else
		std::cout << "access:  read-only\n";
#endif
		return 0;
	}

	int run(const std::vector<std::filesystem::path>& args) {
		if (args.empty()) {
			usage();
			return 2;
		}
		const std::filesystem::path& command = args[0];
		if (command == "--version") {
			std::cout << "fenrir-cli " << version << "\n";
			return 0;
		}
		if (command == "--help" || command == "-h") {
			usage();
			return 0;
		}
		if (command == "info" && args.size() == 2)
			return info(args[1]);
		usage();
		return 2;
	}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
	// dlls loaded at runtime only come from system32, the same as the gui
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
	SetConsoleOutputCP(CP_UTF8); // paths are printed as utf-8
#else
int main(int argc, char** argv) {
#endif
	std::vector<std::filesystem::path> args;
	for (int i = 1; i < argc; i++)
		args.emplace_back(argv[i]);
	return run(args);
}
