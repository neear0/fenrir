// fenrir-cli: the debug and test interface to the storage engine. it opens disk images read-only and says what's in
// them. the files it writes are new ones, recovered files (recover) and a copy of a disk with its map (image), and a
// disposable test image it's told to erase (erase run, after a typed confirmation). nothing it does can change a source.
// the one way it can write to a drive is erase run on a USB drive, in a build configured with FENRIR_DRIVE_ERASE, as
// administrator, after a typed confirmation: see src/platform/windows/drive_erase.cpp

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <io.h>
#else
#include <csignal>
#include <unistd.h>
#endif

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "storage/carving.h"
#include "storage/disks.h"
#include "storage/drive_erase.h"
#include "storage/exporter.h"
#include "storage/filesystem.h"
#include "storage/file_image_source.h"
#include "storage/hash.h"
#include "storage/hex_dump.h"
#include "storage/image_erase.h"
#include "storage/mapped_image.h"
#include "storage/partition.h"
#include "storage/sanitize.h"
#include "storage/simulated_drive.h"

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
			"                                       the deleted files on a FAT32, exFAT or NTFS volume, how\n"
			"                                       recoverable each one is and (--why) what that's based on.\n"
			"                                       --all lists the live ones too\n"
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
			"  image <source> <image> [--map file] [--retries N] [--no-trim] [--no-scrape] [--max-errors N]\n"
			"        [--sector-size N] [--cluster-size N]\n"
			"                                       copy source into a new image file, with a map (ddrescue's mapfile,\n"
			"                                       image.map unless --map says) of what was copied and what couldn't\n"
			"                                       be read. what reads easily first, then closer and closer around\n"
			"                                       what didn't. ctrl+c stops it with the map saved: the same command\n"
			"                                       goes on from there, as it does from a map ddrescue made. the other\n"
			"                                       commands read an image together with its map\n"
			"  map <file>                           what a map says was copied, and where the bad areas are\n"
			"  drives                               the physical drives, their volumes, and which has Windows on it\n"
			"  erase plan <target> [--method M] [--random] [--seed N] [--no-verify]\n"
			"                                       what erasing would do, what it may be called at most, and what\n"
			"                                       it can't promise. nothing is done. <target> is a simulated drive\n"
			"                                       (sim:hdd, sim:sata-ssd, sim:nvme-ssd, sim:usb-flash) or a disposable\n"
			"                                       test image file, or a USB drive, \\\\.\\PhysicalDriveN: planning\n"
			"                                       works in any build, running only in one built with\n"
			"                                       FENRIR_DRIVE_ERASE, as administrator. other drives are refused\n"
			"  erase run <target> --confirm \"ERASE ...\" [the same] [--report dir] [--fault F]...\n"
			"                                       do it, after typing the confirmation the plan asks for. --report\n"
			"                                       writes a JSON report. simulated faults: fail-at=0.5,\n"
			"                                       power-loss-at=0.3, lies, unreadable\n"
			"\n"
			"  any <image> can be a drive, \\\\.\\PhysicalDriveN (see drives), opened read-only: that needs\n"
			"  administrator rights. nothing is ever written to a drive being read, image and recover refuse a\n"
			"  destination on it.\n"
			"  --size N                             read only the first N bytes of the image or drive\n"
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

	// an image, read through its map when there's one next to it (what `image` makes)
	// --size N: only the first N bytes of the source are read, by every command
	std::optional<std::uint64_t> source_limit;

	// an image, read through its map when there's one next to it (what `image` makes), or a drive (\.PhysicalDriveN)
	struct t_image {
		std::unique_ptr<storage::IBlockSource> source;
		std::unique_ptr<storage::IBlockSource> limited; // the first --size bytes of source
		std::shared_ptr<const storage::ImageMap> map;
		std::filesystem::path mapfile;
		explicit operator bool() const { return source != nullptr; }
		storage::IBlockSource& operator*() const { return limited ? *limited : *source; }
		storage::IBlockSource* operator->() const { return limited ? limited.get() : source.get(); }
	};

	t_image open(const std::filesystem::path& path) {
		auto opened = storage::open_source(path);
		if (!opened) {
			print_error(opened.error());
			return {};
		}
		if (!opened->note.empty())
			std::println(stderr, "note: {}", opened->note);
		if (const storage::ImageMap* map = opened->map.get(); map && map->bytes(storage::BlockStatus::finished) < map->size())
			std::println(stderr, "note: read with its map {}: {} of its bytes weren't read when the disk was imaged, and aren't taken for data",
				utf8(opened->mapfile.filename().u8string()), map->size() - map->bytes(storage::BlockStatus::finished));
		t_image out{ std::move(opened->source), nullptr, std::move(opened->map), std::move(opened->mapfile) };
		if (source_limit && *source_limit < out.source->size_bytes()) {
			auto view = storage::PartitionView::open(*out.source, 0, *source_limit);
			if (!view) {
				print_error(view.error());
				return {};
			}
			out.limited = std::move(*view);
		}
		if (out.source->physical_disk())
			if (const auto* disk = dynamic_cast<const storage::DiskSource*>(out.source.get()))
				std::println(stderr, "note: reading drive {} ({}{}{}), read-only", disk->info().number, disk->info().model, disk->info().serial.empty() ? "" : ", serial ", disk->info().serial);
		return out;
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
		if (const auto* disk = dynamic_cast<const storage::DiskSource*>(image.source.get())) {
			const storage::DiskInfo& d = disk->info();
			std::println("drive:   {}, {}{}, serial {}", d.vendor.empty() ? d.model : d.vendor + " " + d.model, d.bus, d.removable ? ", removable" : "", d.serial.empty() ? "(none given)" : d.serial);
			std::println("access:  read-only, the drive is read as it is while windows keeps using it");
		}
#ifdef _WIN32
		else
			std::println("access:  read-only, other programs can't open it for writing while it's open");
#else
		std::println("access:  read-only");
#endif
		if (const storage::ImageMap* map = image.map.get()) {
			std::println("map:     {}, {} of it copied, {} bad, {} not read yet", utf8(image.mapfile.u8string()), human_size(map->bytes(storage::BlockStatus::finished)),
				human_size(map->bytes(storage::BlockStatus::bad)), human_size(map->unknown_bytes()));
			std::println("         only what the map counts as copied is read, the rest fails as unreadable");
		}
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
		// an image file as it is, map or not; a drive (or its first --size bytes) as it reads
		std::unique_ptr<storage::IBlockSource> file;
		t_image drive;
		if (storage::physical_drive_number(path)) {
			drive = open(path);
			if (!drive)
				return 1;
		} else if (auto opened = storage::FileImageSource::open(path)) {
			file = std::move(*opened);
		} else {
			print_error(opened.error());
			return 1;
		}
		storage::IBlockSource* image = file ? file.get() : &*drive;
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
		std::uint64_t offset = 0;                 // in the image
		const storage::ImageMap* map = nullptr;   // the image's, when it has one
	};

	std::optional<t_volume> find_volume(const t_image& opened, std::optional<std::uint64_t> number) {
		storage::IBlockSource& image = *opened;
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
			volume.offset = chosen->offset_bytes;
		} else if (!number && map->partitions.empty()) {
			volume.source = &image;
			volume.where = "the whole image";
			volume.filesystem = map->whole_disk;
		} else {
			if (number)
				std::println(stderr, "error: there's no partition {}, `fenrir-cli partitions` lists them", *number);
			else
				std::println(stderr, "error: no partition looks like a FAT32, exFAT or NTFS one, pick one with --partition N");
			return std::nullopt;
		}
		volume.map = opened.map.get();
		return volume;
	}

	std::optional<storage::VolumeScan> scan_volume(t_volume& volume) {
		if (!storage::can_scan(volume.filesystem)) {
			std::println(stderr, "error: {} looks like {}, fenrir can't read its records (FAT32, exFAT and NTFS only so far)", volume.where, storage::to_string(volume.filesystem));
			return std::nullopt;
		}
		auto scanned = storage::scan_volume(*volume.source, volume.filesystem);
		if (!scanned) {
			print_error(scanned.error());
			return std::nullopt;
		}
		if (volume.map)
			for (storage::RecoveryCandidate& c : scanned->candidates)
				storage::mark_unread(c, *volume.map, volume.offset);
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
		auto volume = find_volume(image, partition);
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
		auto volume = find_volume(image, partition);
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

	std::optional<t_carving> carve_image(storage::IBlockSource& image, std::optional<std::uint64_t> number, bool every_byte) {
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

	// ctrl+c while imaging: the copy stops at the next read and saves its map, instead of the process just ending
	storage::CancellationToken* interrupt_target = nullptr;

#ifdef _WIN32
	BOOL WINAPI on_interrupt(DWORD kind) {
		if ((kind == CTRL_C_EVENT || kind == CTRL_BREAK_EVENT) && interrupt_target) {
			interrupt_target->cancel();
			return TRUE;
		}
		return FALSE;
	}
#else
	extern "C" void on_interrupt(int) {
		if (interrupt_target)
			interrupt_target->cancel();
	}
#endif

	std::string_view to_text(storage::ImagingPhase phase) {
		switch (phase) {
		case storage::ImagingPhase::copying: return "copying";
		case storage::ImagingPhase::trimming: return "trimming";
		case storage::ImagingPhase::scraping: return "scraping";
		case storage::ImagingPhase::retrying: return "retrying";
		case storage::ImagingPhase::finished: return "finished";
		}
		return "?";
	}

	// one line on stderr, rewritten a few times a second, when it's a terminal
	class t_imaging_progress final : public storage::ImagingObserver {
	public:
		void update(const storage::ImagingStatus& s, const storage::ImageMap&) noexcept override {
			const auto now = std::chrono::steady_clock::now();
			if (!shown_ || now - last_ < std::chrono::milliseconds(250))
				return;
			last_ = now;
			const std::uint64_t percent = s.size ? s.finished * 100 / s.size : 100;
			std::print(stderr, "\r{} pass {}: {}% copied, {} bad, {} not read yet, {} read errors   ", to_text(s.phase), s.pass, percent,
				human_size(s.bad), human_size(s.unknown), s.read_errors);
		}
		~t_imaging_progress() override {
			if (shown_)
				std::print(stderr, "\r{:79}\r", "");
		}

	private:
		bool shown_ = terminal(stderr);
		std::chrono::steady_clock::time_point last_{};
	};

	struct t_image_options {
		std::optional<std::filesystem::path> mapfile;
		storage::ImagingOptions imaging;
	};

	void print_map(const storage::ImageMap& map) {
		const std::uint64_t size = map.size();
		auto share = [size](std::uint64_t bytes) { return std::format("{:>14} bytes  {:>10}  {:5.1f}%", bytes, human_size(bytes), size ? bytes * 100.0 / static_cast<double>(size) : 0.0); };
		std::println("copied:      {}", share(map.bytes(storage::BlockStatus::finished)));
		std::println("bad:         {}", share(map.bytes(storage::BlockStatus::bad)));
		std::println("not read:    {}", share(map.unknown_bytes()));
		if (map.unknown_bytes() != 0)
			std::println("             ({} not tried, {} failed in a large read, {} between bad sectors)", human_size(map.bytes(storage::BlockStatus::non_tried)),
				human_size(map.bytes(storage::BlockStatus::non_trimmed)), human_size(map.bytes(storage::BlockStatus::non_scraped)));
	}

	int image(const std::filesystem::path& source_path, const std::filesystem::path& destination, const t_image_options& options) {
		auto source = open(source_path);
		if (!source)
			return 1;
		storage::ImagingOptions imaging = options.imaging;
		if (imaging.sector_size == 0) {
			const auto* disk = dynamic_cast<const storage::DiskSource*>(source.source.get());
			imaging.sector_size = disk ? disk->info().sector_size : 512;
		}
		const std::filesystem::path mapfile = options.mapfile.value_or(storage::default_map_path(destination));
		std::println("source:      {}, {} bytes ({})", utf8(source_path.u8string()), source->size_bytes(), human_size(source->size_bytes()));
		std::println("image:       {}", utf8(destination.u8string()));
		std::println("map:         {}", utf8(mapfile.u8string()));

		storage::CancellationToken cancel;
		interrupt_target = &cancel;
#ifdef _WIN32
		SetConsoleCtrlHandler(on_interrupt, TRUE);
#else
		std::signal(SIGINT, on_interrupt);
#endif
		storage::Result<storage::ImagingStatus> status;
		{
			t_imaging_progress progress;
			status = storage::acquire_image(*source, destination, mapfile, imaging, &cancel, &progress);
		}
#ifdef _WIN32
		SetConsoleCtrlHandler(on_interrupt, FALSE);
#else
		std::signal(SIGINT, SIG_DFL);
#endif
		interrupt_target = nullptr;

		if (status && status->resumed)
			std::println("             (went on from the copy it describes)");
		if (auto map = storage::load_map(mapfile)) {
			std::println("");
			print_map(*map);
		}
		if (!status) {
			if (status.error().code == storage::Errc::cancelled) {
				std::println(stderr, "stopped. the map is saved: the same command goes on from where it stopped");
				return 1;
			}
			print_error(status.error());
			if (status.error().code == storage::Errc::exists)
				std::println(stderr, "nothing is ever written over: pick an image name that isn't taken, or keep its map next to it to go on with it");
			else if (status.error().code == storage::Errc::mismatch)
				std::println(stderr, "the image, its map and the source have to be the ones the copy started with");
			return 1;
		}
		std::println("");
		if (status->bad == 0 && status->unknown == 0)
			std::println("done, all of it copied");
		else
			std::println("done. what wasn't copied is zeros in the image, and only its map says so: keep the map next to it, fenrir reads the two together");
		return 0;
	}

	int drives() {
		auto disks = storage::list_disks();
		if (!disks) {
			print_error(disks.error());
			return 1;
		}
		std::println("  drive  size          bus        model");
		for (const storage::DiskInfo& d : *disks) {
			std::string model = d.vendor.empty() ? d.model : d.vendor + " " + d.model;
			if (d.removable)
				model += ", removable";
			if (d.system)
				model += ", has Windows on it";
			std::println("  {:>5}  {:<12}  {:<9}  {}", d.number, d.size ? human_size(d.size) : "-", d.bus, model);
			std::println("         {}{}", utf8(d.path.u8string()), d.serial.empty() ? "" : ", serial " + d.serial);
			for (const std::string& v : d.volumes)
				std::println("         volume {}", v);
			if (!d.error.empty())
				std::println("         {}", d.error);
		}
		std::println("");
		std::println("listed without administrator rights. reading one (image, info, partitions...) needs them, and only ever reads");
		return 0;
	}

	int show_map(const std::filesystem::path& mapfile) {
		auto map = storage::load_map(mapfile);
		if (!map) {
			print_error(map.error());
			return 1;
		}
		std::println("map:         {}", utf8(mapfile.u8string()));
		std::println("disk:        {} bytes ({})", map->size(), human_size(map->size()));
		std::println("state:       {}{}, at byte {}", to_text(map->phase), map->phase == storage::ImagingPhase::finished ? "" : std::format(" pass {}", map->pass), map->current_pos);
		print_map(*map);
		std::size_t shown = 0, bad = 0;
		for (const storage::MapBlock& b : map->blocks())
			if (b.status == storage::BlockStatus::bad) {
				if (bad++ == 0) {
					std::println("");
					std::println("bad areas:           offset            bytes");
				}
				if (shown < 50) {
					std::println("             {:>16}  {:>15}", b.pos, b.size);
					shown++;
				}
			}
		if (bad > shown)
			std::println("             and {} more", bad - shown);
		return 0;
	}

	struct t_args {
		std::vector<std::filesystem::path> positional;
		std::optional<std::uint64_t> partition, sector_size;
		std::vector<std::uint64_t> ids;
		bool all = false, why = false, carved = false, every_byte = false, bad = false;
		std::optional<std::filesystem::path> map;
		std::optional<std::uint64_t> cluster_size, retries, max_errors;
		std::optional<std::uint64_t> size;
		bool no_trim = false, no_scrape = false;
		std::optional<std::filesystem::path> method, confirm, report; // erase
		std::vector<std::string> faults;
		std::optional<std::uint64_t> seed;
		bool random_pattern = false, no_verify = false;
	};

	t_args parse(const std::vector<std::filesystem::path>& args) {
		t_args out;
		for (std::size_t i = 1; i < args.size(); i++) {
			const std::filesystem::path& a = args[i];
			if (a == "--partition" || a == "--sector-size" || a == "--id" || a == "--cluster-size" || a == "--retries" || a == "--max-errors" || a == "--size") {
				const std::optional<std::uint64_t> v = i + 1 < args.size() ? number(args[++i]) : std::nullopt;
				if (!v)
					out.bad = true;
				else if (a == "--partition")
					out.partition = v;
				else if (a == "--sector-size")
					out.sector_size = v;
				else if (a == "--cluster-size")
					out.cluster_size = v;
				else if (a == "--retries")
					out.retries = v;
				else if (a == "--max-errors")
					out.max_errors = v;
				else if (a == "--size")
					out.size = v;
				else
					out.ids.push_back(*v);
			} else if (a == "--method" || a == "--confirm" || a == "--report" || a == "--fault") {
				if (i + 1 >= args.size()) {
					out.bad = true;
					continue;
				}
				const std::filesystem::path& v = args[++i];
				if (a == "--method")
					out.method = v;
				else if (a == "--confirm")
					out.confirm = v;
				else if (a == "--report")
					out.report = v;
				else
					out.faults.emplace_back(utf8(v.u8string()));
			} else if (a == "--seed") {
				out.seed = i + 1 < args.size() ? number(args[++i]) : std::nullopt;
				out.random_pattern = true;
				out.bad |= !out.seed;
			} else if (a == "--random") {
				out.random_pattern = true;
			} else if (a == "--no-verify") {
				out.no_verify = true;
			} else if (a == "--map") {
				if (i + 1 < args.size())
					out.map = args[++i];
				else
					out.bad = true;
			} else if (a == "--no-trim") {
				out.no_trim = true;
			} else if (a == "--no-scrape") {
				out.no_scrape = true;
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

	// erase plan|run: a simulated drive (sim:hdd, sim:sata-ssd, sim:nvme-ssd, sim:usb-flash), a disposable test image file, or
	// a USB drive (\\.\PhysicalDriveN: planned in any build, run only in a FENRIR_DRIVE_ERASE build, as administrator)
	int erase(bool run, const std::filesystem::path& target_path, const t_args& a) {
		const std::string target_text(utf8(target_path.u8string()));
		const std::optional<std::uint32_t> drive_number = storage::physical_drive_number(target_path);
		if (!drive_number && storage::is_device_path(target_path)) {
			std::println(stderr, "error: only a whole drive, \\\\.\\PhysicalDriveN, can be erased: not a volume or another device");
			return 1;
		}

		std::optional<storage::SimulatedDrive> simulated;
		std::optional<storage::TestImageBackend> image;
		std::optional<storage::DriveOverwriteBackend> drive;
		storage::ISanitizationBackend* backend = nullptr;
		if (drive_number) {
			if (!a.faults.empty()) {
				std::println(stderr, "error: faults are only for simulated drives");
				return 2;
			}
			std::vector<std::filesystem::path> keep;
			if (a.report)
				keep.push_back(*a.report); // the report has to land on a drive that survives
			drive.emplace(*drive_number, std::move(keep));
			backend = &*drive;
		} else if (target_text.starts_with("sim:")) {
			storage::SimulatedFaults faults;
			for (const std::string& f : a.faults) {
				const auto value = [&](std::string_view key) -> std::optional<double> {
					if (!f.starts_with(key))
						return std::nullopt;
					const std::string text = f.substr(key.size());
					double v = 0;
					const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), v);
					return error == std::errc() && end == text.data() + text.size() && v >= 0 && v <= 1 ? std::optional(v) : std::nullopt;
				};
				if (const auto v = value("fail-at="))
					faults.fail_at = v;
				else if (const auto p = value("power-loss-at="))
					faults.power_loss_at = p;
				else if (f == "lies")
					faults.lies = true;
				else if (f == "unreadable")
					faults.unreadable_after = true;
				else {
					std::println(stderr, "error: no such fault: {} (fail-at=F, power-loss-at=F, lies, unreadable)", f);
					return 2;
				}
			}
			simulated = storage::SimulatedDrive::make(target_text.substr(4), faults);
			if (!simulated) {
				std::println(stderr, "error: no simulated drive called {} (sim:hdd, sim:sata-ssd, sim:nvme-ssd, sim:usb-flash)", target_text);
				return 2;
			}
			backend = &*simulated;
		} else {
			if (!a.faults.empty()) {
				std::println(stderr, "error: faults are only for simulated drives");
				return 2;
			}
			image.emplace(target_path);
			backend = &*image;
		}

		auto target = backend->inspect();
		if (!target) {
			print_error(target.error());
			return 1;
		}
		storage::SanitizeRequest request;
		if (a.method) {
			const auto method = storage::method_from(utf8(a.method->u8string()));
			if (!method) {
				std::println(stderr, "error: no method called {}", utf8(a.method->u8string()));
				return 2;
			}
			request.method = *method;
		}
		request.zeros = !a.random_pattern;
		request.seed = a.seed.value_or(0x5EED);
		request.verify = !a.no_verify;

		const storage::TargetIdentity& id = target->identity;
		std::println("target:      {}{}", target_text, simulated ? "  (simulated, in memory: no hardware is touched)" : drive ? "  (a REAL DRIVE: everything on it goes)" : "  (a test image file)");
		std::println("media:       {}, {} bytes ({})", storage::to_string(id.media), id.size, human_size(id.size));
		if (!id.model.empty())
			std::println("drive:       {}, serial {}", id.model, id.serial);
		std::string supports;
		for (storage::Method m : target->capabilities.methods)
			supports += (supports.empty() ? "" : ", ") + std::string(storage::to_string(m));
		std::println("supports:    {}", supports);
		std::println("method:      {}", storage::to_string(request.method));

		const auto planned = storage::plan_sanitize(*target, request);
		if (const auto* refusal = std::get_if<storage::PlanRefusal>(&planned)) {
			std::println("");
			std::println("refused, nothing was done:");
			for (const std::string& r : refusal->reasons)
				std::println("  - {}", r);
			return 1;
		}
		const storage::SanitizePlan& plan = std::get<storage::SanitizePlan>(planned);
		std::println("at most:     {}", storage::to_string(plan.category));
		std::println("scope:       {}", plan.scope);
		std::println("can stop:    {}", plan.cancellable ? "yes, partway (what's done stays done)" : "no, not once it's accepted");
		std::println("");
		std::println("steps:");
		for (const std::string& s : plan.steps)
			std::println("  - {}", s);
		std::println("can't promise:");
		for (const std::string& c : plan.caveats)
			std::println("  - {}", c);

		if (!run) {
			std::println("");
			std::println("nothing was done. to do it: fenrir-cli erase run {} ... --confirm \"{}\"", target_text, plan.confirmation);
			return 0;
		}
		if (!a.confirm) {
			std::println(stderr, "");
			std::println(stderr, "not done: it needs --confirm \"{}\", typed out", plan.confirmation);
			return 1;
		}
		auto approved = storage::authorize(plan, backend->inspect().value_or(storage::Target{}).identity, utf8(a.confirm->u8string()));
		if (!approved) {
			print_error(approved.error());
			return 1;
		}

		storage::CancellationToken cancel;
		interrupt_target = &cancel;
#ifdef _WIN32
		SetConsoleCtrlHandler(on_interrupt, TRUE);
#else
		std::signal(SIGINT, on_interrupt);
#endif
		storage::Result<storage::SanitizeReport> report;
		{
			t_percent percent("erasing");
			report = backend->execute(*approved, &cancel, percent.sink());
		}
#ifdef _WIN32
		SetConsoleCtrlHandler(on_interrupt, FALSE);
#else
		std::signal(SIGINT, SIG_DFL);
#endif
		interrupt_target = nullptr;
		if (!report) {
			print_error(report.error());
			std::println(stderr, "nothing was written to it");
			return 1;
		}

		std::println("");
		std::println("status:      {}", storage::to_string(report->status));
		std::println("done:        {} bytes", report->bytes_done);
		for (const std::string& e : report->events)
			std::println("  - {}", e);
		for (const std::string& e : report->errors)
			std::println("  ! {}", e);
		std::println("checked:     {}", report->verification.bytes_checked != 0 ? std::format("{} bytes{}, {} sectors wrong", report->verification.bytes_checked, report->verification.done ? "" : " (not all of it)", report->verification.mismatches) : std::string("not read back"));
		std::println("             {}", report->verification.note);
		std::println("validation:  {}", report->validation);
		if (a.report) {
			auto written = storage::write_report(*a.report, "fenrir erase report.json", storage::to_json(*report));
			if (!written) {
				print_error(written.error());
				return 1;
			}
			std::println("report:      {}", utf8(written->u8string()));
		}
		return report->status == storage::SanitizeStatus::verified || report->status == storage::SanitizeStatus::completed ? 0 : 1;
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
		source_limit = a.size;
		if (command == "drives" && n == 0)
			return drives();
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
		if (command == "image" && n == 2 && a.sector_size.value_or(0) <= 65536 && a.cluster_size.value_or(0) <= (64u << 20) && a.retries.value_or(0) <= 1000) {
			t_image_options o;
			o.mapfile = a.map;
			o.imaging.sector_size = static_cast<std::uint32_t>(a.sector_size.value_or(0)); // 0: the drive's own, 512 for an image
			o.imaging.cluster_size = static_cast<std::uint32_t>(a.cluster_size.value_or(65536));
			o.imaging.retries = static_cast<unsigned>(a.retries.value_or(0));
			o.imaging.max_read_errors = a.max_errors.value_or(0);
			o.imaging.trim = !a.no_trim;
			o.imaging.scrape = !a.no_scrape;
			return image(a.positional[0], a.positional[1], o);
		}
		if (command == "map" && n == 1)
			return show_map(a.positional[0]);
		if (command == "erase" && n == 2 && (a.positional[0] == "plan" || a.positional[0] == "run"))
			return erase(a.positional[0] == "run", a.positional[1], a);
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
