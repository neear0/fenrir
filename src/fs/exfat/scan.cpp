#include <algorithm>
#include <format>
#include <new>
#include <optional>
#include <string_view>

#include "storage/bytes.h"
#include "storage/exfat.h"
#include "../common.h"

namespace {
	using namespace storage;
	using Severity = Diagnostic::Severity;

	constexpr std::uint32_t fat_bad = 0xFFFFFFF7, fat_end = 0xFFFFFFF8; // 0xFFFFFFFF ends a chain, F8 to FE aren't used
	constexpr std::uint64_t max_directory_bytes = 256ull << 20;        // the spec's limit
	constexpr std::uint8_t in_use = 0x80;
	constexpr std::uint8_t type_bitmap = 0x81, type_upcase = 0x82, type_label = 0x83, type_file = 0x85, type_stream = 0xC0, type_name = 0xC1;
	constexpr std::uint16_t attr_directory = 0x10;
	constexpr std::uint8_t flag_allocation_possible = 0x01, flag_no_fat_chain = 0x02;
	constexpr std::size_t entry_size = 32;

	// sectors 0 to 10 of the boot region, without the volume flags and percent in use, which change while it's mounted
	std::uint32_t boot_checksum(std::span<const std::byte> region, std::uint32_t bps) {
		std::uint32_t sum = 0;
		for (std::size_t i = 0; i < 11 * std::size_t(bps); i++) {
			if (i == 106 || i == 107 || i == 112)
				continue;
			sum = ((sum & 1) ? 0x80000000u : 0u) + (sum >> 1) + std::to_integer<std::uint32_t>(region[i]);
		}
		return sum;
	}

	// a file's entry set, with the in-use bits put back for a deleted one: they were set when the checksum was made
	std::uint16_t set_checksum(std::span<const std::byte> set, bool deleted) {
		std::uint16_t sum = 0;
		for (std::size_t i = 0; i < set.size(); i++) {
			if (i == 2 || i == 3)
				continue;
			std::uint8_t b = std::to_integer<std::uint8_t>(set[i]);
			if (deleted && i % entry_size == 0)
				b |= in_use;
			sum = static_cast<std::uint16_t>(((sum & 1) ? 0x8000u : 0u) + (sum >> 1) + b);
		}
		return sum;
	}

	// the boot region at base: 12 sectors, the last one the checksum of the 11 before it
	std::optional<ExfatVolume> parse_boot(IBlockSource& source, std::uint64_t base, std::string& why, std::vector<std::string>& notes) {
		std::vector<std::byte> first(512);
		if (!read_exact(source, base, first)) {
			why = "it couldn't be read";
			return std::nullopt;
		}
		ByteReader r(first);
		if (!r.matches(3, "EXFAT   ")) {
			why = "it doesn't say EXFAT";
			return std::nullopt;
		}
		if (r.u8(0) != 0xEB || r.u8(1) != 0x76 || r.u8(2) != 0x90 || r.le16(510) != 0xAA55) {
			why = "its jump instruction or boot signature is wrong";
			return std::nullopt;
		}
		for (std::size_t i = 11; i < 64; i++)
			if (r.u8(i) != 0) {
				why = "bytes 11 to 63 aren't zero, as they are on every exFAT volume";
				return std::nullopt;
			}
		const std::uint8_t bps_shift = r.u8(108), spc_shift = r.u8(109);
		if (bps_shift < 9 || bps_shift > 12) {
			why = std::format("its sector size (2^{}) isn't 512 to 4096 bytes", bps_shift);
			return std::nullopt;
		}
		if (spc_shift > 25 - bps_shift) {
			why = std::format("its clusters (2^{} sectors) are bigger than exFAT's 32 MiB", spc_shift);
			return std::nullopt;
		}
		const std::uint32_t bps = 1u << bps_shift;
		std::vector<std::byte> region(12 * std::size_t(bps));
		if (!read_exact(source, base, region)) {
			why = "it couldn't be read";
			return std::nullopt;
		}
		r = ByteReader(region);

		ExfatVolume v;
		v.bytes_per_sector = bps;
		v.sectors_per_cluster = 1u << spc_shift;
		v.cluster_size = bps << spc_shift;
		v.total_sectors = r.le64(72);
		const std::uint32_t fat_offset = r.le32(80), fat_length = r.le32(84), heap_offset = r.le32(88);
		v.cluster_count = r.le32(92);
		v.root_cluster = r.le32(96);
		v.serial = r.le32(100);
		v.revision = r.le16(104);
		const std::uint16_t flags = r.le16(106);
		v.fat_count = r.u8(110);

		if ((v.revision >> 8) != 1) {
			why = std::format("its revision is {}.{:02}, only 1.x exists", v.revision >> 8, v.revision & 0xFF);
			return std::nullopt;
		}
		if (v.fat_count != 1 && v.fat_count != 2) {
			why = std::format("it says it has {} FATs, exFAT has 1 or 2", v.fat_count);
			return std::nullopt;
		}
		const std::uint64_t fat_needed = ((std::uint64_t(v.cluster_count) + 2) * 4 + bps - 1) / bps;
		if (fat_offset < 24 || fat_length < fat_needed || heap_offset < fat_offset + std::uint64_t(fat_length) * v.fat_count
			|| heap_offset >= v.total_sectors || v.cluster_count == 0 || v.cluster_count > 0xFFFFFFF5
			|| v.cluster_count > (v.total_sectors - heap_offset) >> spc_shift) {
			why = std::format("its layout makes no sense (FAT at sector {}, {} sectors long, clusters from sector {}, {} clusters in {} sectors)",
				fat_offset, fat_length, heap_offset, v.cluster_count, v.total_sectors);
			return std::nullopt;
		}
		if (v.root_cluster < 2 || v.root_cluster >= v.cluster_count + 2ull) {
			why = std::format("its root directory cluster ({}) isn't a data cluster", v.root_cluster);
			return std::nullopt;
		}
		const std::uint32_t sum = boot_checksum(region, bps);
		for (std::size_t at = 11 * std::size_t(bps); at < 12 * std::size_t(bps); at += 4)
			if (r.le32(at) != sum) {
				why = "its checksum doesn't match";
				return std::nullopt;
			}
		v.active_fat = v.fat_count == 2 ? (flags & 1) : 0;
		v.dirty = flags & 2;
		v.fat_offset = (fat_offset + std::uint64_t(v.active_fat) * fat_length) * bps;
		v.heap_offset = std::uint64_t(heap_offset) * bps;
		if (v.dirty)
			notes.push_back("the volume wasn't unmounted cleanly (its dirty flag is set): what was being written then may be inconsistent");
		if (flags & 4)
			notes.push_back("the volume says its media has failed reads or writes before");
		if (v.total_sectors * bps > source.size_bytes())
			notes.push_back(std::format("the volume is {} bytes but only {} are there: the image may be truncated, clusters past its end can't be read",
				v.total_sectors * bps, source.size_bytes()));
		return v;
	}

	Timestamp exfat_time(std::uint32_t stamp, std::uint8_t ten_ms) {
		Timestamp t;
		if (stamp == 0)
			return t;
		t.year = static_cast<std::uint16_t>(1980 + (stamp >> 25));
		t.month = static_cast<std::uint8_t>((stamp >> 21) & 0x0F);
		t.day = static_cast<std::uint8_t>((stamp >> 16) & 0x1F);
		t.hour = static_cast<std::uint8_t>((stamp >> 11) & 0x1F);
		t.minute = static_cast<std::uint8_t>((stamp >> 5) & 0x3F);
		t.second = static_cast<std::uint8_t>((stamp & 0x1F) * 2 + (ten_ms >= 100 ? 1 : 0));
		t.millisecond = static_cast<std::uint16_t>(ten_ms % 100 * 10);
		t.valid = t.month >= 1 && t.month <= 12 && t.day >= 1 && t.hour < 24 && t.minute < 60 && t.second < 60 && ten_ms < 200;
		return t;
	}

	// a directory read whole, with where each of its clusters is: entry sets can run from one cluster into the next
	struct t_directory {
		std::vector<std::byte> bytes;
		std::vector<std::uint64_t> offsets; // per cluster, in the volume
		std::vector<bool> readable;         // per cluster
	};

	// the clusters a file is in as far as they can be followed, and why that's fewer than it needs
	struct t_follow {
		std::vector<std::uint32_t> clusters;
		std::string stop;
		bool chain_gone = false;      // deleted, its fat chain was cleared: one piece was assumed
		bool chain_too_long = false;  // deleted, its fat chain goes on past what its size needs
	};

	class t_scanner {
	public:
		t_scanner(IBlockSource& source, ExfatScan& scan, const CancellationToken* cancel) : source_(source), scan_(scan), v_(scan.volume), cancel_(cancel) {}

		void note(Severity severity, std::string message) { fsutil::note(scan_.diagnostics, severity, std::move(message)); }

		Result<void> load_fat() {
			const std::uint64_t bytes = (std::uint64_t(v_.cluster_count) + 2) * 4;
			if (bytes > fsutil::max_table_bytes)
				return fail(Errc::unsupported, "reading the FAT: it's bigger than the 256 MiB fenrir loads");
			std::vector<std::byte> raw(static_cast<std::size_t>(bytes));
			if (Result<void> read = read_exact(source_, v_.fat_offset, raw); !read)
				return std::unexpected(read.error());
			ByteReader r(raw);
			fat_.resize(static_cast<std::size_t>(bytes / 4));
			for (std::size_t i = 0; i < fat_.size(); i++)
				fat_[i] = r.le32(i * 4);
			return {};
		}

		// the root directory, then the allocation bitmap it names, then everything in it
		Result<void> walk_root() {
			dir_seen_.assign(v_.cluster_count + 2u, false);
			std::vector<std::uint32_t> root;
			std::string problem;
			chain(v_.root_cluster, max_directory_bytes / v_.cluster_size, root, problem, true);
			if (!problem.empty())
				note(Severity::warning, std::format("the root directory's cluster chain {}, only what comes before that is read", problem));
			const t_directory dir = read_directory(root);
			if (Result<void> bitmap = load_bitmap(dir); !bitmap)
				return bitmap;
			walk(dir, RecoveryCandidate::no_parent, "", 0, false);
			if (cancelled_)
				return fail(Errc::cancelled, "scanning exFAT");
			fsutil::mark_shared_claims(claims_, scan_.candidates);
			if (broken_sets_ > 0)
				note(Severity::info, std::format("{} deleted entry set{} no longer matched {} checksum and {} left out: newer entries were written over part of {}",
					broken_sets_, broken_sets_ == 1 ? "" : "s", broken_sets_ == 1 ? "its" : "their", broken_sets_ == 1 ? "was" : "were", broken_sets_ == 1 ? "it" : "them"));
			return {};
		}

	private:
		bool data_cluster(std::uint64_t c) const { return c >= 2 && c < v_.cluster_count + 2ull; }
		std::uint64_t cluster_offset(std::uint64_t c) const { return v_.heap_offset + (c - 2) * v_.cluster_size; }
		bool cluster_readable(std::uint64_t c) const { return cluster_offset(c) + v_.cluster_size <= source_.size_bytes(); }
		bool allocated(std::uint64_t c) const {
			const std::uint64_t bit = c - 2;
			return (std::to_integer<std::uint8_t>(bitmap_[static_cast<std::size_t>(bit / 8)]) >> (bit % 8)) & 1;
		}

		// up to max clusters of a fat chain from start. directories' clusters are only read once in the whole scan, so a
		// fat that links directories into each other can't make the walk go round
		void chain(std::uint32_t start, std::uint64_t max, std::vector<std::uint32_t>& out, std::string& problem, bool directory) {
			for (std::uint32_t c = start; out.size() < max;) {
				if (!data_cluster(c)) {
					problem = std::format("leads to cluster {}, which isn't a data cluster", c);
					return;
				}
				if (directory && dir_seen_[c]) {
					problem = std::format("leads back to cluster {}, which was already read", c);
					return;
				}
				if (directory)
					dir_seen_[c] = true;
				out.push_back(c);
				const std::uint32_t next = fat_[c];
				if (next >= fat_end)
					return;
				if (next == 0 || next == fat_bad) {
					problem = std::format("runs into a {} cluster after cluster {}", next == 0 ? "free" : "bad", c);
					return;
				}
				if (std::ranges::find(out, next) != out.end()) {
					problem = std::format("loops back to cluster {}", next);
					return;
				}
				c = next;
			}
		}

		t_directory read_directory(const std::vector<std::uint32_t>& clusters) {
			t_directory dir;
			dir.bytes.resize(clusters.size() * std::size_t(v_.cluster_size));
			for (std::size_t i = 0; i < clusters.size(); i++) {
				const std::uint32_t c = clusters[i];
				dir.offsets.push_back(cluster_offset(c));
				const std::span<std::byte> into = std::span(dir.bytes).subspan(i * v_.cluster_size, v_.cluster_size);
				bool ok = cluster_readable(c);
				if (!ok) {
					note(Severity::warning, std::format("directory cluster {} is past the end of the image", c));
				} else if (Result<void> read = read_exact(source_, cluster_offset(c), into); !read) {
					note(Severity::error, std::format("directory cluster {} couldn't be read: {}", c, to_string(read.error().code)));
					ok = false;
				}
				dir.readable.push_back(ok);
			}
			return dir;
		}

		Result<void> load_bitmap(const t_directory& root) {
			for (std::size_t at = 0; at + entry_size <= root.bytes.size(); at += entry_size) {
				ByteReader e(std::span(root.bytes).subspan(at, entry_size));
				const std::uint8_t type = e.u8(0);
				if (type == 0)
					break;
				if (type == type_label && e.u8(1) <= 11) {
					std::u16string label;
					for (std::size_t i = 0; i < e.u8(1); i++)
						label += static_cast<char16_t>(e.le16(2 + 2 * i));
					v_.label = fsutil::utf8(label);
				}
				if (type != type_bitmap || static_cast<std::uint32_t>(e.u8(1) & 1) != v_.active_fat || !bitmap_.empty())
					continue;
				const std::uint32_t first = e.le32(20);
				const std::uint64_t length = e.le64(24), needed = (std::uint64_t(v_.cluster_count) + 7) / 8;
				if (length < needed || length > fsutil::max_table_bytes)
					return fail(Errc::unsupported, "reading the allocation bitmap: its size doesn't fit the volume");
				std::vector<std::uint32_t> clusters;
				std::string problem;
				const std::uint64_t count = (length + v_.cluster_size - 1) / v_.cluster_size;
				chain(first, count, clusters, problem, false);
				if (clusters.size() < count) {
					// its chain should be in the fat, some formatters only mark it in one piece
					note(Severity::info, std::format("the allocation bitmap's cluster chain {}, it's read as {} clusters in one piece from {}", problem, count, first));
					clusters.clear();
					for (std::uint64_t i = 0; i < count; i++)
						clusters.push_back(static_cast<std::uint32_t>(first + i));
				}
				bitmap_.resize(static_cast<std::size_t>(count * v_.cluster_size));
				for (std::size_t i = 0; i < clusters.size(); i++) {
					if (!data_cluster(clusters[i]))
						return fail(Errc::unsupported, "reading the allocation bitmap: it runs past the last cluster");
					const std::span<std::byte> into = std::span(bitmap_).subspan(i * v_.cluster_size, v_.cluster_size);
					if (Result<void> read = read_exact(source_, cluster_offset(clusters[i]), into); !read)
						return std::unexpected(read.error());
				}
				bitmap_.resize(static_cast<std::size_t>(needed));
				for (std::uint64_t c = 2; c < v_.cluster_count + 2ull; c++)
					v_.free_clusters += !allocated(c);
			}
			if (bitmap_.empty())
				return fail(Errc::unsupported, "scanning exFAT: the root directory has no allocation bitmap, nothing says which clusters are in use");
			return {};
		}

		void walk(const t_directory& dir, std::uint64_t parent, const std::string& path, int depth, bool deleted_dir) {
			const std::size_t entries = dir.bytes.size() / entry_size;
			const std::size_t per_cluster = v_.cluster_size / entry_size;
			auto readable = [&](std::size_t i) { return dir.readable[i / per_cluster]; };
			for (std::size_t i = 0; i < entries;) {
				if (cancel_ && cancel_->cancelled()) {
					cancelled_ = true;
					return;
				}
				if (!readable(i)) {
					i++;
					continue;
				}
				const std::uint8_t type = std::to_integer<std::uint8_t>(dir.bytes[i * entry_size]);
				if (type == 0x00)
					return; // the end marker: nothing after it was ever used
				if ((type & 0x7F) != (type_file & 0x7F)) {
					i++;
					continue;
				}
				const bool deleted = !(type & in_use);
				const std::size_t count = std::to_integer<std::uint8_t>(dir.bytes[i * entry_size + 1]);
				const std::size_t used = entry_set(dir, i, count, deleted, readable, parent, path, depth, deleted_dir);
				if (cancelled_)
					return;
				i += used;
			}
		}

		// the entry set at entry i: a file entry, its stream extension, its name. how many entries it took up, 1 when
		// it isn't one, so whatever follows is still looked at
		template <class Readable>
		std::size_t entry_set(const t_directory& dir, std::size_t i, std::size_t count, bool deleted, Readable& readable, std::uint64_t parent,
			const std::string& path, int depth, bool deleted_dir) {
			const std::size_t entries = dir.bytes.size() / entry_size;
			auto broken = [&] {
				broken_sets_ += deleted;
				if (!deleted)
					note(Severity::warning, std::format("the entry set at byte {} doesn't hold together and was skipped", entry_offset(dir, i)));
				return std::size_t(1);
			};
			if (count < 2 || count > 18 || i + count >= entries)
				return broken();
			for (std::size_t k = 1; k <= count; k++) {
				const std::uint8_t t = std::to_integer<std::uint8_t>(dir.bytes[(i + k) * entry_size]);
				if (!readable(i + k) || !(t & 0x40) || bool(t & in_use) == deleted)
					return broken(); // a secondary entry that isn't one, or one in use in a deleted set: it was reused
			}
			const std::span<const std::byte> set = std::span(dir.bytes).subspan(i * entry_size, (count + 1) * entry_size);
			ByteReader primary(set.first(entry_size));
			ByteReader stream(set.subspan(entry_size, entry_size));
			if ((stream.u8(0) | in_use) != type_stream)
				return broken();
			const std::size_t name_length = stream.u8(3), name_entries = (name_length + 14) / 15;
			if (name_length == 0 || name_entries + 1 > count)
				return broken();
			std::u16string name;
			for (std::size_t k = 0; k < name_entries; k++) {
				ByteReader n(set.subspan((2 + k) * entry_size, entry_size));
				if ((n.u8(0) | in_use) != type_name)
					return broken();
				for (std::size_t c = 0; c < 15 && name.size() < name_length; c++)
					name += static_cast<char16_t>(n.le16(2 + 2 * c));
			}
			const bool checksum_ok = set_checksum(set, deleted) == primary.le16(2);
			if (!checksum_ok && deleted)
				return broken();

			RecoveryCandidate cand;
			cand.id = scan_.candidates.size();
			cand.parent = parent;
			cand.deleted = deleted || deleted_dir;
			cand.attributes = primary.le16(4);
			cand.directory = cand.attributes & attr_directory;
			cand.created = exfat_time(primary.le32(8), primary.u8(20));
			cand.modified = exfat_time(primary.le32(12), primary.u8(21));
			cand.accessed = exfat_time(primary.le32(16), 0);
			cand.entry_offset = entry_offset(dir, i);
			cand.name = fsutil::utf8(name);
			cand.path = path + "/" + cand.name;
			if (!checksum_ok)
				cand.reasons.push_back("its directory entries don't match their checksum, something in them was changed");

			const std::uint8_t flags = stream.u8(1);
			t_data data;
			data.contiguous = flags & flag_no_fat_chain;
			data.size = (flags & flag_allocation_possible) ? stream.le64(24) : 0;
			data.valid = std::min(data.size, stream.le64(8));
			data.first = (flags & flag_allocation_possible) ? stream.le32(20) : 0;
			cand.first_cluster = data.first;

			if (cand.directory)
				directory(cand, data, depth);
			else
				file(cand, data);
			if (!checksum_ok && scan_.candidates.back().integrity == Integrity::intact)
				scan_.candidates.back().integrity = Integrity::uncertain;
			return count + 1;
		}

		std::uint64_t entry_offset(const t_directory& dir, std::size_t i) const {
			const std::size_t per_cluster = v_.cluster_size / entry_size;
			return dir.offsets[i / per_cluster] + i % per_cluster * entry_size;
		}

		struct t_data {
			std::uint64_t size = 0, valid = 0;
			std::uint32_t first = 0;
			bool contiguous = false;
		};

		// the clusters that hold the first need clusters' worth of a file. a deleted file's are only its own while
		// they're free: the first one that isn't ends it
		t_follow follow(const t_data& data, std::uint64_t need, bool deleted) {
			t_follow f;
			auto take = [&](std::uint64_t c) {
				if (!data_cluster(c)) {
					f.stop = std::format("it would run past the last cluster ({})", v_.cluster_count + 1);
					return false;
				}
				if (!cluster_readable(c)) {
					f.stop = std::format("cluster {} is past the end of the image", c);
					return false;
				}
				if (deleted && allocated(c)) {
					f.stop = std::format("cluster {} is in use by something else now", c);
					return false;
				}
				f.clusters.push_back(static_cast<std::uint32_t>(c));
				return true;
			};
			auto in_one_piece = [&] {
				for (std::uint64_t k = 0; k < need && take(data.first + k);)
					k++;
			};
			if (data.contiguous) {
				in_one_piece();
				return f;
			}
			if (deleted && need > 1 && fat_[data.first] == 0) {
				f.chain_gone = true;
				in_one_piece();
				return f;
			}
			for (std::uint64_t c = data.first; f.clusters.size() < need;) {
				if (std::ranges::find(f.clusters, c) != f.clusters.end()) {
					f.stop = std::format("its FAT chain loops back to cluster {}", c);
					break;
				}
				if (!take(c))
					break;
				if (f.clusters.size() == need) {
					if (deleted && fat_[c] < fat_end)
						f.chain_too_long = true;
					break;
				}
				const std::uint32_t next = fat_[c];
				if (next >= fat_end || next == 0 || next == fat_bad) {
					f.stop = std::format("its FAT chain ends after {} of the {} clusters it needs", f.clusters.size(), need);
					break;
				}
				c = next;
			}
			return f;
		}

		// the bytes up to the valid data length from the clusters, zeros after that
		void add_extents(RecoveryCandidate& cand, const std::vector<std::uint32_t>& clusters, std::uint64_t valid) {
			std::uint64_t on_disk = std::min(valid, cand.recoverable_bytes);
			for (std::size_t i = 0; i < clusters.size() && on_disk > 0;) {
				std::size_t j = i + 1;
				while (j < clusters.size() && clusters[j] == clusters[j - 1] + 1)
					j++;
				const std::uint64_t length = std::min<std::uint64_t>(on_disk, (j - i) * std::uint64_t(v_.cluster_size));
				cand.extents.push_back({ cluster_offset(clusters[i]), length });
				on_disk -= length;
				i = j;
			}
			if (cand.recoverable_bytes > valid)
				cand.extents.push_back({ 0, cand.recoverable_bytes - valid, true });
		}

		void claim(const std::vector<std::uint32_t>& clusters, std::size_t candidate) {
			for (std::size_t i = 0; i < clusters.size();) {
				std::size_t j = i + 1;
				while (j < clusters.size() && clusters[j] == clusters[j - 1] + 1)
					j++;
				claims_.push_back({ clusters[i], clusters[j - 1] + 1ull, candidate });
				i = j;
			}
		}

		void file(RecoveryCandidate& cand, const t_data& data) {
			cand.size = data.size;
			if (data.size == 0) {
				cand.integrity = Integrity::intact;
				if (cand.deleted)
					cand.reasons.push_back("it was empty, there's no data to lose");
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			if (data.valid < data.size)
				cand.reasons.push_back(std::format("only its first {} bytes were ever written (its valid data length), the rest reads as zeros", data.valid));
			if (data.valid == 0) {
				cand.integrity = Integrity::intact;
				cand.recoverable_bytes = cand.size;
				cand.extents.push_back({ 0, cand.size, true });
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			if (!data_cluster(data.first)) {
				cand.integrity = Integrity::unrecoverable;
				cand.reasons.push_back(std::format("its first cluster ({}) isn't a data cluster, there's nothing that says where its data was", data.first));
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			// only the clusters holding written bytes matter: past the valid data length it's zeros whatever they hold
			const std::uint64_t need = (data.valid + v_.cluster_size - 1) / v_.cluster_size;
			const t_follow f = follow(data, need, cand.deleted);
			const std::uint64_t got = f.clusters.size();
			cand.recoverable_bytes = got == need ? cand.size : std::min(cand.size, got * v_.cluster_size);
			add_extents(cand, f.clusters, data.valid);

			if (!cand.deleted) {
				cand.integrity = got == need ? Integrity::intact : got == 0 ? Integrity::unrecoverable : Integrity::partial;
				if (got < need)
					cand.reasons.push_back(f.stop);
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			if (got == 0) {
				cand.integrity = Integrity::unrecoverable;
				cand.reasons.push_back(std::format("its first cluster ({}) is in use by something else now, its data has been overwritten", data.first));
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			const std::uint32_t first = f.clusters.front(), last = f.clusters.back();
			if (got < need) {
				cand.integrity = Integrity::partial;
				cand.reasons.push_back(std::format("it needs {} clusters, but {}: only the first {} bytes are recoverable", need, f.stop, cand.recoverable_bytes));
			} else {
				cand.integrity = Integrity::intact;
				if (need == 1)
					cand.reasons.push_back(std::format("its one cluster ({}) is free", first));
				else if (data.contiguous)
					cand.reasons.push_back(std::format("its entry says it was stored in one piece, and clusters {} to {} are all free", first, last));
				else if (f.chain_gone)
					cand.reasons.push_back(std::format("its FAT chain was cleared, so it's assumed to have been stored in one piece: clusters {} to {} are free", first, last));
				else
					cand.reasons.push_back(std::format("its FAT chain is still there and all {} clusters in it are free: deleting a file on exFAT frees its clusters in the allocation bitmap, the FAT usually keeps the chain", need));
			}
			if (f.chain_too_long) {
				cand.integrity = Integrity::uncertain;
				cand.reasons.push_back("its FAT chain goes on past the clusters its size needs, so the chain may be another file's that used the same clusters later");
			}
			claim(f.clusters, scan_.candidates.size());
			scan_.candidates.push_back(std::move(cand));
		}

		void directory(RecoveryCandidate& cand, const t_data& data, int depth) {
			const std::size_t index = scan_.candidates.size();
			std::vector<std::uint32_t> clusters;
			const std::uint64_t need = (std::min(data.size, max_directory_bytes) + v_.cluster_size - 1) / v_.cluster_size;
			if (!data_cluster(data.first) || need == 0) {
				cand.integrity = Integrity::unrecoverable;
				cand.reasons.push_back(std::format("its first cluster ({}) isn't a data cluster", data.first));
			} else {
				const t_follow f = follow(data, need, cand.deleted);
				for (std::uint32_t c : f.clusters) {
					if (dir_seen_[c])
						break;
					dir_seen_[c] = true;
					clusters.push_back(c);
				}
				if (clusters.size() == need) {
					cand.integrity = Integrity::intact;
				} else if (clusters.empty()) {
					cand.integrity = Integrity::unrecoverable;
					cand.reasons.push_back(f.clusters.empty() ? std::format("its first cluster ({}) is in use by something else now", data.first)
						: std::format("its first cluster ({}) was already read as another directory's", data.first));
				} else {
					cand.integrity = Integrity::partial;
					cand.reasons.push_back(std::format("only {} of its {} clusters could be read ({}), entries in the rest can't be found", clusters.size(), need,
						f.stop.empty() ? "the others were already read as another directory's" : f.stop));
				}
				if (cand.deleted && !clusters.empty() && cand.integrity == Integrity::intact)
					cand.reasons.push_back("its clusters are free, so the entries in them are read");
			}
			const std::string path = cand.path;
			const bool deleted = cand.deleted;
			const std::uint64_t id = cand.id;
			scan_.candidates.push_back(std::move(cand));
			if (clusters.empty())
				return;
			if (depth + 1 >= fsutil::max_depth) {
				scan_.candidates[index].reasons.push_back(std::format("it's more than {} directories deep, what's in it isn't read", fsutil::max_depth));
				note(Severity::warning, std::format("directories are nested more than {} deep, the deepest aren't read", fsutil::max_depth));
				return;
			}
			walk(read_directory(clusters), id, path, depth + 1, deleted);
		}

		IBlockSource& source_;
		ExfatScan& scan_;
		ExfatVolume& v_;
		const CancellationToken* cancel_;
		std::vector<std::uint32_t> fat_;
		std::vector<std::byte> bitmap_;
		std::vector<bool> dir_seen_;
		std::vector<fsutil::Claim> claims_;
		std::uint64_t broken_sets_ = 0;
		bool cancelled_ = false;
	};

	Result<ExfatScan> scan(IBlockSource& source, const CancellationToken* cancel) {
		if (source.size_bytes() < 12 * 512)
			return fail(Errc::unsupported, "scanning exFAT: the volume is smaller than its boot region");
		ExfatScan result;
		std::vector<std::string> notes;
		std::string why;
		std::optional<ExfatVolume> volume = parse_boot(source, 0, why, notes);
		if (!volume) {
			// the backup boot region follows the main one, 12 sectors in
			std::vector<std::byte> first(512);
			std::uint32_t bps = 0;
			if (read_exact(source, 0, first)) {
				const std::uint8_t shift = std::to_integer<std::uint8_t>(first[108]);
				bps = shift >= 9 && shift <= 12 ? 1u << shift : 0;
			}
			for (std::uint32_t try_bps : { bps, 512u, 4096u }) {
				if (volume || try_bps == 0 || source.size_bytes() < 24ull * try_bps)
					continue;
				std::string backup_why;
				std::vector<std::string> backup_notes;
				volume = parse_boot(source, 12ull * try_bps, backup_why, backup_notes);
				if (volume && volume->bytes_per_sector != try_bps)
					volume.reset();
				if (volume)
					notes = std::move(backup_notes);
			}
			if (!volume)
				return fail(Errc::unsupported, "scanning exFAT: the boot sector isn't an exFAT one");
			volume->from_backup_boot_region = true;
			result.diagnostics.push_back({ Severity::warning, std::format("the boot region isn't valid ({}), the backup after it is used", why) });
		}
		result.volume = *volume;
		for (std::string& n : notes)
			result.diagnostics.push_back({ Severity::info, std::move(n) });

		t_scanner scanner(source, result, cancel);
		if (Result<void> fat = scanner.load_fat(); !fat)
			return std::unexpected(fat.error());
		if (Result<void> walked = scanner.walk_root(); !walked)
			return std::unexpected(walked.error());
		return result;
	}
}

storage::Result<storage::ExfatScan> storage::scan_exfat(IBlockSource& volume, const CancellationToken* cancel) noexcept {
	try {
		return scan(volume, cancel);
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "scanning exFAT");
	}
}
