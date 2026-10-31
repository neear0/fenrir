#include <algorithm>
#include <format>
#include <map>
#include <new>
#include <optional>
#include <string_view>

#include "storage/bytes.h"
#include "storage/ntfs.h"
#include "../common.h"

namespace {
	using namespace storage;
	using Severity = Diagnostic::Severity;

	constexpr std::uint32_t attr_standard_information = 0x10, attr_file_name = 0x30, attr_volume_name = 0x60, attr_volume_information = 0x70,
		attr_data = 0x80, attr_end = 0xFFFFFFFF;
	constexpr std::uint16_t record_in_use = 0x0001, record_directory = 0x0002;
	constexpr std::uint16_t flag_compressed = 0x00FF, flag_encrypted = 0x4000;
	constexpr std::uint8_t namespace_dos = 2;
	constexpr std::uint64_t record_volume = 3, record_root = 5, record_bitmap = 6, first_user_record = 24;
	constexpr std::uint64_t max_records = 4ull << 20;      // 4 GiB of 1 KiB records
	constexpr std::size_t fixup_stride = 512;              // ntfs protects every 512 bytes, whatever the sector size
	constexpr std::size_t chunk = std::size_t(1) << 20;   // the mft is read this much at a time
	constexpr std::string_view orphan_directory = "$OrphanFiles"; // what the sleuth kit calls it too

	std::uint64_t record_number(std::uint64_t reference) { return reference & 0xFFFFFFFFFFFFull; }
	std::uint16_t sequence_of(std::uint64_t reference) { return static_cast<std::uint16_t>(reference >> 48); }

	// 100 ns ticks since 1601-01-01 utc
	Timestamp filetime(std::uint64_t ticks) {
		Timestamp t;
		t.utc = true;
		if (ticks == 0)
			return t;
		const std::uint64_t ms = ticks / 10000, seconds = ms / 1000;
		const std::int64_t days = static_cast<std::int64_t>(seconds / 86400) - 134774; // to days since 1970-01-01
		const std::uint64_t in_day = seconds % 86400;
		// howard hinnant's civil_from_days
		const std::int64_t z = days + 719468;
		const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
		const std::int64_t doe = z - era * 146097;
		const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		const std::int64_t mp = (5 * doy + 2) / 153;
		const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
		const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
		const std::int64_t year = yoe + era * 400 + (month <= 2);
		t.year = static_cast<std::uint16_t>(year);
		t.month = static_cast<std::uint8_t>(month);
		t.day = static_cast<std::uint8_t>(day);
		t.hour = static_cast<std::uint8_t>(in_day / 3600);
		t.minute = static_cast<std::uint8_t>(in_day / 60 % 60);
		t.second = static_cast<std::uint8_t>(in_day % 60);
		t.millisecond = static_cast<std::uint16_t>(ms % 1000);
		t.valid = year >= 1980 && year <= 9999;
		return t;
	}

	struct t_run {
		std::uint64_t vcn = 0, lcn = 0, length = 0; // clusters
		bool sparse = false;
	};

	// one piece of a $DATA attribute: a non-resident one can be split over several records, each holding the runs
	// from its starting vcn
	struct t_piece {
		std::uint64_t start_vcn = 0;
		std::vector<t_run> runs;
		std::string problem; // its run list didn't decode
	};

	struct t_stream {
		std::u16string name;  // empty: the file's data, else an alternate data stream
		bool resident = false;
		std::vector<std::byte> bytes; // resident
		std::uint64_t size = 0, initialized = 0;
		std::uint16_t flags = 0;
		std::uint16_t compression_unit = 0;
		std::vector<t_piece> pieces;
	};

	struct t_name {
		std::uint64_t parent = 0;
		std::u16string name;
		std::uint8_t name_space = 0;
	};

	struct t_record {
		bool present = false;  // a FILE record that decoded
		std::uint64_t base = 0; // non-zero: an extension of this record
		std::uint16_t sequence = 0, flags = 0;
		std::uint64_t offset = 0; // in the volume
		std::vector<t_name> names;
		std::vector<t_stream> streams;
		Timestamp created, modified, accessed;
		std::uint32_t attributes = 0;
		bool has_times = false;
		std::vector<std::pair<std::uint32_t, std::vector<std::byte>>> volume; // $Volume's name and information

		bool in_use() const { return flags & record_in_use; }
		bool directory() const { return flags & record_directory; }
	};

	// the update sequence: the last 2 bytes of every 512 hold its number, the real bytes are in the array. a mismatch
	// is a write that was torn halfway
	bool apply_fixups(std::span<std::byte> record) {
		ByteReader r(record);
		const std::size_t at = r.le16(4), count = r.le16(6);
		if (count == 0 || count - 1 != record.size() / fixup_stride || at + 2 * count > record.size())
			return false;
		const std::uint16_t usn = r.le16(at);
		for (std::size_t i = 1; i < count; i++) {
			const std::size_t end = i * fixup_stride - 2;
			if (r.le16(end) != usn)
				return false;
			record[end] = record[at + 2 * i];
			record[end + 1] = record[at + 2 * i + 1];
		}
		return true;
	}

	// a non-resident attribute's run list: a header byte (low nibble the length's size, high nibble the offset's), the
	// length, then the lcn as a signed offset from the last one. no offset: a sparse run
	std::vector<t_run> decode_runs(std::span<const std::byte> bytes, std::uint64_t start_vcn, std::uint64_t cluster_count, std::string& problem) {
		std::vector<t_run> runs;
		std::uint64_t vcn = start_vcn;
		std::int64_t lcn = 0;
		for (std::size_t at = 0; at < bytes.size();) {
			const std::uint8_t header = std::to_integer<std::uint8_t>(bytes[at]);
			if (header == 0)
				return runs;
			const std::size_t length_size = header & 0x0F, offset_size = header >> 4;
			if (length_size == 0 || length_size > 8 || offset_size > 8 || at + 1 + length_size + offset_size > bytes.size()) {
				problem = "its run list is damaged";
				return runs;
			}
			std::uint64_t length = 0;
			for (std::size_t i = 0; i < length_size; i++)
				length |= std::to_integer<std::uint64_t>(bytes[at + 1 + i]) << (8 * i);
			std::uint64_t raw = 0;
			for (std::size_t i = 0; i < offset_size; i++)
				raw |= std::to_integer<std::uint64_t>(bytes[at + 1 + length_size + i]) << (8 * i);
			if (offset_size > 0 && offset_size < 8 && (raw >> (8 * offset_size - 1)) & 1)
				raw |= ~0ull << (8 * offset_size); // sign extended
			at += 1 + length_size + offset_size;
			if (length == 0 || length > cluster_count) {
				problem = "its run list has a run of an impossible length";
				return runs;
			}
			t_run run{ vcn, 0, length, offset_size == 0 };
			if (!run.sparse) {
				lcn += static_cast<std::int64_t>(raw);
				if (lcn < 0 || static_cast<std::uint64_t>(lcn) >= cluster_count || length > cluster_count - static_cast<std::uint64_t>(lcn)) {
					problem = std::format("its run list points past the volume's last cluster ({})", cluster_count - 1);
					return runs;
				}
				run.lcn = static_cast<std::uint64_t>(lcn);
			}
			runs.push_back(run);
			vcn += length;
		}
		problem = "its run list has no end";
		return runs;
	}

	class t_scanner {
	public:
		t_scanner(IBlockSource& source, NtfsScan& scan, const CancellationToken* cancel) : source_(source), scan_(scan), v_(scan.volume), cancel_(cancel) {}

		void note(Severity severity, std::string message) { fsutil::note(scan_.diagnostics, severity, std::move(message)); }

		// $MFT's own record, from the mft or its mirror, and from it where the rest of the mft is
		Result<void> load_mft() {
			std::vector<std::byte> raw(v_.record_size);
			std::optional<t_record> own;
			for (const bool mirror : { false, true }) {
				const std::uint64_t cluster = mirror ? v_.mft_mirror_cluster : v_.mft_cluster;
				if (cluster >= v_.cluster_count || !read_exact(source_, cluster * v_.cluster_size, raw))
					continue;
				t_record r = parse(raw, cluster * v_.cluster_size);
				if (r.present && find_stream(r, u"") && !find_stream(r, u"")->resident) {
					own = std::move(r);
					v_.mft_from_mirror = mirror;
					break;
				}
			}
			if (!own)
				return fail(Errc::unsupported, "scanning NTFS: the MFT's own record can't be read, from the MFT or its mirror");
			if (v_.mft_from_mirror)
				note(Severity::warning, "the MFT's own record is damaged, the copy in $MFTMirr is used");
			mft_ = *find_stream(*own, u"");
			return {};
		}

		// every record, then the extensions folded into their base records
		Result<void> read_records() {
			std::string problem;
			std::vector<t_run> runs = runs_of(mft_, problem);
			if (!problem.empty())
				note(Severity::error, std::format("the MFT's {}, the records after that can't be found", problem));
			// no more than its runs hold, and they hold no more than the volume: a damaged size can't ask for millions
			std::uint64_t covered = 0;
			for (const t_run& run : runs)
				covered += run.length;
			std::uint64_t count = std::min({ mft_.size, covered * v_.cluster_size, v_.cluster_count * v_.cluster_size }) / v_.record_size;
			if (count > max_records) {
				note(Severity::warning, std::format("the MFT holds {} records, only the first {} are read", count, max_records));
				count = max_records;
			}
			records_.resize(static_cast<std::size_t>(count));
			v_.records = count;
			const std::uint64_t per_chunk = std::max<std::uint64_t>(1, chunk / v_.record_size);
			std::vector<std::byte> buffer;
			std::uint64_t torn = 0, damaged = 0, unreadable = 0;
			for (std::uint64_t first = 0; first < count; first += per_chunk) {
				if (cancel_ && cancel_->cancelled())
					return fail(Errc::cancelled, "scanning NTFS");
				const std::uint64_t in_chunk = std::min(per_chunk, count - first);
				buffer.resize(static_cast<std::size_t>(in_chunk * v_.record_size));
				// a chunk that doesn't read is tried a record at a time, so one bad sector costs one record
				const bool whole = read_stream(runs, first * v_.record_size, buffer);
				for (std::uint64_t i = 0; i < in_chunk; i++) {
					const std::span<std::byte> raw = std::span(buffer).subspan(static_cast<std::size_t>(i * v_.record_size), v_.record_size);
					const std::uint64_t at = (first + i) * v_.record_size;
					if (!whole && !read_stream(runs, at, raw)) {
						unreadable++;
						continue;
					}
					ByteReader r(raw);
					if (r.matches(0, "BAAD")) {
						damaged++;
						continue;
					}
					if (!r.matches(0, "FILE"))
						continue; // never used
					t_record record = parse(raw, volume_offset(runs, at));
					if (!record.present) {
						torn++;
						continue;
					}
					records_[static_cast<std::size_t>(first + i)] = std::move(record);
				}
			}
			if (torn > 0)
				note(Severity::warning, std::format("{} MFT record{} failed {} update sequence check or didn't decode, and {} left out: a write was torn halfway, or {} damaged",
					torn, torn == 1 ? "" : "s", torn == 1 ? "its" : "their", torn == 1 ? "was" : "were", torn == 1 ? "it's" : "they're"));
			if (damaged > 0)
				note(Severity::warning, std::format("{} MFT record{} marked BAAD by Windows and left out", damaged, damaged == 1 ? " is" : "s are"));
			if (unreadable > 0)
				note(Severity::error, std::format("{} MFT record{} couldn't be read", unreadable, unreadable == 1 ? "" : "s"));
			merge_extensions();
			return {};
		}

		void compare_mirror() {
			const std::uint64_t mirrored = std::min<std::uint64_t>(4, records_.size());
			std::vector<std::byte> mft(static_cast<std::size_t>(mirrored * v_.record_size)), mirror(mft.size());
			if (!read_exact(source_, v_.mft_cluster * v_.cluster_size, mft) || !read_exact(source_, v_.mft_mirror_cluster * v_.cluster_size, mirror))
				return;
			if (mft != mirror)
				note(Severity::warning, "the first records of the MFT differ from their copy in $MFTMirr");
		}

		void read_volume_record() {
			if (records_.size() <= record_volume || !records_[record_volume].present)
				return;
			for (const auto& [type, bytes] : records_[record_volume].volume) {
				ByteReader r(bytes);
				if (type == attr_volume_name) {
					std::u16string label;
					for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
						label += static_cast<char16_t>(r.le16(i));
					v_.label = fsutil::utf8(label);
				} else if (type == attr_volume_information && bytes.size() >= 12) {
					v_.major_version = r.u8(8);
					v_.minor_version = r.u8(9);
					v_.dirty = r.le16(10) & 1;
				}
			}
			if (v_.dirty)
				note(Severity::info, "the volume wasn't unmounted cleanly (its dirty flag is set): what was being written then may be inconsistent");
		}

		Result<void> load_bitmap() {
			const t_stream* data = records_.size() > record_bitmap && records_[record_bitmap].present ? find_stream(records_[record_bitmap], u"") : nullptr;
			if (!data)
				return fail(Errc::unsupported, "scanning NTFS: $Bitmap has no data, nothing says which clusters are in use");
			const std::uint64_t needed = (v_.cluster_count + 7) / 8;
			if (data->size < needed || data->size > fsutil::max_table_bytes)
				return fail(Errc::unsupported, "scanning NTFS: $Bitmap's size doesn't fit the volume");
			bitmap_.assign(static_cast<std::size_t>(needed), std::byte{ 0 });
			if (data->resident) {
				std::copy_n(data->bytes.begin(), std::min(bitmap_.size(), data->bytes.size()), bitmap_.begin());
			} else {
				std::string problem;
				const std::vector<t_run> runs = runs_of(*data, problem);
				if (!problem.empty() || !read_stream(runs, 0, bitmap_))
					return fail(Errc::unsupported, "scanning NTFS: $Bitmap's data can't be read");
			}
			for (std::uint64_t c = 0; c < v_.cluster_count; c++)
				v_.free_clusters += !allocated(c);
			return {};
		}

		// the tree from the root, then whatever has no way back to it
		Result<void> build() {
			children_.assign(records_.size(), {});
			parent_of_.assign(records_.size(), no_record);
			for (std::uint64_t n = 0; n < records_.size(); n++) {
				const t_record& r = records_[static_cast<std::size_t>(n)];
				if (!r.present || r.base != 0 || n == record_root)
					continue;
				const t_name* name = primary_name(r);
				if (!name)
					continue;
				const std::uint64_t p = record_number(name->parent);
				if (valid_parent(p, sequence_of(name->parent))) {
					parent_of_[static_cast<std::size_t>(n)] = p;
					children_[static_cast<std::size_t>(p)].push_back(n);
				}
			}
			visited_.assign(records_.size(), false);
			if (records_.size() > record_root && records_[record_root].present) {
				visited_[record_root] = true;
				walk(record_root, RecoveryCandidate::no_parent, "", true);
			} else {
				note(Severity::error, "the root directory's record is missing, everything is listed under " + std::string(orphan_directory));
			}
			// files whose directory's record is gone or was reused, and directories in a loop
			for (std::uint64_t n = first_user_record; n < records_.size(); n++) {
				if (cancel_ && cancel_->cancelled())
					return fail(Errc::cancelled, "scanning NTFS");
				const t_record& r = records_[static_cast<std::size_t>(n)];
				if (visited_[static_cast<std::size_t>(n)] || !r.present || r.base != 0 || !primary_name(r))
					continue;
				if (parent_of_[static_cast<std::size_t>(n)] != no_record && !visited_[static_cast<std::size_t>(parent_of_[static_cast<std::size_t>(n)])]
					&& !in_loop(n))
					continue; // its parent comes up as an orphan itself, it's listed under that
				const bool loop = parent_of_[static_cast<std::size_t>(n)] != no_record;
				visited_[static_cast<std::size_t>(n)] = true;
				const std::size_t index = add(n, RecoveryCandidate::no_parent, "/" + std::string(orphan_directory));
				scan_.candidates[index].reasons.insert(scan_.candidates[index].reasons.begin(), loop
					? std::format("the directories it's in point at each other instead of leading to the root, so it's listed under {}", orphan_directory)
					: std::format("the directory it was in can't be found any more (its record was reused or is damaged), so it's listed under {}", orphan_directory));
				if (records_[static_cast<std::size_t>(n)].directory())
					walk(n, scan_.candidates[index].id, scan_.candidates[index].path, true);
			}
			fsutil::mark_shared_claims(claims_, scan_.candidates);
			return {};
		}

	private:
		static constexpr std::uint64_t no_record = UINT64_MAX;

		bool allocated(std::uint64_t c) const { return (std::to_integer<std::uint8_t>(bitmap_[static_cast<std::size_t>(c / 8)]) >> (c % 8)) & 1; }

		static const t_stream* find_stream(const t_record& r, std::u16string_view name) {
			for (const t_stream& s : r.streams)
				if (s.name == name)
					return &s;
			return nullptr;
		}

		// the name to show: the long one (win32, or posix) over the 8.3 one windows also keeps
		static const t_name* primary_name(const t_record& r) {
			const t_name* best = nullptr;
			for (const t_name& n : r.names)
				if (!best || (best->name_space == namespace_dos && n.name_space != namespace_dos))
					best = &n;
			return best;
		}

		// a parent reference holds the record's number and its sequence number at the time. windows adds one to the
		// sequence number when it deletes a record, so a deleted directory's files point at the one before
		bool valid_parent(std::uint64_t p, std::uint16_t sequence) const {
			if (p >= records_.size() || !records_[static_cast<std::size_t>(p)].present)
				return false;
			const t_record& parent = records_[static_cast<std::size_t>(p)];
			if (!parent.directory() || parent.base != 0)
				return false;
			return parent.sequence == sequence || (!parent.in_use() && parent.sequence == static_cast<std::uint16_t>(sequence + 1));
		}

		bool in_loop(std::uint64_t n) const {
			std::uint64_t at = n;
			for (std::size_t steps = 0; steps < records_.size(); steps++) {
				at = parent_of_[static_cast<std::size_t>(at)];
				if (at == no_record || visited_[static_cast<std::size_t>(at)])
					return false;
				if (at == n)
					return true;
			}
			return true;
		}

		// what's in directory n, depth first with a stack of its own: a directory comes right before what's in it
		void walk(std::uint64_t n, std::uint64_t parent_id, const std::string& path, bool emit) {
			struct t_frame {
				std::uint64_t record;
				std::uint64_t id;
				std::string path;
				bool emit;
				std::size_t next = 0;
			};
			std::vector<t_frame> stack{ { n, parent_id, path, emit } };
			while (!stack.empty()) {
				t_frame& top = stack.back();
				const std::vector<std::uint64_t>& kids = children_[static_cast<std::size_t>(top.record)];
				if (top.next == kids.size()) {
					stack.pop_back();
					continue;
				}
				const std::uint64_t child = kids[top.next++];
				if (visited_[static_cast<std::size_t>(child)])
					continue;
				visited_[static_cast<std::size_t>(child)] = true;
				// ntfs's own files ($MFT, $Bitmap, $Extend and what's in it) aren't anyone's files to recover
				const bool child_emit = top.emit && child >= first_user_record;
				std::uint64_t id = top.id;
				std::string child_path = top.path;
				if (child_emit) {
					const std::size_t index = add(child, top.id, top.path);
					id = scan_.candidates[index].id;
					child_path = scan_.candidates[index].path;
				}
				if (records_[static_cast<std::size_t>(child)].directory())
					stack.push_back({ child, id, std::move(child_path), child_emit });
			}
		}

		// record n as a candidate, and its alternate data streams after it. the index of the first
		std::size_t add(std::uint64_t n, std::uint64_t parent_id, const std::string& parent_path) {
			const t_record& r = records_[static_cast<std::size_t>(n)];
			const t_name& name = *primary_name(r);
			RecoveryCandidate cand;
			cand.id = scan_.candidates.size();
			cand.parent = parent_id;
			cand.name = fsutil::utf8(name.name);
			for (const t_name& other : r.names)
				if (other.name_space == namespace_dos && &other != &name)
					cand.short_name = fsutil::utf8(other.name);
			cand.path = parent_path + "/" + cand.name;
			cand.deleted = !r.in_use() || (parent_id != RecoveryCandidate::no_parent && scan_.candidates[static_cast<std::size_t>(parent_id)].deleted);
			cand.directory = r.directory();
			cand.created = r.created;
			cand.modified = r.modified;
			cand.accessed = r.accessed;
			cand.attributes = r.attributes;
			cand.entry_offset = r.offset;

			const std::size_t index = scan_.candidates.size();
			if (cand.directory) {
				cand.integrity = Integrity::intact;
				if (cand.deleted)
					cand.reasons.push_back("its MFT record is still there, it hasn't been reused");
				scan_.candidates.push_back(std::move(cand));
			} else {
				const t_stream* data = find_stream(r, u"");
				const std::string base_name = cand.name, base_path = cand.path;
				const bool deleted = cand.deleted;
				file(std::move(cand), data);
				// alternate data streams: "file.txt:Zone.Identifier", next to the file
				for (const t_stream& s : r.streams) {
					if (s.name.empty())
						continue;
					RecoveryCandidate stream;
					stream.id = scan_.candidates.size();
					stream.parent = parent_id;
					stream.name = base_name + ":" + fsutil::utf8(s.name);
					stream.path = base_path + ":" + fsutil::utf8(s.name);
					stream.deleted = deleted;
					stream.created = r.created;
					stream.modified = r.modified;
					stream.accessed = r.accessed;
					stream.entry_offset = r.offset;
					stream.reasons.push_back(std::format("an alternate data stream of {}: more data kept under the file's name, which Explorer doesn't show", base_name));
					file(std::move(stream), &s);
				}
			}
			return index;
		}

		void file(RecoveryCandidate cand, const t_stream* s) {
			if (!s) {
				cand.integrity = Integrity::intact;
				cand.reasons.push_back("it has no data, only its record");
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			cand.size = s->size;
			if (s->flags & flag_encrypted) {
				cand.integrity = Integrity::unrecoverable;
				cand.reasons.push_back("it's encrypted with EFS: its data can't be read without the key of the user who encrypted it");
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			if (!s->resident && ((s->flags & flag_compressed) || s->compression_unit != 0)) {
				cand.integrity = Integrity::unrecoverable;
				cand.reasons.push_back("it's compressed with NTFS compression, which fenrir doesn't decompress yet: its data may well still be there, but it can't be exported as the file");
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			if (s->resident) {
				cand.resident = s->bytes;
				cand.recoverable_bytes = s->bytes.size();
				cand.size = s->bytes.size();
				cand.integrity = Integrity::intact;
				if (cand.deleted)
					cand.reasons.push_back("its data is in its own MFT record, which hasn't been reused");
				scan_.candidates.push_back(std::move(cand));
				return;
			}
			if (s->size == 0) {
				cand.integrity = Integrity::intact;
				scan_.candidates.push_back(std::move(cand));
				return;
			}

			std::string problem;
			const std::vector<t_run> runs = runs_of(*s, problem);
			const std::uint64_t valid = std::min(s->initialized, s->size);
			const std::uint64_t cs = v_.cluster_size;
			if (valid < s->size)
				cand.reasons.push_back(std::format("only its first {} bytes were ever written (its initialized size), the rest reads as zeros", valid));
			if (!runs.empty() && !runs.front().sparse)
				cand.first_cluster = runs.front().lcn;

			// the bytes in vcn order: from the disk, or zeros for sparse runs and past the initialized size. a deleted
			// file's ends at the first cluster that's in use again
			std::uint64_t done = 0;
			std::string stop;
			std::uint64_t taken = UINT64_MAX; // the cluster in use again that ended a deleted file
			bool sparse = false;
			const std::size_t index = scan_.candidates.size();
			for (const t_run& run : runs) {
				if (done >= s->size)
					break;
				if (run.vcn * cs != done) {
					stop = std::format("its run list skips from cluster {} of it to {}", done / cs, run.vcn);
					break;
				}
				const std::uint64_t run_bytes = std::min(run.length * cs, s->size - done);
				if (run.sparse) {
					sparse = true;
					add_extent(cand, { 0, run_bytes, true });
					done += run_bytes;
					continue;
				}
				// written bytes need their clusters; past the initialized size they don't, it's zeros
				const std::uint64_t written = done < valid ? std::min(run_bytes, valid - done) : 0;
				std::uint64_t usable = written;
				if (cand.deleted && written > 0) {
					const std::uint64_t clusters = (written + cs - 1) / cs;
					for (std::uint64_t k = 0; k < clusters; k++)
						if (allocated(run.lcn + k)) {
							usable = k * cs;
							taken = run.lcn + k;
							stop = std::format("cluster {} is in use by something else now", run.lcn + k);
							break;
						}
					if (usable > 0)
						claims_.push_back({ run.lcn, run.lcn + (usable + cs - 1) / cs, index });
				}
				if (usable > 0)
					add_extent(cand, { run.lcn * cs, usable });
				done += usable;
				if (!stop.empty())
					break;
				if (written < run_bytes) {
					add_extent(cand, { 0, run_bytes - written, true });
					done += run_bytes - written;
				}
			}
			if (stop.empty() && done < s->size)
				stop = problem.empty() ? std::format("its run list ends after {} of the {} clusters its size needs", done / cs, (s->size + cs - 1) / cs) : problem;
			cand.recoverable_bytes = done;

			if (stop.empty()) {
				cand.integrity = Integrity::intact;
				if (cand.deleted) {
					std::uint64_t pieces = 0;
					for (const t_run& run : runs)
						pieces += !run.sparse && run.vcn * cs < valid;
					cand.reasons.push_back(pieces <= 1 ? "its record still says where its data is, and those clusters are all free"
						: std::format("its record still says where each of its {} pieces is, and those clusters are all free", pieces));
				}
				if (sparse)
					cand.reasons.push_back("it's sparse: parts of it were never stored, they read as zeros");
			} else {
				cand.integrity = done == 0 ? Integrity::unrecoverable : Integrity::partial;
				if (done == 0 && taken != UINT64_MAX)
					cand.reasons.push_back(std::format("its first cluster ({}) is in use by something else now, its data has been overwritten", taken));
				else
					cand.reasons.push_back(done == 0 ? stop : std::format("{}: only the first {} bytes are recoverable", stop, done));
				if (cand.integrity == Integrity::unrecoverable) {
					cand.extents.clear();
					cand.recoverable_bytes = 0;
				}
			}
			scan_.candidates.push_back(std::move(cand));
		}

		// next to the extent before when they continue it
		static void add_extent(RecoveryCandidate& cand, Extent e) {
			if (!cand.extents.empty()) {
				Extent& last = cand.extents.back();
				if (last.zeros && e.zeros) {
					last.length += e.length;
					return;
				}
				if (!last.zeros && !e.zeros && last.offset + last.length == e.offset) {
					last.length += e.length;
					return;
				}
			}
			cand.extents.push_back(e);
		}

		// bytes of a non-resident stream from offset, through its runs: false when part of them isn't on the disk or
		// can't be read
		bool read_stream(const std::vector<t_run>& runs, std::uint64_t offset, std::span<std::byte> out) {
			const std::uint64_t cs = v_.cluster_size, end = offset + out.size();
			std::uint64_t filled = 0;
			for (const t_run& run : runs) {
				const std::uint64_t from = std::max(offset, run.vcn * cs), to = std::min(end, (run.vcn + run.length) * cs);
				if (from >= to)
					continue;
				if (run.sparse)
					return false;
				const std::uint64_t at = run.lcn * cs + (from - run.vcn * cs);
				if (at + (to - from) > source_.size_bytes() || !read_exact(source_, at, out.subspan(static_cast<std::size_t>(from - offset), static_cast<std::size_t>(to - from))))
					return false;
				filled += to - from;
			}
			return filled == out.size();
		}

		// where byte offset of a stream is in the volume
		std::uint64_t volume_offset(const std::vector<t_run>& runs, std::uint64_t offset) const {
			for (const t_run& run : runs)
				if (!run.sparse && offset >= run.vcn * v_.cluster_size && offset < (run.vcn + run.length) * v_.cluster_size)
					return run.lcn * v_.cluster_size + (offset - run.vcn * v_.cluster_size);
			return 0;
		}

		std::vector<t_run> runs_of(const t_stream& s, std::string& problem) const {
			std::vector<t_run> runs;
			for (const t_piece& p : s.pieces) {
				if (!runs.empty() && p.start_vcn != runs.back().vcn + runs.back().length) {
					problem = "pieces of its run list are missing";
					break;
				}
				runs.insert(runs.end(), p.runs.begin(), p.runs.end());
				if (!p.problem.empty()) {
					problem = p.problem;
					break;
				}
			}
			return runs;
		}

		// a FILE record, fixed up: its names, times and data. not present when it doesn't hold together
		t_record parse(std::span<std::byte> raw, std::uint64_t offset) {
			t_record rec;
			rec.offset = offset;
			if (!apply_fixups(raw))
				return rec;
			ByteReader r(raw);
			rec.sequence = r.le16(16);
			rec.flags = r.le16(22);
			rec.base = r.le64(32);
			const std::size_t used = std::min<std::size_t>(r.le32(24), raw.size());
			std::size_t at = r.le16(20);
			if (at < 42 || at >= used)
				return rec;
			while (at + 16 <= used) {
				const std::uint32_t type = r.le32(at);
				if (type == attr_end)
					break;
				const std::uint32_t length = r.le32(at + 4);
				if (length < 16 || length % 8 != 0 || at + length > used)
					return rec;
				ByteReader a(raw.subspan(at, length));
				const bool non_resident = a.u8(8);
				const std::size_t name_length = a.u8(9), name_offset = a.le16(10);
				const std::uint16_t flags = a.le16(12);
				std::u16string attr_name;
				if (name_length > 0) {
					if (name_offset + 2 * name_length > length)
						return rec;
					for (std::size_t i = 0; i < name_length; i++)
						attr_name += static_cast<char16_t>(a.le16(name_offset + 2 * i));
				}
				std::span<const std::byte> value;
				if (!non_resident) {
					const std::size_t value_length = a.le32(16), value_offset = a.le16(20);
					if (value_offset > length || value_length > length - value_offset)
						return rec;
					value = raw.subspan(at + value_offset, value_length);
				}
				ByteReader v(value);
				if (type == attr_standard_information && !non_resident && value.size() >= 48) {
					rec.created = filetime(v.le64(0));
					rec.modified = filetime(v.le64(8));
					rec.accessed = filetime(v.le64(24));
					rec.attributes = v.le32(32);
					rec.has_times = true;
				} else if (type == attr_file_name && !non_resident && value.size() >= 66) {
					t_name n;
					n.parent = v.le64(0);
					n.name_space = v.u8(65);
					const std::size_t chars = v.u8(64);
					if (66 + 2 * chars > value.size())
						return rec;
					for (std::size_t i = 0; i < chars; i++)
						n.name += static_cast<char16_t>(v.le16(66 + 2 * i));
					if (!rec.has_times) {
						rec.created = filetime(v.le64(8));
						rec.modified = filetime(v.le64(16));
						rec.accessed = filetime(v.le64(32));
						rec.attributes = v.le32(56);
					}
					rec.names.push_back(std::move(n));
				} else if ((type == attr_volume_name || type == attr_volume_information) && !non_resident) {
					rec.volume.emplace_back(type, std::vector<std::byte>(value.begin(), value.end()));
				} else if (type == attr_data) {
					t_stream* stream = nullptr;
					for (t_stream& s : rec.streams)
						if (s.name == attr_name)
							stream = &s;
					if (!stream) {
						rec.streams.push_back({});
						stream = &rec.streams.back();
						stream->name = attr_name;
					}
					stream->flags |= flags;
					if (!non_resident) {
						stream->resident = true;
						stream->bytes.assign(value.begin(), value.end());
						stream->size = stream->initialized = value.size();
					} else {
						t_piece piece;
						piece.start_vcn = a.le64(16);
						const std::size_t runs_offset = a.le16(32);
						if (piece.start_vcn == 0) {
							stream->compression_unit = a.le16(34);
							stream->size = a.le64(48);
							stream->initialized = a.le64(56);
						}
						if (runs_offset >= length)
							return rec;
						piece.runs = decode_runs(raw.subspan(at + runs_offset, length - runs_offset), piece.start_vcn, v_.cluster_count, piece.problem);
						stream->pieces.push_back(std::move(piece));
					}
				}
				if (!a.ok() || !v.ok())
					return rec;
				at += length;
			}
			rec.present = true;
			return rec;
		}

		// attributes that didn't fit in a record are in extension records that point back at it: their names and data
		// pieces belong to the base
		void merge_extensions() {
			for (std::uint64_t n = 0; n < records_.size(); n++) {
				t_record& ext = records_[static_cast<std::size_t>(n)];
				if (!ext.present || ext.base == 0)
					continue;
				const std::uint64_t b = record_number(ext.base);
				if (b >= records_.size() || b == n || !records_[static_cast<std::size_t>(b)].present || records_[static_cast<std::size_t>(b)].sequence != sequence_of(ext.base))
					continue;
				t_record& base = records_[static_cast<std::size_t>(b)];
				for (t_name& name : ext.names)
					base.names.push_back(std::move(name));
				for (t_stream& s : ext.streams) {
					t_stream* into = nullptr;
					for (t_stream& mine : base.streams)
						if (mine.name == s.name)
							into = &mine;
					if (!into) {
						base.streams.push_back(std::move(s));
						continue;
					}
					into->flags |= s.flags;
					for (t_piece& p : s.pieces) {
						if (p.start_vcn == 0) {
							into->size = s.size;
							into->initialized = s.initialized;
							into->compression_unit = s.compression_unit;
						}
						into->pieces.push_back(std::move(p));
					}
				}
				ext.names.clear();
				ext.streams.clear();
			}
			for (t_record& r : records_)
				for (t_stream& s : r.streams)
					std::ranges::sort(s.pieces, {}, &t_piece::start_vcn);
		}

		IBlockSource& source_;
		NtfsScan& scan_;
		NtfsVolume& v_;
		const CancellationToken* cancel_;
		t_stream mft_;
		std::vector<t_record> records_;
		std::vector<std::byte> bitmap_;
		std::vector<std::vector<std::uint64_t>> children_;
		std::vector<std::uint64_t> parent_of_;
		std::vector<bool> visited_;
		std::vector<fsutil::Claim> claims_;
	};

	std::optional<NtfsVolume> parse_boot(std::span<const std::byte> sector, std::uint64_t source_size, std::string& why, std::vector<std::string>& notes) {
		ByteReader r(sector);
		if (!r.matches(3, "NTFS    ")) {
			why = "it doesn't say NTFS";
			return std::nullopt;
		}
		if (r.le16(510) != 0xAA55) {
			why = "no boot signature";
			return std::nullopt;
		}
		NtfsVolume v;
		v.bytes_per_sector = r.le16(11);
		const std::uint8_t spc = r.u8(13);
		v.sectors_per_cluster = spc <= 0x80 ? spc : (spc >= 0xF4 ? 1u << (256 - spc) : 0); // past 128 it's a power of 2
		v.total_sectors = r.le64(40);
		v.mft_cluster = r.le64(48);
		v.mft_mirror_cluster = r.le64(56);
		const auto per_record = static_cast<std::int8_t>(r.u8(64));
		v.serial = r.le64(72);
		const std::uint32_t bps = v.bytes_per_sector, sectors = v.sectors_per_cluster;
		if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) {
			why = std::format("{} bytes per sector isn't 512, 1024, 2048 or 4096", bps);
			return std::nullopt;
		}
		if (sectors == 0 || (sectors & (sectors - 1)) != 0 || std::uint64_t(bps) * sectors > (2u << 20)) {
			why = std::format("its cluster size ({} sectors) isn't a power of 2 up to 2 MiB", sectors);
			return std::nullopt;
		}
		v.cluster_size = bps * sectors;
		if (per_record > 0)
			v.record_size = static_cast<std::uint32_t>(per_record) * v.cluster_size > 65536 ? 0 : static_cast<std::uint32_t>(per_record) * v.cluster_size;
		else if (per_record >= -16 && per_record <= -9)
			v.record_size = 1u << -per_record;
		if (v.record_size < 512 || v.record_size > 65536 || (v.record_size & (v.record_size - 1)) != 0) {
			why = "its MFT record size isn't 512 bytes to 64 KiB";
			return std::nullopt;
		}
		v.cluster_count = v.total_sectors / sectors;
		if (v.cluster_count == 0 || v.mft_cluster >= v.cluster_count || v.mft_mirror_cluster >= v.cluster_count) {
			why = std::format("its MFT (cluster {}) or its mirror (cluster {}) is past its end ({} clusters)", v.mft_cluster, v.mft_mirror_cluster, v.cluster_count);
			return std::nullopt;
		}
		if (v.total_sectors * bps > source_size)
			notes.push_back(std::format("the volume is {} bytes but only {} are there: the image may be truncated, clusters past its end can't be read", v.total_sectors * bps, source_size));
		return v;
	}

	Result<NtfsScan> scan(IBlockSource& source, const CancellationToken* cancel) {
		if (source.size_bytes() < 1024)
			return fail(Errc::unsupported, "scanning NTFS: the volume is smaller than its boot sector");
		std::vector<std::byte> boot(512);
		if (Result<void> read = read_exact(source, 0, boot); !read)
			return std::unexpected(read.error());

		NtfsScan result;
		std::vector<std::string> notes;
		std::string why;
		std::optional<NtfsVolume> volume = parse_boot(boot, source.size_bytes(), why, notes);
		if (!volume) {
			// the backup is in the volume's last sector, which the boot sector's count leaves out
			std::vector<std::string> backup_notes;
			std::string backup_why;
			for (const std::uint64_t bps : { 512ull, 4096ull }) {
				if (volume || source.size_bytes() < 2 * bps)
					continue;
				std::vector<std::byte> backup(512);
				if (read_exact(source, (source.size_bytes() / bps - 1) * bps, backup))
					volume = parse_boot(backup, source.size_bytes(), backup_why, backup_notes);
			}
			if (!volume)
				return fail(Errc::unsupported, "scanning NTFS: the boot sector isn't an NTFS one");
			volume->from_backup_boot_sector = true;
			notes = std::move(backup_notes);
			result.diagnostics.push_back({ Severity::warning, std::format("the boot sector isn't valid ({}), the backup in the volume's last sector is used", why) });
		}
		result.volume = *volume;
		for (std::string& n : notes)
			result.diagnostics.push_back({ Severity::info, std::move(n) });

		t_scanner scanner(source, result, cancel);
		if (Result<void> mft = scanner.load_mft(); !mft)
			return std::unexpected(mft.error());
		if (Result<void> records = scanner.read_records(); !records)
			return std::unexpected(records.error());
		scanner.compare_mirror();
		scanner.read_volume_record();
		if (Result<void> bitmap = scanner.load_bitmap(); !bitmap)
			return std::unexpected(bitmap.error());
		if (Result<void> built = scanner.build(); !built)
			return std::unexpected(built.error());
		return result;
	}
}

storage::Result<storage::NtfsScan> storage::scan_ntfs(IBlockSource& volume, const CancellationToken* cancel) noexcept {
	try {
		return scan(volume, cancel);
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "scanning NTFS");
	}
}
