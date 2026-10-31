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

#include "storage/carving.h"
#include "storage/exporter.h"
#include "storage/filesystem.h"
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
			"  files <image> [--partition N] [--all] [--why]\n"
			"                                       the deleted files on a FAT32 or exFAT volume, how recoverable each is\n"
			"                                       and (--why) what that's based on. --all lists the live ones too\n"
			"  carve <image> [--partition N] [--every-byte] [--why]\n"
			"                                       files found by their content alone (JPEG, PNG, PDF, ZIP and what\n"
			"                                       a zip can be: DOCX, ODT, JAR...) on the whole image, or one\n"
			"                                       partition. ones a filesystem scan finds too are left out, `files` has\n"
			"                                       them. --every-byte looks for them at every byte, not every 512th\n"
			"  recover <image> <dir> [--carved] [--partition N] [--every-byte] [--id N]...\n"
			"                                       copy deleted files (--carved: carved ones) out to dir: all that\n"
			"                                       can be, or the ids given.\n"
			"                                       nothing in dir is ever replaced. dir shouldn't be on the drive\n"
			"                                       the image came from, if that drive is the one being recovered\n"
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

	// a percentage on stderr, so a redirected run gets just the results
	class t_percent final : public storage::ProgressSink {
	public:
		explicit t_percent(std::string_view what) : what_(what) {}

		void progress(std::uint64_t done, std::uint64_t total) noexcept override {
			const int percent = total == 0 ? 100 : static_cast<int>(done * 100 / total);
			if (percent != shown_) {
				shown_ = percent;
				std::print(stderr, "\r{}: {:3}%", what_, percent);
			}
		}
		~t_percent() override {
			if (shown_ >= 0)
				std::print(stderr, "\r{:{}}\r", "", what_.size() + 6);
		}

		// itself when stderr is a terminal, else nothing
		storage::ProgressSink* sink() { return terminal(stderr) ? this : nullptr; }

	private:
		std::string_view what_;
		int shown_ = -1;
	};

	int hash(const std::filesystem::path& path) {
		auto image = open(path);
		if (!image)
			return 1;
		storage::Result<storage::Sha256::Digest> digest;
		{
			t_percent percent("hashing");
			digest = storage::sha256_source(*image, nullptr, percent.sink());
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

	// the volume in an image: the partition asked for, else the first one fenrir can read the filesystem of, else the whole
	// image when it has no partition table. nothing, with the reason printed, when there isn't one
	struct t_volume {
		std::unique_ptr<storage::PartitionView> partition; // null: the whole image is the volume
		storage::IBlockSource* source = nullptr;
		std::string where;
		storage::FileSystem filesystem = storage::FileSystem::unknown;
	};

	std::optional<t_volume> find_volume(storage::FileImageSource& image, std::optional<std::uint64_t> number) {
		auto map = storage::scan_partitions(image);
		if (!map) {
			print_error(map.error());
			return std::nullopt;
		}
		t_volume volume;
		const storage::Partition* chosen = nullptr;
		for (const storage::Partition& p : map->partitions)
			if (number ? p.number == *number : storage::can_scan(p.filesystem) && p.inside_disk) {
				chosen = &p;
				break;
			}
		if (chosen) {
			auto view = storage::open_partition(image, *chosen);
			if (!view) {
				print_error(view.error());
				return std::nullopt;
			}
			volume.partition = std::move(*view);
			volume.source = volume.partition.get();
			volume.where = std::format("partition {}", chosen->number);
			volume.filesystem = chosen->filesystem;
		} else if (!number && map->partitions.empty()) {
			volume.source = &image;
			volume.where = "the whole image";
			volume.filesystem = map->whole_disk;
		} else {
			if (number)
				std::println(stderr, "error: there's no partition {}, `fenrir-cli partitions` lists them", *number);
			else
				std::println(stderr, "error: no partition looks like a FAT32 or exFAT one, pick one with --partition N");
			return std::nullopt;
		}
		return volume;
	}

	std::optional<storage::VolumeScan> scan_volume(t_volume& volume) {
		if (!storage::can_scan(volume.filesystem)) {
			std::println(stderr, "error: {} looks like {}, fenrir can't read its records (FAT32 and exFAT only so far)", volume.where, storage::to_string(volume.filesystem));
			return std::nullopt;
		}
		auto scanned = storage::scan_volume(*volume.source, volume.filesystem);
		if (!scanned) {
			print_error(scanned.error());
			return std::nullopt;
		}
		std::println("volume:  {}, {}", volume.where, scanned->summary);
		return std::move(*scanned);
	}

	void print_diagnostics(const std::vector<storage::Diagnostic>& diagnostics) {
		if (diagnostics.empty())
			return;
		std::println("");
		std::println("notes:");
		for (const storage::Diagnostic& d : diagnostics)
			std::println("  {}: {}", storage::to_string(d.severity), d.message);
	}

	int files(const std::filesystem::path& path, std::optional<std::uint64_t> partition, bool all, bool why) {
		auto image = open(path);
		if (!image)
			return 1;
		auto volume = find_volume(*image, partition);
		if (!volume)
			return 1;
		auto scan = scan_volume(*volume);
		if (!scan)
			return 1;

		std::uint64_t deleted = 0;
		std::println("");
		std::println("    id  status                size  deleted  path");
		for (const storage::RecoveryCandidate& c : scan->candidates) {
			deleted += c.deleted;
			if (!all && !c.deleted)
				continue;
			const std::string size = c.directory ? "<dir>" : std::format("{}", c.size);
			std::println("  {:>4}  {:<13} {:>12}  {:<7}  {}", c.id, storage::to_string(c.integrity), size, c.deleted ? "deleted" : "", c.path);
			if (why)
				for (const std::string& reason : c.reasons)
					std::println("        - {}", reason);
		}
		std::println("");
		std::println("{} entries, {} deleted{}", scan->candidates.size(), deleted, all ? "" : " (those are listed, --all lists everything)");
		print_diagnostics(scan->diagnostics);
		return 0;
	}

	int export_all(storage::IBlockSource& source, const std::vector<const storage::RecoveryCandidate*>& chosen, const std::filesystem::path& destination) {
		std::println("to:      {}", utf8(destination.u8string()));
		std::println("");
		std::uint64_t written = 0, bytes = 0, failed = 0;
		for (const storage::RecoveryCandidate* c : chosen) {
			auto exported = storage::export_candidate(source, *c, destination);
			if (!exported) {
				failed++;
				std::println("  {:>4}  not written ({}: {})  {}", c->id, exported.error().context, storage::to_string(exported.error().code), c->path);
				continue;
			}
			written++;
			bytes += exported->bytes;
			std::println("  {:>4}  {:<13} {}  {}", c->id, storage::to_string(c->integrity), storage::to_hex(exported->sha256), utf8(exported->path.make_preferred().u8string()));
		}
		std::println("");
		std::println("{} written ({} bytes), {} not. the SHA-256 is of what was written: check anything that isn't intact before trusting it", written, bytes, failed);
		return failed == 0 ? 0 : 1;
	}

	int recover(const std::filesystem::path& path, const std::filesystem::path& destination, std::optional<std::uint64_t> partition, const std::vector<std::uint64_t>& ids) {
		auto image = open(path);
		if (!image)
			return 1;
		auto volume = find_volume(*image, partition);
		if (!volume)
			return 1;
		auto scan = scan_volume(*volume);
		if (!scan)
			return 1;

		std::vector<const storage::RecoveryCandidate*> chosen;
		if (ids.empty()) {
			for (const storage::RecoveryCandidate& c : scan->candidates)
				if (c.deleted && !c.directory && c.integrity != storage::Integrity::unrecoverable)
					chosen.push_back(&c);
		} else {
			for (std::uint64_t id : ids) {
				if (id >= scan->candidates.size()) {
					std::println(stderr, "error: there's no entry {}, `fenrir-cli files` lists them", id);
					return 2;
				}
				chosen.push_back(&scan->candidates[static_cast<std::size_t>(id)]);
			}
		}
		return export_all(*volume->source, chosen, destination);
	}

	// a carve of the whole image or one partition, without what a filesystem scan of the same area finds: those come back
	// with their names from `files`
	struct t_carving {
		std::unique_ptr<storage::PartitionView> partition; // null: the whole image was carved
		storage::IBlockSource* source = nullptr;
		storage::CarveScan scan;
		std::size_t dropped = 0;
		std::vector<std::string> volumes; // the volumes whose files were left out
	};

	std::optional<t_carving> carve_image(storage::FileImageSource& image, std::optional<std::uint64_t> number, bool every_byte) {
		auto map = storage::scan_partitions(image);
		if (!map) {
			print_error(map.error());
			return std::nullopt;
		}
		struct t_known { // a volume fenrir can read the records of in the carved area, from base on
			storage::IBlockSource* source;
			std::unique_ptr<storage::PartitionView> view;
			std::uint64_t base;
			std::string where;
			storage::FileSystem filesystem;
		};
		std::vector<t_known> volumes;
		t_carving out;
		std::string where;
		if (number) {
			const auto p = std::ranges::find(map->partitions, *number, &storage::Partition::number);
			if (p == map->partitions.end()) {
				std::println(stderr, "error: there's no partition {}, `fenrir-cli partitions` lists them", *number);
				return std::nullopt;
			}
			auto view = storage::open_partition(image, *p);
			if (!view) {
				print_error(view.error());
				return std::nullopt;
			}
			out.partition = std::move(*view);
			out.source = out.partition.get();
			where = std::format("partition {}", *number);
			if (storage::can_scan(p->filesystem))
				volumes.push_back({ out.source, nullptr, 0, where, p->filesystem });
		} else {
			out.source = &image;
			where = "the whole image";
			for (const storage::Partition& p : map->partitions)
				if (storage::can_scan(p.filesystem) && p.inside_disk)
					if (auto view = storage::open_partition(image, p))
						volumes.push_back({ view->get(), std::move(*view), p.offset_bytes, std::format("partition {}", p.number), p.filesystem });
			if (map->partitions.empty() && storage::can_scan(map->whole_disk))
				volumes.push_back({ &image, nullptr, 0, "the image's own", map->whole_disk });
		}

		const std::uint64_t size = out.source->size_bytes();
		std::println("carving: {}, {} bytes ({}), {}", where, size, human_size(size), every_byte ? "a file can start at any byte" : "files start at a sector (every 512th byte)");
		{
			t_percent percent("carving");
			auto scan = storage::carve(*out.source, { .alignment = every_byte ? 1u : 512u }, nullptr, percent.sink());
			if (!scan) {
				print_error(scan.error());
				return std::nullopt;
			}
			out.scan = std::move(*scan);
		}
		for (const t_known& v : volumes) {
			auto known = storage::scan_volume(*v.source, v.filesystem);
			if (!known)
				continue; // it only looked like one, there's nothing to leave out
			out.dropped += storage::drop_known(out.scan.candidates, known->candidates, v.base);
			out.volumes.push_back(v.where);
		}
		return out;
	}

	int carve(const std::filesystem::path& path, std::optional<std::uint64_t> partition, bool every_byte, bool why) {
		auto image = open(path);
		if (!image)
			return 1;
		const auto carving = carve_image(*image, partition, every_byte);
		if (!carving)
			return 1;
		const storage::CarveScan& s = carving->scan;

		std::println("");
		std::println("    id  status                size  format        offset  name");
		for (const storage::RecoveryCandidate& c : s.candidates) {
			std::println("  {:>4}  {:<13} {:>12}  {:<6} {:>12}  {}", c.id, storage::to_string(c.integrity), c.size, c.format, c.entry_offset, c.name);
			if (why)
				for (const std::string& reason : c.reasons)
					std::println("        - {}", reason);
		}
		std::println("");
		std::print("{} files", s.candidates.size());
		if (carving->dropped != 0) {
			std::string volumes;
			for (const std::string& v : carving->volumes)
				volumes += (volumes.empty() ? "" : ", ") + v;
			std::print(", and {} more that the filesystem scan of {} finds with their names (`files --all` lists them)", carving->dropped, volumes);
		}
		std::println("");
		if (s.rejected != 0)
			std::println("{} didn't hold up as files and {} left out", s.rejected == 1 ? "1 signature" : std::format("{} signatures", s.rejected), s.rejected == 1 ? "was" : "were");
		print_diagnostics(s.diagnostics);
		return 0;
	}

	int recover_carved(const std::filesystem::path& path, const std::filesystem::path& destination, std::optional<std::uint64_t> partition, bool every_byte,
		const std::vector<std::uint64_t>& ids) {
		auto image = open(path);
		if (!image)
			return 1;
		const auto carving = carve_image(*image, partition, every_byte);
		if (!carving)
			return 1;
		const std::vector<storage::RecoveryCandidate>& found = carving->scan.candidates;
		std::vector<const storage::RecoveryCandidate*> chosen;
		for (const storage::RecoveryCandidate& c : found)
			if (ids.empty())
				chosen.push_back(&c);
		for (std::uint64_t id : ids) {
			if (id >= found.size()) {
				std::println(stderr, "error: there's no carved file {}, `fenrir-cli carve` lists them", id);
				return 2;
			}
			chosen.push_back(&found[static_cast<std::size_t>(id)]);
		}
		return export_all(*carving->source, chosen, destination);
	}

	struct t_args {
		std::vector<std::filesystem::path> positional;
		std::optional<std::uint64_t> partition, sector_size;
		std::vector<std::uint64_t> ids;
		bool all = false, why = false, carved = false, every_byte = false, bad = false;
	};

	t_args parse(const std::vector<std::filesystem::path>& args) {
		t_args out;
		for (std::size_t i = 1; i < args.size(); i++) {
			const std::filesystem::path& a = args[i];
			if (a == "--partition" || a == "--sector-size" || a == "--id") {
				const std::optional<std::uint64_t> v = i + 1 < args.size() ? number(args[++i]) : std::nullopt;
				if (!v)
					out.bad = true;
				else if (a == "--partition")
					out.partition = v;
				else if (a == "--sector-size")
					out.sector_size = v;
				else
					out.ids.push_back(*v);
			} else if (a == "--all") {
				out.all = true;
			} else if (a == "--carved") {
				out.carved = true;
			} else if (a == "--every-byte") {
				out.every_byte = true;
			} else if (a == "--why") {
				out.why = true;
			} else if (a.native().starts_with(std::filesystem::path("--").native())) {
				out.bad = true;
			} else {
				out.positional.push_back(a);
			}
		}
		return out;
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
		const t_args a = parse(args);
		const std::size_t n = a.positional.size();
		if (a.bad) {
			usage();
			return 2;
		}
		if (command == "info" && n == 1)
			return info(a.positional[0]);
		if (command == "hash" && n == 1)
			return hash(a.positional[0]);
		if (command == "partitions" && n == 1 && a.sector_size.value_or(0) <= 65536)
			return partitions(a.positional[0], static_cast<std::uint32_t>(a.sector_size.value_or(0)));
		if (command == "hex" && n >= 1 && n <= 3) {
			const auto offset = n >= 2 ? number(a.positional[1]) : std::optional<std::uint64_t>(0);
			const auto length = n >= 3 ? number(a.positional[2]) : std::optional<std::uint64_t>(512);
			if (offset && length)
				return hex(a.positional[0], *offset, *length);
		}
		if (command == "files" && n == 1)
			return files(a.positional[0], a.partition, a.all, a.why);
		if (command == "carve" && n == 1)
			return carve(a.positional[0], a.partition, a.every_byte, a.why);
		if (command == "recover" && n == 2 && a.carved)
			return recover_carved(a.positional[0], a.positional[1], a.partition, a.every_byte, a.ids);
		if (command == "recover" && n == 2)
			return recover(a.positional[0], a.positional[1], a.partition, a.ids);
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
