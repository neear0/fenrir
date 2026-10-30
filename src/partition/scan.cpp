#include <algorithm>
#include <format>
#include <new>
#include <optional>
#include <unordered_set>

#include "storage/bytes.h"
#include "storage/hash.h"
#include "storage/partition.h"

// mbr: the layout every partitioning tool uses (boot code, 4 entries at 0x1BE, 0x55AA). extended partitions: a chain of
// extended boot records, each with one logical partition relative to itself and a link relative to the extended
// partition's start. gpt: uefi specification 2.10, section 5.3 (header, entry array, backup at the end of the disk)

namespace {
	using namespace storage;
	using Severity = Diagnostic::Severity;

	constexpr std::size_t max_diagnostics = 200;   // a table of garbage could otherwise produce one per entry
	constexpr std::uint32_t max_logical = 128;     // linux stops well before this
	constexpr std::uint64_t max_gpt_array = 16u << 20;
	constexpr std::size_t max_sniffed = 256;       // partitions whose first bytes are read for a filesystem signature
	constexpr std::size_t sniff_bytes = 4096;

	bool extended_type(std::uint8_t type) { return type == 0x05 || type == 0x0F || type == 0x85; }

	struct t_mbr_entry {
		std::uint8_t status = 0, type = 0;
		std::uint32_t first = 0, count = 0;
	};

	t_mbr_entry mbr_entry(ByteReader& r, int index) {
		const std::size_t at = 0x1BE + 16 * static_cast<std::size_t>(index);
		return { r.u8(at), r.u8(at + 4), r.le32(at + 8), r.le32(at + 12) };
	}

	struct t_mbr {
		bool valid = false; // the boot signature, and every entry's status byte 0x00 or 0x80
		bool protective = false;
		int used = 0;       // non-empty entries, the protective one not counted
		std::uint32_t signature = 0;
		std::array<t_mbr_entry, 4> entries{};
	};

	t_mbr parse_mbr(std::span<const std::byte> sector) {
		ByteReader r(sector);
		t_mbr mbr;
		if (r.le16(510) != 0xAA55)
			return mbr;
		mbr.valid = true;
		mbr.signature = r.le32(0x1B8);
		for (int i = 0; i < 4; i++) {
			mbr.entries[i] = mbr_entry(r, i);
			const t_mbr_entry& e = mbr.entries[i];
			if (e.status != 0x00 && e.status != 0x80)
				mbr.valid = false;
			if (e.type == 0xEE)
				mbr.protective = true;
			else if (e.type != 0)
				mbr.used++;
		}
		return mbr;
	}

	struct t_gpt {
		std::uint32_t revision = 0;
		std::uint64_t my_lba = 0, alternate_lba = 0, first_usable = 0, last_usable = 0, entries_lba = 0;
		std::uint32_t entry_count = 0, entry_size = 0, entries_crc = 0;
		Guid disk_guid;
		std::vector<std::byte> entries; // the whole array, its crc checked
	};

	// utf-16le up to the first 0, as utf-8. unpaired surrogates and control characters become U+FFFD: names are printed to
	// terminals and the ui, and an escape sequence in one shouldn't do anything there
	std::string utf8_name(std::span<const std::byte> utf16) {
		ByteReader r(utf16);
		std::string out;
		auto put = [&out](char32_t c) {
			if (c < 0x20 || (c >= 0x7F && c < 0xA0))
				c = 0xFFFD;
			if (c < 0x80) {
				out += static_cast<char>(c);
			} else if (c < 0x800) {
				out += static_cast<char>(0xC0 | (c >> 6));
				out += static_cast<char>(0x80 | (c & 0x3F));
			} else if (c < 0x10000) {
				out += static_cast<char>(0xE0 | (c >> 12));
				out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (c & 0x3F));
			} else {
				out += static_cast<char>(0xF0 | (c >> 18));
				out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
				out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (c & 0x3F));
			}
		};
		for (std::size_t i = 0; i + 1 < r.size(); i += 2) {
			const char32_t unit = r.le16(i);
			if (unit == 0)
				break;
			if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < r.size()) {
				const char32_t low = r.le16(i + 2);
				if (low >= 0xDC00 && low <= 0xDFFF) {
					put(0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
					i += 2;
					continue;
				}
			}
			put(unit >= 0xD800 && unit <= 0xDFFF ? char32_t(0xFFFD) : unit);
		}
		return out;
	}

	class t_scan {
	public:
		t_scan(IBlockSource& source, PartitionMap& map) : source_(source), map_(map), size_(source.size_bytes()) {}

		void note(Severity severity, std::string message) {
			if (map_.diagnostics.size() < max_diagnostics)
				map_.diagnostics.push_back({ severity, std::move(message) });
			else if (map_.diagnostics.size() == max_diagnostics)
				map_.diagnostics.push_back({ Severity::warning, "more problems were found than are listed here" });
		}

		std::uint64_t size() const { return size_; }
		std::uint32_t sector_size() const { return map_.sector_size; }
		std::uint64_t sectors() const { return map_.disk_sectors; }

		// length bytes at offset, which the caller has checked are inside the source. a failed read is passed on
		Result<std::vector<std::byte>> read(std::uint64_t offset, std::size_t length) {
			std::vector<std::byte> bytes(length);
			if (Result<void> done = read_exact(source_, offset, bytes); !done)
				return std::unexpected(done.error());
			return bytes;
		}

		Result<std::vector<std::byte>> read_sector(std::uint64_t lba) { return read(lba * sector_size(), sector_size()); }

		// the gpt header at lba and its entry array, or why there's no valid one there
		std::optional<t_gpt> gpt_at(std::uint64_t lba, std::string& why, bool& signature) {
			signature = false;
			if (lba >= sectors()) {
				why = std::format("it would be at sector {}, past the end of the image ({} sectors)", lba, sectors());
				return std::nullopt;
			}
			Result<std::vector<std::byte>> sector = read_sector(lba);
			if (!sector) {
				why = std::format("sector {} couldn't be read: {}", lba, to_string(sector.error().code));
				return std::nullopt;
			}
			ByteReader r(*sector);
			if (!r.matches(0, "EFI PART")) {
				why = std::format("sector {} has no GPT signature", lba);
				return std::nullopt;
			}
			signature = true;

			const std::uint32_t header_size = r.le32(12);
			if (header_size < 92 || header_size > sector_size()) {
				why = std::format("its header size ({} bytes) is out of range", header_size);
				return std::nullopt;
			}
			std::vector<std::byte> header(sector->begin(), sector->begin() + header_size);
			std::fill_n(header.begin() + 16, 4, std::byte{ 0 }); // the crc is computed with its own field zeroed
			const std::uint32_t stored_crc = r.le32(16), crc = crc32(header);
			if (stored_crc != crc) {
				why = std::format("its header CRC doesn't match (stored {:08X}, computed {:08X})", stored_crc, crc);
				return std::nullopt;
			}

			t_gpt gpt;
			gpt.revision = r.le32(8);
			gpt.my_lba = r.le64(24);
			gpt.alternate_lba = r.le64(32);
			gpt.first_usable = r.le64(40);
			gpt.last_usable = r.le64(48);
			std::copy_n(reinterpret_cast<const std::uint8_t*>(r.bytes(56, 16).data()), 16, gpt.disk_guid.bytes.begin());
			gpt.entries_lba = r.le64(72);
			gpt.entry_count = r.le32(80);
			gpt.entry_size = r.le32(84);
			gpt.entries_crc = r.le32(88);

			if (gpt.my_lba != lba) {
				why = std::format("it says it's at sector {}, but it was read from sector {}", gpt.my_lba, lba);
				return std::nullopt;
			}
			// a usable area past the end of the image is allowed: it's what a truncated image looks like, and its
			// partitions can still be listed, marked as not inside the disk
			if (gpt.first_usable > gpt.last_usable) {
				why = std::format("its usable area starts (sector {}) after it ends (sector {})", gpt.first_usable, gpt.last_usable);
				return std::nullopt;
			}
			// 128 x 2^n bytes per entry, uefi 5.3.2
			if (gpt.entry_size < 128 || gpt.entry_size > 65536 || (gpt.entry_size & (gpt.entry_size - 1)) != 0) {
				why = std::format("its entry size ({} bytes) isn't 128 times a power of 2", gpt.entry_size);
				return std::nullopt;
			}
			const std::uint64_t array_bytes = std::uint64_t(gpt.entry_count) * gpt.entry_size; // < 2^48, can't overflow
			if (array_bytes > max_gpt_array) {
				why = std::format("its entry array would be {} bytes, more than the {} fenrir reads", array_bytes, max_gpt_array);
				return std::nullopt;
			}
			const std::optional<std::uint64_t> array_offset = checked_mul(gpt.entries_lba, sector_size());
			const std::optional<std::uint64_t> array_end = array_offset ? checked_add(*array_offset, array_bytes) : std::nullopt;
			if (!array_end || *array_end > size()) {
				why = std::format("its entry array (at sector {}) runs past the end of the image", gpt.entries_lba);
				return std::nullopt;
			}
			Result<std::vector<std::byte>> array = read(*array_offset, static_cast<std::size_t>(array_bytes));
			if (!array) {
				why = std::format("its entry array couldn't be read: {}", to_string(array.error().code));
				return std::nullopt;
			}
			if (const std::uint32_t array_crc = crc32(*array); array_crc != gpt.entries_crc) {
				why = std::format("its entry array's CRC doesn't match (stored {:08X}, computed {:08X})", gpt.entries_crc, array_crc);
				return std::nullopt;
			}
			gpt.entries = std::move(*array);
			return gpt;
		}

		void add_mbr_partitions(const t_mbr& mbr) {
			std::optional<t_mbr_entry> extended;
			for (int i = 0; i < 4; i++) {
				const t_mbr_entry& e = mbr.entries[i];
				const int number = i + 1;
				if (e.type == 0)
					continue;
				if (extended_type(e.type)) {
					if (extended)
						note(Severity::warning, std::format("entry {} is a second extended partition, only the first one is followed", number));
					else
						extended = e;
					continue;
				}
				if (e.count == 0) {
					note(Severity::warning, std::format("entry {} has type {:02X} but no sectors, it's left out", number, e.type));
					continue;
				}
				add_mbr_partition(e, static_cast<std::uint32_t>(number), e.first, false);
			}
			if (extended)
				walk_extended(*extended);
		}

		void add_mbr_partition(const t_mbr_entry& e, std::uint32_t number, std::uint64_t first, bool logical) {
			if (first == 0)
				note(Severity::warning, std::format("partition {} starts at sector 0, on top of the partition table", number));
			Partition& p = map_.partitions.emplace_back();
			p.number = number;
			p.first_lba = first;
			p.sector_count = e.count;
			p.mbr_type = e.type;
			p.bootable = e.status == 0x80;
			p.logical = logical;
			p.type_name = mbr_type_name(e.type);
		}

		void walk_extended(const t_mbr_entry& extended) {
			const std::uint64_t start = extended.first, end = start + extended.count; // 32-bit values, no overflow
			if (extended.count == 0) {
				note(Severity::warning, "the extended partition has no sectors");
				return;
			}
			std::unordered_set<std::uint64_t> visited;
			std::uint32_t logical = 0;
			for (std::uint64_t ebr = start;;) {
				if (logical >= max_logical) {
					note(Severity::warning, std::format("the extended partition lists more than {} logical partitions, the rest are left out", max_logical));
					break;
				}
				if (!visited.insert(ebr).second) {
					note(Severity::warning, std::format("the extended partition's chain loops back to sector {}, it's followed no further", ebr));
					break;
				}
				if (ebr >= sectors()) {
					note(Severity::warning, std::format("an extended boot record would be at sector {}, past the end of the image", ebr));
					break;
				}
				Result<std::vector<std::byte>> sector = read_sector(ebr);
				if (!sector) {
					note(Severity::error, std::format("the extended boot record at sector {} couldn't be read: {}", ebr, to_string(sector.error().code)));
					break;
				}
				ByteReader r(*sector);
				if (r.le16(510) != 0xAA55) {
					note(Severity::warning, std::format("sector {} should hold an extended boot record but has no boot signature, the chain stops there", ebr));
					break;
				}
				const t_mbr_entry here = mbr_entry(r, 0), next = mbr_entry(r, 1);
				if (here.type != 0 && here.count != 0) {
					const std::uint32_t number = 5 + logical++;
					const std::uint64_t first = ebr + here.first; // < 2^33, no overflow
					add_mbr_partition(here, number, first, true);
					if (first < start || first + here.count > end)
						note(Severity::warning, std::format("logical partition {} isn't inside the extended partition (sectors {} to {})", number, start, end - 1));
				}
				if (next.type == 0 || next.first == 0)
					break;
				ebr = start + next.first;
			}
			note(Severity::info, std::format("the extended partition (sectors {} to {}) holds {} logical partition{}", start, end - 1, logical, logical == 1 ? "" : "s"));
		}

		void add_gpt_partitions(const t_gpt& gpt) {
			for (std::uint32_t i = 0; i < gpt.entry_count; i++) {
				ByteReader r(std::span(gpt.entries).subspan(std::size_t(i) * gpt.entry_size, gpt.entry_size));
				Guid type;
				std::copy_n(reinterpret_cast<const std::uint8_t*>(r.bytes(0, 16).data()), 16, type.bytes.begin());
				if (type.is_zero())
					continue; // an unused entry
				const std::uint32_t number = i + 1;
				const std::uint64_t first = r.le64(32), last = r.le64(40);
				if (last < first || last - first == UINT64_MAX) {
					note(Severity::warning, std::format("entry {} ends (sector {}) before it starts (sector {}), it's left out", number, last, first));
					continue;
				}
				if (first < gpt.first_usable || last > gpt.last_usable)
					note(Severity::warning, std::format("partition {} (sectors {} to {}) is outside the usable area (sectors {} to {})", number, first, last, gpt.first_usable, gpt.last_usable));

				Partition& p = map_.partitions.emplace_back();
				p.number = number;
				p.first_lba = first;
				p.sector_count = last - first + 1;
				p.type_guid = type;
				std::copy_n(reinterpret_cast<const std::uint8_t*>(r.bytes(16, 16).data()), 16, p.unique_guid.bytes.begin());
				p.attributes = r.le64(48);
				p.name = utf8_name(r.bytes(56, 72));
				p.type_name = gpt_type_name(type);
			}
		}

		// bytes, whether it fits, and what filesystem it starts with
		void finish_partitions() {
			std::size_t sniffed = 0;
			for (Partition& p : map_.partitions) {
				const std::optional<std::uint64_t> offset = checked_mul(p.first_lba, sector_size());
				const std::optional<std::uint64_t> length = checked_mul(p.sector_count, sector_size());
				const std::optional<std::uint64_t> end = offset && length ? checked_add(*offset, *length) : std::nullopt;
				p.offset_bytes = offset.value_or(0);
				p.size_bytes = length.value_or(0);
				p.inside_disk = end && *end <= size();
				if (!p.inside_disk) {
					note(Severity::warning, std::format("partition {} (sectors {} to {}) runs past the end of the image ({} sectors): the image may be truncated",
						p.number, p.first_lba, p.first_lba + (p.sector_count - 1), sectors()));
					continue;
				}
				if (sniffed++ >= max_sniffed)
					continue;
				Result<std::vector<std::byte>> first = read(p.offset_bytes, static_cast<std::size_t>(std::min<std::uint64_t>(sniff_bytes, p.size_bytes)));
				if (!first) {
					note(Severity::warning, std::format("the start of partition {} couldn't be read: {}", p.number, to_string(first.error().code)));
					continue;
				}
				p.filesystem = detect_filesystem(*first);
			}
			if (sniffed > max_sniffed)
				note(Severity::info, std::format("only the first {} partitions were checked for a filesystem", max_sniffed));
		}

		void find_overlaps() {
			std::vector<const Partition*> sorted;
			for (const Partition& p : map_.partitions)
				if (p.sector_count > 0)
					sorted.push_back(&p);
			std::ranges::sort(sorted, {}, [](const Partition* p) { return p->first_lba; });
			// first_lba + sector_count - 1 can't wrap: gpt entries have last >= first, mbr values are 33 bits at most
			const auto last = [](const Partition* q) { return q->first_lba + (q->sector_count - 1); };
			const Partition* furthest = nullptr; // of those so far, the one that ends last
			for (const Partition* p : sorted) {
				if (furthest && p->first_lba <= last(furthest))
					note(Severity::warning, std::format("partitions {} and {} overlap", furthest->number, p->number));
				if (!furthest || last(p) > last(furthest))
					furthest = p;
			}
		}

	private:
		IBlockSource& source_;
		PartitionMap& map_;
		std::uint64_t size_;
	};

	bool boot_sector_filesystem(FileSystem fs) { // the ones whose boot sector is sector 0, where an mbr would be
		return fs == FileSystem::fat12_16 || fs == FileSystem::fat32 || fs == FileSystem::exfat || fs == FileSystem::ntfs || fs == FileSystem::bitlocker;
	}

	Result<PartitionMap> scan(IBlockSource& source, const PartitionScanOptions& options) {
		const std::uint32_t asked = options.sector_size;
		if (asked != 0 && asked != 512 && asked != 1024 && asked != 2048 && asked != 4096)
			return fail(Errc::invalid_argument, "scanning partitions: the sector size has to be 512, 1024, 2048 or 4096");

		PartitionMap map;
		t_scan scan(source, map);
		const std::uint64_t size = source.size_bytes();
		if (size < 512) {
			map.disk_sectors = 0;
			scan.note(Severity::error, std::format("the image is {} bytes, smaller than one sector: there's no room for a partition table", size));
			return map;
		}

		Result<std::vector<std::byte>> start = scan.read(0, static_cast<std::size_t>(std::min<std::uint64_t>(size, 8192)));
		if (!start)
			return std::unexpected(start.error());
		ByteReader head(*start);

		map.sector_size = asked;
		if (asked == 0) // gpt's header is in the second sector, which says how big a sector is
			map.sector_size = !head.matches(512, "EFI PART") && head.matches(4096, "EFI PART") ? 4096 : 512;
		map.disk_sectors = size / map.sector_size;
		if (map.disk_sectors == 0) { // only when a sector size was asked for
			scan.note(Severity::error, std::format("the image is {} bytes, smaller than one {} byte sector", size, map.sector_size));
			return map;
		}
		if (size % map.sector_size != 0)
			scan.note(Severity::info, std::format("the image isn't a whole number of {} byte sectors, the last {} bytes aren't in any sector", map.sector_size, size % map.sector_size));

		const t_mbr mbr = parse_mbr(std::span(*start).first(512));
		map.mbr_signature = mbr.valid ? mbr.signature : 0;
		const FileSystem at_start = detect_filesystem(std::span(*start).first(std::min<std::size_t>(start->size(), sniff_bytes)));

		// the primary gpt, then the backup where the primary says it is, or at the very end
		std::string primary_why, backup_why;
		bool primary_signature = false, backup_signature = false;
		std::optional<t_gpt> primary = map.disk_sectors > 1 ? scan.gpt_at(1, primary_why, primary_signature) : std::nullopt;
		const std::uint64_t last = map.disk_sectors - 1;
		std::uint64_t backup_lba = primary ? primary->alternate_lba : last;
		std::optional<t_gpt> backup = backup_lba > 1 ? scan.gpt_at(backup_lba, backup_why, backup_signature) : std::nullopt;
		if (!backup && primary && backup_lba != last) {
			std::string why_last;
			bool signature_last = false;
			if (last > 1 && (backup = scan.gpt_at(last, why_last, signature_last)))
				backup_lba = last;
		}
		const bool gpt_seen = primary || backup || primary_signature || backup_signature;
		const t_gpt* gpt = primary ? &*primary : backup ? &*backup : nullptr;

		if (gpt && mbr.valid && !mbr.protective && mbr.used > 0) {
			// what systems go by: without a protective entry they read the mbr. a gpt that's still there is usually left
			// over from before the disk was repartitioned, and may still show where the old partitions were
			map.scheme = PartitionScheme::mbr;
			scan.note(Severity::warning, "there's also a valid GPT on the disk, but the MBR doesn't protect it, so operating systems ignore it: "
				"it's probably left over from before the disk was repartitioned");
			scan.add_mbr_partitions(mbr);
		} else if (gpt) {
			map.scheme = PartitionScheme::gpt;
			map.gpt_from_backup = !primary;
			map.disk_guid = gpt->disk_guid;
			map.gpt_first_usable = gpt->first_usable;
			map.gpt_last_usable = gpt->last_usable;
			if (!primary)
				scan.note(Severity::warning, std::format("the primary GPT isn't valid ({}), the backup at sector {} is used instead", primary_why, backup_lba));
			else if (!backup)
				scan.note(Severity::warning, std::format("the backup GPT isn't valid ({}): the image may be truncated", backup_why));
			else {
				if (primary->alternate_lba != last)
					scan.note(Severity::info, std::format("the primary GPT says the backup is at sector {}, but the last sector is {}: the disk may have been resized", primary->alternate_lba, last));
				if (primary->disk_guid != backup->disk_guid || primary->first_usable != backup->first_usable || primary->last_usable != backup->last_usable
					|| primary->entry_count != backup->entry_count || primary->entry_size != backup->entry_size || primary->entries_crc != backup->entries_crc)
					scan.note(Severity::warning, "the primary and backup GPT don't match, the primary is used");
			}
			if (gpt->last_usable >= map.disk_sectors)
				scan.note(Severity::warning, std::format("the GPT is for a disk of at least {} sectors, the image has {}: it's probably truncated", gpt->last_usable + 2, map.disk_sectors));
			if (gpt->revision != 0x00010000)
				scan.note(Severity::info, std::format("the GPT's revision is {}.{}, fenrir knows 1.0", gpt->revision >> 16, gpt->revision & 0xFFFF));
			if (!mbr.valid || !mbr.protective)
				scan.note(Severity::warning, "sector 0 has no protective MBR, some systems won't see this disk as GPT");
			else if (mbr.used > 0)
				scan.note(Severity::info, std::format("the MBR is a hybrid one: besides the protective entry it lists {} partition{} of its own, the GPT's are listed here",
					mbr.used, mbr.used == 1 ? "" : "s"));
			scan.add_gpt_partitions(*gpt);
		} else if (mbr.valid && mbr.protective) {
			map.scheme = PartitionScheme::gpt;
			scan.note(Severity::error, std::format("the MBR says the disk uses GPT, but neither GPT is valid: the primary because {}, the backup because {}", primary_why, backup_why));
		} else if (boot_sector_filesystem(at_start) || (!mbr.valid && at_start != FileSystem::unknown)) {
			map.scheme = PartitionScheme::none;
			map.whole_disk = at_start;
			scan.note(Severity::info, std::format("there's no partition table, the whole disk is one {} filesystem", to_string(at_start)));
		} else if (mbr.valid) {
			map.scheme = PartitionScheme::mbr;
			if (gpt_seen)
				scan.note(Severity::warning, std::format("there's a damaged GPT on the disk too: the primary because {}, the backup because {}", primary_why, backup_why));
			scan.add_mbr_partitions(mbr);
		} else {
			map.scheme = PartitionScheme::none;
			const bool blank = std::ranges::all_of(std::span(*start).first(512), [](std::byte b) { return b == std::byte{ 0 }; });
			if (gpt_seen)
				scan.note(Severity::error, std::format("there's a GPT signature but no valid GPT: the primary because {}, the backup because {}", primary_why, backup_why));
			else
				scan.note(Severity::info, blank ? "sector 0 is empty: there's no partition table" : "sector 0 has no partition table");
		}

		if (map.scheme == PartitionScheme::mbr && map.partitions.empty())
			scan.note(Severity::info, "the MBR lists no partitions");
		scan.finish_partitions();
		scan.find_overlaps();
		return map;
	}
}

storage::Result<storage::PartitionMap> storage::scan_partitions(IBlockSource& source, const PartitionScanOptions& options) noexcept {
	try {
		return scan(source, options);
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "scanning partitions");
	}
}
