// fenrir-cli: the read-only debug and test interface to the storage engine. it opens disk images and says what's in them,
// it has no command that changes anything

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "storage/file_image_source.h"
#include "storage/hash.h"
#include "storage/hex_dump.h"
#include "storage/partition.h"

namespace {
	constexpr std::string_view version = "0.0.1";
	constexpr std::uint64_t max_hex_length = 1 << 20;

	void usage() {
		std::print(
			"usage: fenrir-cli <command>\n"
			"\n"
			"  info <image>                         open a raw disk image read-only and report its size\n"
			"  partitions <image> [--sector-size N] list its partition table (MBR or GPT) and anything wrong with it\n"
			"  hex <image> [offset] [length]        show bytes from it, 512 from offset 0 unless told (0x for hex)\n"
			"  hash <image>                         its SHA-256\n"
			"  --version                            print the version\n");
	}

	std::string human_size(std::uint64_t bytes) {
		const char* units[] = { "bytes", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
		double value = static_cast<double>(bytes);
		int unit = 0;
		while (value >= 1024.0 && unit < 6) {
			value /= 1024.0;
			unit++;
		}
		if (unit == 0)
			return std::format("{} bytes", bytes);
		return std::format("{:.2f} {}", value, units[unit]);
	}

	std::string_view utf8(const std::u8string& text) { return { reinterpret_cast<const char*>(text.data()), text.size() }; }

	void print_error(const storage::Error& error) {
		std::print(stderr, "error: {}: {}", error.context, storage::to_string(error.code));
		if (error.os_error != 0)
			std::print(stderr, " (os error {})", error.os_error);
		std::println(stderr, "");
	}

	// decimal, or hex with 0x
	std::optional<std::uint64_t> number(const std::filesystem::path& arg) {
		const std::string text = arg.string();
		const bool hex = text.starts_with("0x") || text.starts_with("0X");
		const char* begin = text.data() + (hex ? 2 : 0);
		const char* end = text.data() + text.size();
		std::uint64_t value = 0;
		const auto [at, error] = std::from_chars(begin, end, value, hex ? 16 : 10);
		if (error != std::errc() || at != end || begin == end)
			return std::nullopt;
		return value;
	}

	std::unique_ptr<storage::FileImageSource> open(const std::filesystem::path& path) {
		auto opened = storage::FileImageSource::open(path);
		if (!opened) {
			print_error(opened.error());
			return nullptr;
		}
		return std::move(*opened);
	}

	int info(const std::filesystem::path& path) {
		auto image = open(path);
		if (!image)
			return 1;
		const std::uint64_t size = image->size_bytes();

		std::println("image:   {}", utf8(path.u8string()));
		std::println("size:    {} bytes ({})", size, human_size(size));
		if (size % 512 == 0)
			std::println("sectors: {} x 512 bytes", size / 512);
		else
			std::println("sectors: not a whole number of 512 byte sectors ({} bytes left over)", size % 512);
#ifdef _WIN32
		std::println("access:  read-only, other programs can't open it for writing while it's open");
#else
		std::println("access:  read-only");
#endif
		return 0;
	}

	int partitions(const std::filesystem::path& path, std::uint32_t sector_size) {
		auto image = open(path);
		if (!image)
			return 1;
		auto scanned = storage::scan_partitions(*image, { .sector_size = sector_size });
		if (!scanned) {
			print_error(scanned.error());
			return 1;
		}
		const storage::PartitionMap& map = *scanned;

		std::println("image:   {}", utf8(path.u8string()));
		std::println("size:    {} bytes ({}), {} sectors", image->size_bytes(), human_size(image->size_bytes()), map.disk_sectors);
		switch (map.scheme) {
		case storage::PartitionScheme::none:
			if (map.whole_disk != storage::FileSystem::unknown)
				std::println("scheme:  none, the whole disk is one {} filesystem", storage::to_string(map.whole_disk));
			else
				std::println("scheme:  none");
			break;
		case storage::PartitionScheme::mbr:
			std::println("scheme:  MBR, {} byte sectors, disk signature {:08X}", map.sector_size, map.mbr_signature);
			break;
		case storage::PartitionScheme::gpt:
			std::print("scheme:  GPT, {} byte sectors", map.sector_size);
			if (!map.disk_guid.is_zero())
				std::print(", disk {}", storage::to_string(map.disk_guid));
			std::println("{}", map.gpt_from_backup ? ", from the backup" : "");
			if (map.gpt_last_usable != 0)
				std::println("usable:  sectors {} to {}", map.gpt_first_usable, map.gpt_last_usable);
			break;
		}

		if (!map.partitions.empty()) {
			std::println("");
			std::println("    #  first sector       sectors        size  type                    filesystem  name");
			for (const storage::Partition& p : map.partitions) {
				std::string type;
				if (map.scheme == storage::PartitionScheme::mbr)
					type = std::format("{:02X} {}", p.mbr_type, p.type_name.empty() ? "unknown" : p.type_name);
				else
					type = p.type_name.empty() ? storage::to_string(p.type_guid) : std::string(p.type_name);
				std::string line = std::format("  {:>3}{:1} {:>12} {:>13} {:>11}  {:<23} {:<11} {}", p.number, p.bootable ? "*" : "",
					p.first_lba, p.sector_count, human_size(p.size_bytes), type, storage::to_string(p.filesystem), p.name);
				if (!p.inside_disk)
					line += " (past the end)";
				while (line.ends_with(' '))
					line.pop_back();
				std::println("{}", line);
			}
			if (map.scheme == storage::PartitionScheme::mbr)
				std::println("  * bootable. 5 and up are logical partitions, inside the extended one");
		}

		if (!map.diagnostics.empty()) {
			std::println("");
			std::println("notes:");
			for (const storage::Diagnostic& d : map.diagnostics)
				std::println("  {}: {}", storage::to_string(d.severity), d.message);
		}
		return 0;
	}

	int hex(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t length) {
		if (length == 0 || length > max_hex_length) {
			std::println(stderr, "error: the length has to be 1 to {} bytes", max_hex_length);
			return 2;
		}
		auto image = open(path);
		if (!image)
			return 1;
		const std::uint64_t size = image->size_bytes();
		if (offset >= size) {
			std::println(stderr, "error: offset {} is past the end of the image ({} bytes)", offset, size);
			return 1;
		}
		std::vector<std::byte> bytes(static_cast<std::size_t>(std::min(length, size - offset)));
		if (auto read = storage::read_exact(*image, offset, bytes); !read) {
			print_error(read.error());
			return 1;
		}
		std::print("{}", storage::hex_dump(bytes, offset));
		if (bytes.size() < length)
			std::println(stderr, "the image ends after {} bytes", bytes.size());
		return 0;
	}

	bool terminal(std::FILE* file) {
#ifdef _WIN32
		return _isatty(_fileno(file)) != 0;
#else
		return isatty(fileno(file)) != 0;
#endif
	}

	// a percentage on stderr, only when it's a terminal, so a redirected run gets just the hash
	class t_percent final : public storage::ProgressSink {
	public:
		void progress(std::uint64_t done, std::uint64_t total) noexcept override {
			const int percent = total == 0 ? 100 : static_cast<int>(done * 100 / total);
			if (percent != shown_) {
				shown_ = percent;
				std::print(stderr, "\rhashing: {:3}%", percent);
			}
		}
		~t_percent() override {
			if (shown_ >= 0)
				std::print(stderr, "\r              \r");
		}

	private:
		int shown_ = -1;
	};

	int hash(const std::filesystem::path& path) {
		auto image = open(path);
		if (!image)
			return 1;
		storage::Result<storage::Sha256::Digest> digest;
		{
			t_percent percent;
			digest = storage::sha256_source(*image, nullptr, terminal(stderr) ? &percent : nullptr);
		}
		if (!digest) {
			print_error(digest.error());
			return 1;
		}
		std::println("image:   {}", utf8(path.u8string()));
		std::println("size:    {} bytes", image->size_bytes());
		std::println("sha256:  {}", storage::to_hex(*digest));
		return 0;
	}

	int run(const std::vector<std::filesystem::path>& args) {
		if (args.empty()) {
			usage();
			return 2;
		}
		const std::filesystem::path& command = args[0];
		if (command == "--version") {
			std::println("fenrir-cli {}", version);
			return 0;
		}
		if (command == "--help" || command == "-h") {
			usage();
			return 0;
		}
		if (command == "info" && args.size() == 2)
			return info(args[1]);
		if (command == "hash" && args.size() == 2)
			return hash(args[1]);
		if (command == "partitions" && args.size() == 2)
			return partitions(args[1], 0);
		if (command == "partitions" && args.size() == 4 && args[2] == "--sector-size") {
			const auto size = number(args[3]);
			if (!size || *size > 65536) {
				usage();
				return 2;
			}
			return partitions(args[1], static_cast<std::uint32_t>(*size));
		}
		if (command == "hex" && args.size() >= 2 && args.size() <= 4) {
			const auto offset = args.size() >= 3 ? number(args[2]) : std::optional<std::uint64_t>(0);
			const auto length = args.size() >= 4 ? number(args[3]) : std::optional<std::uint64_t>(512);
			if (!offset || !length) {
				usage();
				return 2;
			}
			return hex(args[1], *offset, *length);
		}
		usage();
		return 2;
	}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
	// dlls loaded at runtime only come from system32, the same as the gui
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
	SetConsoleOutputCP(CP_UTF8); // paths and partition names are printed as utf-8
#else
int main(int argc, char** argv) {
#endif
	std::vector<std::filesystem::path> args;
	for (int i = 1; i < argc; i++)
		args.emplace_back(argv[i]);
	return run(args);
}
