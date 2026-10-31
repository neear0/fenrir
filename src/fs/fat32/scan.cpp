#include <algorithm>
#include <array>
#include <format>
#include <new>
#include <optional>
#include <string_view>

#include "storage/bytes.h"
#include "storage/fat32.h"
#include "../common.h"

namespace {
	using namespace storage;
	using Severity = Diagnostic::Severity;

	using fsutil::max_depth;
	using fsutil::put_utf8;
	using fsutil::utf8;
	constexpr std::uint32_t fat_mask = 0x0FFFFFFF, fat_bad = 0x0FFFFFF7, fat_end = 0x0FFFFFF8;
	constexpr std::uint8_t attr_volume = 0x08, attr_directory = 0x10, attr_lfn = 0x0F;
	constexpr std::uint8_t deleted_mark = 0xE5;

	std::optional<Fat32Volume> parse_boot(std::span<const std::byte> sector, std::uint64_t source_size, std::string& why, std::vector<std::string>& notes) {
		ByteReader r(sector);
		if (r.le16(510) != 0xAA55) {
			why = "no boot signature";
			return std::nullopt;
		}
		if (!((r.u8(0) == 0xEB && r.u8(2) == 0x90) || r.u8(0) == 0xE9)) {
			why = "no jump instruction at its start";
			return std::nullopt;
		}
		Fat32Volume v;
		v.bytes_per_sector = r.le16(11);
		v.sectors_per_cluster = r.u8(13);
		v.reserved_sectors = r.le16(14);
		v.fat_count = r.u8(16);
		const std::uint16_t root_entries = r.le16(17), total16 = r.le16(19), fat16 = r.le16(22);
		const std::uint32_t total32 = r.le32(32);
		v.fat_sectors = r.le32(36);
		const std::uint16_t ext_flags = r.le16(40), version = r.le16(42);
		v.root_cluster = r.le32(44);

		const std::uint32_t bps = v.bytes_per_sector, spc = v.sectors_per_cluster;
		if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) {
			why = std::format("{} bytes per sector isn't 512, 1024, 2048 or 4096", bps);
			return std::nullopt;
		}
		if (spc == 0 || (spc & (spc - 1)) != 0) {
			why = std::format("{} sectors per cluster isn't a power of 2", spc);
			return std::nullopt;
		}
		if (root_entries != 0 || fat16 != 0) {
			why = "it's FAT12 or FAT16, not FAT32";
			return std::nullopt;
		}
		if (v.reserved_sectors == 0 || v.fat_count == 0 || v.fat_count > 4 || v.fat_sectors == 0) {
			why = std::format("its layout makes no sense ({} reserved sectors, {} FATs of {} sectors)", v.reserved_sectors, v.fat_count, v.fat_sectors);
			return std::nullopt;
		}
		if (version != 0) {
			why = std::format("its FAT32 version is {}.{}, only 0.0 exists", version >> 8, version & 0xFF);
			return std::nullopt;
		}
		v.total_sectors = total16 != 0 ? total16 : total32;
		v.cluster_size = bps * spc;
		const std::uint64_t data_start = v.reserved_sectors + std::uint64_t(v.fat_count) * v.fat_sectors;
		if (data_start >= v.total_sectors) {
			why = std::format("its data area would start at sector {}, past its end ({} sectors)", data_start, v.total_sectors);
			return std::nullopt;
		}
		std::uint64_t clusters = (v.total_sectors - data_start) / spc;
		const std::uint64_t fat_entries = std::uint64_t(v.fat_sectors) * bps / 4;
		if (fat_entries < 3) {
			why = "its FAT is too small for any cluster";
			return std::nullopt;
		}
		if (clusters + 2 > fat_entries) {
			notes.push_back(std::format("the FAT has entries for {} clusters, the data area holds {}: only the first {} are used", fat_entries - 2, clusters, fat_entries - 2));
			clusters = fat_entries - 2;
		}
		if (clusters > 0x0FFFFFF5) {
			why = std::format("{} clusters is more than FAT32 can number", clusters);
			return std::nullopt;
		}
		v.cluster_count = static_cast<std::uint32_t>(clusters);
		if (v.cluster_count < 65525)
			notes.push_back(std::format("{} clusters is fewer than Microsoft's FAT32 minimum of 65525, it's read as FAT32 because its boot sector is FAT32's", v.cluster_count));
		if (v.root_cluster < 2 || v.root_cluster >= v.cluster_count + 2u) {
			why = std::format("its root directory cluster ({}) isn't a data cluster", v.root_cluster);
			return std::nullopt;
		}
		v.active_fat = (ext_flags & 0x80) ? ext_flags & 0x0F : 0;
		if (v.active_fat >= v.fat_count) {
			why = std::format("it says FAT {} is the one in use, but there are only {}", v.active_fat + 1, v.fat_count);
			return std::nullopt;
		}
		v.fat_offset = (v.reserved_sectors + std::uint64_t(v.active_fat) * v.fat_sectors) * bps;
		v.data_offset = data_start * bps;
		if (v.total_sectors * bps > source_size)
			notes.push_back(std::format("the volume is {} bytes but only {} are there: the image may be truncated, clusters past its end can't be read", v.total_sectors * bps, source_size));

		if (r.u8(66) == 0x29) {
			v.volume_id = r.le32(67);
			std::string label;
			for (std::size_t i = 0; i < 11; i++) {
				const std::uint8_t c = r.u8(71 + i);
				label += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '_';
			}
			while (label.ends_with(' '))
				label.pop_back();
			if (label != "NO NAME")
				v.label = label;
		}
		return v;
	}

	// the 8.3 name's checksum that each of its long name entries repeats
	std::uint8_t short_name_checksum(const std::array<std::uint8_t, 11>& name) {
		std::uint8_t sum = 0;
		for (std::uint8_t c : name)
			sum = static_cast<std::uint8_t>(((sum & 1) << 7) + (sum >> 1) + c);
		return sum;
	}

	// "NOTES.TXT", lowercased where the entry's case bits say so (that's how windows stores "notes.txt" without a long
	// name). bytes past ascii are in an oem code page the volume doesn't name, they become U+FFFD
	std::string format_short_name(const std::array<std::uint8_t, 11>& name, std::uint8_t case_bits) {
		auto part = [&](std::size_t from, std::size_t length, bool lower) {
			std::size_t end = from + length;
			while (end > from && name[end - 1] == ' ')
				end--;
			std::string text;
			for (std::size_t i = from; i < end; i++) {
				const std::uint8_t c = name[i];
				if (c >= 0x80)
					put_utf8(text, 0xFFFD);
				else
					put_utf8(text, lower && c >= 'A' && c <= 'Z' ? c + 32 : c);
			}
			return text;
		};
		const std::string base = part(0, 8, case_bits & 0x08), ext = part(8, 3, case_bits & 0x10);
		return ext.empty() ? base : base + "." + ext;
	}

	// the character windows and linux put first in the 8.3 name they make from a long name
	std::optional<std::uint8_t> expected_first(const std::u16string& long_name) {
		for (char16_t c : long_name) {
			if (c == u'.' || c == u' ')
				continue;
			if (c >= 0x80)
				return std::nullopt; // depends on the code page, anything goes
			if (c >= u'a' && c <= u'z')
				return static_cast<std::uint8_t>(c - 32);
			if ((c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || std::u16string_view(u"!#$%&'()-@^_`{}~").contains(c))
				return static_cast<std::uint8_t>(c);
			return static_cast<std::uint8_t>('_');
		}
		return std::nullopt;
	}

	Timestamp fat_time(std::uint16_t date, std::uint16_t time, std::uint8_t tenths, bool date_only) {
		Timestamp t;
		t.date_only = date_only;
		if (date == 0)
			return t;
		t.year = static_cast<std::uint16_t>(1980 + (date >> 9));
		t.month = static_cast<std::uint8_t>((date >> 5) & 0x0F);
		t.day = static_cast<std::uint8_t>(date & 0x1F);
		t.valid = t.month >= 1 && t.month <= 12 && t.day >= 1;
		if (!date_only) {
			t.hour = static_cast<std::uint8_t>(time >> 11);
			t.minute = static_cast<std::uint8_t>((time >> 5) & 0x3F);
			t.second = static_cast<std::uint8_t>((time & 0x1F) * 2 + (tenths >= 100 ? 1 : 0));
			t.millisecond = static_cast<std::uint16_t>(tenths % 100 * 10);
			t.valid = t.valid && t.hour < 24 && t.minute < 60 && t.second < 60 && tenths < 200;
		}
		return t;
	}

	struct t_lfn {
		std::array<std::byte, 32> raw{};
		bool deleted() const { return std::to_integer<std::uint8_t>(raw[0]) == deleted_mark; }
		std::uint8_t order() const { return std::to_integer<std::uint8_t>(raw[0]); }
		std::uint8_t checksum() const { return std::to_integer<std::uint8_t>(raw[13]); }
		void append_to(std::u16string& name, bool& ended) const { // its 13 utf-16 characters, up to a 0
			ByteReader r(raw);
			for (std::size_t at : { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 }) {
				const char16_t c = r.le16(at);
				if (ended || c == 0) {
					ended = true;
					continue;
				}
				name += c;
			}
		}
	};

	class t_scanner {
	public:
		t_scanner(IBlockSource& source, Fat32Scan& scan, const CancellationToken* cancel) : source_(source), scan_(scan), v_(scan.volume), cancel_(cancel) {}

		void note(Severity severity, std::string message) {
			fsutil::note(scan_.diagnostics, severity, std::move(message));
		}

		Result<void> load_fat() {
			const std::uint64_t bytes = (std::uint64_t(v_.cluster_count) + 2) * 4;
			if (bytes > fsutil::max_table_bytes)
				return fail(Errc::unsupported, "reading the FAT: it's bigger than the 256 MiB fenrir loads");
			fat_.resize(static_cast<std::size_t>(bytes / 4));
			std::vector<std::byte> raw(static_cast<std::size_t>(bytes));
			if (Result<void> read = read_exact(source_, v_.fat_offset, raw); !read)
				return std::unexpected(read.error());
			ByteReader r(raw);
			for (std::size_t i = 0; i < fat_.size(); i++)
				fat_[i] = r.le32(i * 4) & fat_mask;
			for (std::uint32_t c = 2; c < v_.cluster_count + 2u; c++)
				v_.free_clusters += fat_[c] == 0;

			// the other copies, in pieces: windows writes them all, so a difference means a crash mid-write or damage
			for (std::uint32_t copy = 0; copy < v_.fat_count; copy++) {
				if (copy == v_.active_fat)
					continue;
				const std::uint64_t offset = (v_.reserved_sectors + std::uint64_t(copy) * v_.fat_sectors) * v_.bytes_per_sector;
				std::uint64_t differ = 0;
				std::vector<std::byte> piece(std::size_t(1) << 20);
				for (std::uint64_t done = 0; done < bytes; done += piece.size()) {
					const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(piece.size(), bytes - done));
					if (Result<void> read = read_exact(source_, offset + done, std::span(piece).first(length)); !read) {
						note(Severity::warning, std::format("FAT {} couldn't be read to compare it: {}", copy + 1, to_string(read.error().code)));
						differ = 0;
						break;
					}
					ByteReader other(std::span(piece).first(length));
					for (std::size_t i = 0; i < length / 4; i++)
						differ += (other.le32(i * 4) & fat_mask) != fat_[static_cast<std::size_t>(done / 4 + i)];
				}
				if (differ > 0)
					note(Severity::warning, std::format("FAT {} differs from FAT {} in {} entries, FAT {} is used", copy + 1, v_.active_fat + 1, differ, v_.active_fat + 1));
			}
			return {};
		}

		Result<void> walk_root() {
			dir_seen_.assign(v_.cluster_count + 2u, false);
			std::vector<std::uint32_t> root;
			std::string problem;
			directory_chain(v_.root_cluster, root, problem);
			if (!problem.empty())
				note(Severity::warning, std::format("the root directory's cluster chain {}, only what comes before that is read", problem));
			walk(root, RecoveryCandidate::no_parent, "", 0, false);
			if (cancelled_)
				return fail(Errc::cancelled, "scanning FAT32");
			fsutil::mark_shared_claims(claims_, scan_.candidates);
			if (ignored_long_names_ > 0)
				note(Severity::info, std::format("{} long name{} didn't match the entry after {} and {} ignored", ignored_long_names_,
					ignored_long_names_ == 1 ? "" : "s", ignored_long_names_ == 1 ? "it" : "them", ignored_long_names_ == 1 ? "was" : "were"));
			return {};
		}

	private:
		bool data_cluster(std::uint64_t c) const { return c >= 2 && c < v_.cluster_count + 2ull; }
		std::uint64_t cluster_offset(std::uint64_t c) const { return v_.data_offset + (c - 2) * v_.cluster_size; }
		bool cluster_readable(std::uint64_t c) const { return cluster_offset(c) + v_.cluster_size <= source_.size_bytes(); }

		// a directory's clusters, each only once in the whole scan so a fat that links directories into each other
		// can't make the walk go round
		void directory_chain(std::uint32_t start, std::vector<std::uint32_t>& out, std::string& problem) {
			for (std::uint32_t c = start;;) {
				if (!data_cluster(c)) {
					problem = std::format("leads to cluster {}, which isn't a data cluster", c);
					return;
				}
				if (dir_seen_[c]) {
					problem = std::format("leads back to cluster {}, which was already read", c);
					return;
				}
				dir_seen_[c] = true;
				out.push_back(c);
				const std::uint32_t next = fat_[c];
				if (next >= fat_end)
					return;
				if (next == 0 || next == fat_bad) {
					problem = std::format("runs into a {} cluster after cluster {}", next == 0 ? "free" : "bad", c);
					return;
				}
				c = next;
			}
		}

		void walk(const std::vector<std::uint32_t>& clusters, std::uint64_t parent, const std::string& path, int depth, bool deleted_dir) {
			std::vector<t_lfn> pending;
			std::vector<std::byte> buffer(v_.cluster_size);
			for (std::uint32_t c : clusters) {
				if (cancel_ && cancel_->cancelled()) {
					cancelled_ = true;
					return;
				}
				if (!cluster_readable(c)) {
					note(Severity::warning, std::format("directory cluster {} is past the end of the image", c));
					pending.clear();
					continue;
				}
				if (Result<void> read = read_exact(source_, cluster_offset(c), buffer); !read) {
					note(Severity::error, std::format("directory cluster {} couldn't be read: {}", c, to_string(read.error().code)));
					pending.clear();
					continue;
				}
				for (std::size_t at = 0; at < buffer.size(); at += 32) {
					ByteReader e(std::span(buffer).subspan(at, 32));
					const std::uint8_t first = e.u8(0), attr = e.u8(11);
					if (first == 0x00)
						return; // the end marker: nothing after it was ever used
					if ((attr & 0x3F) == attr_lfn) {
						t_lfn lfn;
						std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(at), 32, lfn.raw.begin());
						pending.push_back(lfn);
						continue;
					}
					if (e.matches(0, ".          ") || e.matches(0, "..         ")) {
						pending.clear();
						continue;
					}
					if ((attr & attr_volume) && !(attr & attr_directory)) {
						if (depth == 0 && first != deleted_mark) { // all 11 characters are the label, there's no extension
							std::string label;
							for (std::size_t i = 0; i < 11; i++)
								put_utf8(label, e.u8(i) < 0x80 ? char32_t(e.u8(i)) : char32_t(0xFFFD));
							while (label.ends_with(' '))
								label.pop_back();
							scan_.volume.label = label;
						}
						pending.clear();
						continue;
					}
					entry(e, pending, cluster_offset(c) + at, parent, path, depth, deleted_dir);
					pending.clear();
					if (cancelled_)
						return;
				}
			}
		}

		void entry(ByteReader& e, const std::vector<t_lfn>& pending, std::uint64_t entry_offset, std::uint64_t parent, const std::string& path, int depth, bool deleted_dir) {
			std::array<std::uint8_t, 11> sfn{};
			for (std::size_t i = 0; i < 11; i++)
				sfn[i] = e.u8(i);
			const bool marked = sfn[0] == deleted_mark;
			if (sfn[0] == 0x05)
				sfn[0] = deleted_mark; // 0x05 stands in for a real 0xE5 first byte (a kanji lead byte)

			RecoveryCandidate cand;
			cand.id = scan_.candidates.size();
			cand.parent = parent;
			cand.deleted = marked || deleted_dir;
			cand.attributes = e.u8(11);
			cand.directory = cand.attributes & attr_directory;
			cand.first_cluster = std::uint64_t(e.le16(20)) << 16 | e.le16(26);
			cand.size = e.le32(28);
			cand.entry_offset = entry_offset;
			cand.created = fat_time(e.le16(16), e.le16(14), e.u8(13), false);
			cand.accessed = fat_time(e.le16(18), 0, 0, true);
			cand.modified = fat_time(e.le16(24), e.le16(22), 0, false);

			std::optional<std::u16string> long_name;
			if (!marked && !pending.empty()) {
				// live: the entries go n|0x40, n-1, ... 1 and all carry the 8.3 name's checksum
				const std::uint8_t sum = short_name_checksum(sfn);
				const std::size_t n = pending.front().order() & 0x1F;
				bool fits = (pending.front().order() & 0x40) && n == pending.size();
				for (std::size_t i = 0; fits && i < n; i++)
					fits = !pending[i].deleted() && (pending[i].order() & 0x1F) == n - i && pending[i].checksum() == sum;
				if (fits) {
					std::u16string name;
					bool ended = false;
					for (std::size_t i = n; i-- > 0;)
						pending[i].append_to(name, ended);
					long_name = name;
				} else {
					ignored_long_names_++;
				}
			} else if (marked && !pending.empty()) {
				// deleted: the order bytes are gone, overwritten with the mark. what's left is the checksum, which can only
				// be matched by one first byte of the 8.3 name. that's the long name's first character when they belong
				// together, and anything else when they don't
				const std::uint8_t sum = pending.back().checksum();
				std::size_t from = pending.size();
				while (from > 0 && pending[from - 1].deleted() && pending[from - 1].checksum() == sum)
					from--;
				std::u16string name;
				bool ended = false;
				for (std::size_t i = pending.size(); i-- > from;)
					pending[i].append_to(name, ended);
				std::optional<std::uint8_t> match;
				for (int b = 0; b < 256 && !match; b++) {
					std::array<std::uint8_t, 11> trial = sfn;
					trial[0] = static_cast<std::uint8_t>(b);
					if (short_name_checksum(trial) == sum)
						match = static_cast<std::uint8_t>(b);
				}
				const std::optional<std::uint8_t> expected = expected_first(name);
				if (match && from < pending.size() && !name.empty() && (expected ? *match == *expected : (*match >= 0x80 || *match == '_'))) {
					long_name = name;
					sfn[0] = *match;
				} else {
					ignored_long_names_++;
				}
			}
			if (marked && !long_name) {
				sfn[0] = '_';
				cand.name_damaged = true;
				cand.reasons.push_back("the first character of its 8.3 name is lost when a file is deleted, it's shown as _");
			}
			cand.short_name = format_short_name(sfn, e.u8(12));
			cand.name = long_name ? utf8(*long_name) : cand.short_name;
			cand.path = path + "/" + cand.name;

			if (cand.directory)
				directory(cand, depth);
			else if (cand.deleted)
				deleted_file(cand);
			else
				live_file(cand);
		}

		void directory(RecoveryCandidate& cand, int depth) {
			const std::size_t index = scan_.candidates.size();
			std::vector<std::uint32_t> clusters;
			if (!data_cluster(cand.first_cluster)) {
				cand.integrity = Integrity::unrecoverable;
				cand.reasons.push_back(std::format("its first cluster ({}) isn't a data cluster", cand.first_cluster));
			} else if (!cand.deleted) {
				std::string problem;
				directory_chain(static_cast<std::uint32_t>(cand.first_cluster), clusters, problem);
				cand.integrity = problem.empty() ? Integrity::intact : Integrity::partial;
				if (!problem.empty())
					cand.reasons.push_back(std::format("its cluster chain {}, only the clusters before that are read", problem));
			} else {
				// a deleted directory's own chain is gone. its first cluster is still its own if it's free and it starts
				// with the "." entry pointing back at it
				const std::uint32_t first = static_cast<std::uint32_t>(cand.first_cluster);
				std::vector<std::byte> head(64);
				const bool readable = cluster_readable(first) && read_exact(source_, cluster_offset(first), head).has_value();
				ByteReader dot(head);
				const bool self = readable && dot.matches(0, ".          ") && (dot.u8(11) & attr_directory)
					&& (std::uint64_t(dot.le16(20)) << 16 | dot.le16(26)) == first;
				if (fat_[first] != 0) {
					cand.integrity = Integrity::unrecoverable;
					cand.reasons.push_back(std::format("its first cluster ({}) is in use by something else now", first));
				} else if (!self) {
					cand.integrity = Integrity::unrecoverable;
					cand.reasons.push_back(std::format("its first cluster ({}) no longer holds its entries", first));
				} else if (dir_seen_[first]) {
					cand.integrity = Integrity::uncertain;
					cand.reasons.push_back(std::format("its first cluster ({}) was already read as another directory's", first));
				} else {
					dir_seen_[first] = true;
					clusters.push_back(first);
					cand.integrity = Integrity::intact;
					cand.reasons.push_back("only its first cluster is read: a deleted directory's chain is gone, entries in later clusters can't be found");
				}
			}
			const std::string path = cand.path;
			const bool deleted = cand.deleted;
			const std::uint64_t id = cand.id;
			scan_.candidates.push_back(std::move(cand));
			if (clusters.empty())
				return;
			if (depth + 1 >= max_depth) {
				scan_.candidates[index].reasons.push_back(std::format("it's more than {} directories deep, what's in it isn't read", max_depth));
				note(Severity::warning, std::format("directories are nested more than {} deep, the deepest aren't read", max_depth));
				return;
			}
			walk(clusters, id, path, depth + 1, deleted);
		}

		void live_file(RecoveryCandidate& cand) {
			if (cand.size == 0) {
				cand.integrity = Integrity::intact;
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			const std::uint64_t need = (cand.size + v_.cluster_size - 1) / v_.cluster_size;
			std::vector<std::uint64_t> chain;
			std::string problem;
			std::uint64_t c = cand.first_cluster;
			while (chain.size() < need) {
				if (!data_cluster(c)) {
					problem = std::format("its cluster chain leads to cluster {}, which isn't a data cluster", c);
					break;
				}
				if (!cluster_readable(c)) {
					problem = std::format("cluster {} is past the end of the image", c);
					break;
				}
				chain.push_back(c);
				const std::uint32_t next = fat_[static_cast<std::size_t>(c)];
				if (chain.size() == need) {
					if (next < fat_end)
						cand.reasons.push_back("its cluster chain is longer than its size needs, the extra clusters aren't part of it");
					break;
				}
				if (next >= fat_end || next == 0 || next == fat_bad) {
					problem = std::format("its cluster chain ends after {} of the {} clusters its size needs", chain.size(), need);
					break;
				}
				c = next;
			}
			cand.recoverable_bytes = std::min<std::uint64_t>(cand.size, chain.size() * v_.cluster_size);
			add_extents(cand, chain);
			if (problem.empty()) {
				cand.integrity = Integrity::intact;
			} else {
				cand.integrity = chain.empty() ? Integrity::unrecoverable : Integrity::partial;
				cand.reasons.push_back(problem);
			}
			scan_.candidates.push_back(std::move(cand));
		}

		void deleted_file(RecoveryCandidate& cand) {
			if (cand.size == 0) {
				cand.integrity = Integrity::intact;
				cand.reasons.push_back("it was empty, there's no data to lose");
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			const std::uint64_t first = cand.first_cluster;
			if (!data_cluster(first)) {
				cand.integrity = Integrity::unrecoverable;
				cand.reasons.push_back(std::format("its first cluster ({}) isn't a data cluster, there's nothing that says where its data was", first));
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			const std::uint64_t need = (cand.size + v_.cluster_size - 1) / v_.cluster_size;
			std::uint64_t run = 0;
			std::string stop;
			for (; run < need; run++) {
				const std::uint64_t c = first + run;
				if (!data_cluster(c)) {
					stop = std::format("it would run past the last cluster ({})", v_.cluster_count + 1);
					break;
				}
				if (!cluster_readable(c)) {
					stop = std::format("cluster {} is past the end of the image", c);
					break;
				}
				if (fat_[static_cast<std::size_t>(c)] != 0) {
					stop = std::format("cluster {} is in use by something else now", c);
					break;
				}
			}
			if (run == 0) {
				cand.integrity = Integrity::unrecoverable;
				cand.reasons.push_back(std::format("its first cluster ({}) is in use by something else now, its data has been overwritten", first));
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			cand.recoverable_bytes = std::min(cand.size, run * v_.cluster_size);
			cand.extents.push_back({ cluster_offset(first), cand.recoverable_bytes });
			if (run == need) {
				cand.integrity = Integrity::intact;
				cand.reasons.push_back(need == 1
					? std::format("its one cluster ({}) is free", first)
					: std::format("clusters {} to {} are free, it's assumed to have been stored in one piece: FAT doesn't keep a deleted file's chain", first, first + need - 1));
			} else {
				cand.integrity = Integrity::partial;
				cand.reasons.push_back(std::format("it needs {} clusters from {}, but {}: it was fragmented or the rest was overwritten, only the first {} bytes are recoverable",
					need, first, stop, cand.recoverable_bytes));
			}
			if ((first >> 16) == 0 && v_.cluster_count > 0xFFFF) {
				cand.integrity = Integrity::uncertain;
				cand.reasons.push_back("the high half of its first cluster number is 0, which is also what windows leaves behind when it deletes from FAT32: the first cluster may be wrong");
			}
			claims_.push_back({ first, first + run, scan_.candidates.size() });
			scan_.candidates.push_back(std::move(cand));
		}

		void add_extents(RecoveryCandidate& cand, const std::vector<std::uint64_t>& chain) {
			std::uint64_t left = cand.recoverable_bytes;
			for (std::size_t i = 0; i < chain.size() && left > 0;) {
				std::size_t j = i + 1;
				while (j < chain.size() && chain[j] == chain[j - 1] + 1)
					j++;
				const std::uint64_t length = std::min<std::uint64_t>(left, (j - i) * std::uint64_t(v_.cluster_size));
				cand.extents.push_back({ cluster_offset(chain[i]), length });
				left -= length;
				i = j;
			}
		}

		IBlockSource& source_;
		Fat32Scan& scan_;
		Fat32Volume& v_;
		const CancellationToken* cancel_;
		std::vector<std::uint32_t> fat_;
		std::vector<bool> dir_seen_;
		std::vector<fsutil::Claim> claims_;
		std::uint64_t ignored_long_names_ = 0;
		bool cancelled_ = false;
	};

	Result<Fat32Scan> scan(IBlockSource& source, const CancellationToken* cancel) {
		if (source.size_bytes() < 512)
			return fail(Errc::unsupported, "scanning FAT32: the volume is smaller than one sector");
		std::vector<std::byte> boot(512);
		if (Result<void> read = read_exact(source, 0, boot); !read)
			return std::unexpected(read.error());

		Fat32Scan result;
		std::vector<std::string> notes;
		std::string why;
		std::optional<Fat32Volume> volume = parse_boot(boot, source.size_bytes(), why, notes);
		if (!volume) {
			// the backup boot sector is at sector 6 on every fat32 volume windows or mkfs.fat makes
			std::string backup_why;
			std::vector<std::string> backup_notes;
			std::vector<std::byte> backup(512);
			if (source.size_bytes() >= 7 * 512 && read_exact(source, 6 * 512, backup))
				volume = parse_boot(backup, source.size_bytes(), backup_why, backup_notes);
			if (!volume)
				return fail(Errc::unsupported, "scanning FAT32: the boot sector isn't a FAT32 one");
			volume->from_backup_boot_sector = true;
			notes = std::move(backup_notes);
			result.diagnostics.push_back({ Severity::warning, std::format("the boot sector isn't valid ({}), the backup in sector 6 is used", why) });
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

std::string_view storage::to_string(Integrity integrity) noexcept {
	switch (integrity) {
	case Integrity::intact: return "intact";
	case Integrity::partial: return "partial";
	case Integrity::uncertain: return "uncertain";
	case Integrity::unrecoverable: return "unrecoverable";
	}
	return "?";
}

storage::Result<storage::Fat32Scan> storage::scan_fat32(IBlockSource& volume, const CancellationToken* cancel) noexcept {
	try {
		return scan(volume, cancel);
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "scanning FAT32");
	}
}
