#pragma once

// an ntfs volume put together from microsoft's ntfs developer notes and the linux-ntfs documentation: a boot sector and
// its backup, an mft with the system files fenrir reads ($MFT, $MFTMirr, $Volume, the root, $Bitmap) and stubs of the
// others, then files written to it and deleted the way windows deletes them: the record's in-use flag is cleared, its
// sequence number goes up by one and its clusters are freed in $Bitmap. directories' indexes aren't kept up to date,
// fenrir doesn't read them (it builds the tree from each record's parent reference). none of storage_core's code is used

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "fat32.h"
#include "fixtures.h"

namespace fixtures {
	class t_ntfs {
	public:
		static constexpr std::uint32_t bps = 512, cluster = 4096, record_size = 1024;
		static constexpr std::uint64_t mft_lcn = 16, mft_records = 64, mirror_lcn = 8, bitmap_lcn = 40, root = 5;

		struct t_run {
			std::uint64_t lcn = 0, length = 0;
			bool sparse = false;
		};
		struct t_options {
			bool resident = false;
			std::uint64_t initialized = UINT64_MAX; // bytes written, the rest reads as zeros
			std::uint16_t data_flags = 0;           // 0x0001 compressed, 0x4000 encrypted
			std::u16string dos_name;                // an 8.3 name kept as its own file name attribute
		};
		struct t_file {
			std::uint64_t record = 0;
			std::vector<std::byte> content; // on the disk, or in the record
			std::vector<t_run> runs;
		};

		explicit t_ntfs(std::size_t bytes) {
			image_.assign(bytes, std::byte{ 0 });
			total_sectors_ = bytes / bps - 1; // the backup boot sector is past the volume's own count
			clusters_ = total_sectors_ / (cluster / bps);
			bitmap_.assign(static_cast<std::size_t>(clusters_), false);
			records_.assign(mft_records, {});
			use({ { 0, 2 }, { 4, 2 }, { mirror_lcn, 1 }, { mft_lcn, mft_records * record_size / cluster }, { bitmap_lcn, 1 } });

			system(0, u"$MFT", { nonresident(0x80, u"", { { mft_lcn, mft_records * record_size / cluster } }, mft_records * record_size, UINT64_MAX, 0) });
			system(1, u"$MFTMirr", { nonresident(0x80, u"", { { mirror_lcn, 1 } }, cluster, UINT64_MAX, 0) });
			system(2, u"$LogFile", { nonresident(0x80, u"", { { 4, 2 } }, 2 * cluster, UINT64_MAX, 0) });
			std::vector<std::byte> label;
			for (char16_t c : std::u16string_view(u"FENRIR NT"))
				label.insert(label.end(), { std::byte(c & 0xFF), std::byte(c >> 8) });
			std::vector<std::byte> info(12);
			info[8] = std::byte{ 3 };
			info[9] = std::byte{ 1 };
			system(3, u"$Volume", { resident(0x60, u"", label), resident(0x70, u"", info), resident(0x80, u"", {}) });
			system(4, u"$AttrDef", { resident(0x80, u"", {}) });
			records_[root] = { true, 1, 0x0003, 0, { standard_information(0x06), file_name(root, 1, u".", 2, true, 0), index_root() } };
			const std::uint64_t bitmap_bytes = (clusters_ + 7) / 8;
			system(6, u"$Bitmap", { nonresident(0x80, u"", { { bitmap_lcn, 1 } }, bitmap_bytes, UINT64_MAX, 0) });
			system(7, u"$Boot", { nonresident(0x80, u"", { { 0, 2 } }, 2 * cluster, UINT64_MAX, 0) });
			system(8, u"$BadClus", { resident(0x80, u"", {}) });
			system(9, u"$Secure", {});
			system(10, u"$UpCase", { resident(0x80, u"", {}) });
			records_[11] = { true, 1, 0x0003, 0, { standard_information(0x06), file_name(root, 1, u"$Extend", 3, true, 0), index_root() } };
			// what windows keeps in $Extend, which has to stay out of the listing
			records_[24] = { true, 1, 0x0001, 0, { standard_information(0x06), file_name(11, 1, u"$Quota", 3, false, 0), resident(0x80, u"", {}) } };
		}

		std::uint64_t cluster_count() const { return clusters_; }
		static std::size_t cluster_offset(std::uint64_t lcn) { return static_cast<std::size_t>(lcn * cluster); }
		static std::size_t record_offset(std::uint64_t record) { return cluster_offset(mft_lcn) + static_cast<std::size_t>(record * record_size); }

		t_file add_file(std::uint64_t parent, std::u16string_view name, std::vector<std::byte> content, std::vector<t_run> runs, const t_options& options = {}) {
			return add_file_at(next_++, parent, name, std::move(content), std::move(runs), options);
		}

		// at a given record: one deleted earlier, which the new file reuses
		t_file add_file_at(std::uint64_t record, std::uint64_t parent, std::u16string_view name, std::vector<std::byte> content, std::vector<t_run> runs,
			const t_options& options = {}) {
			std::vector<std::vector<std::byte>> attrs{ standard_information(0x20) };
			if (!options.dos_name.empty()) {
				attrs.push_back(file_name(parent, sequence(parent), name, 1, false, content.size()));
				attrs.push_back(file_name(parent, sequence(parent), options.dos_name, 2, false, content.size()));
			} else {
				attrs.push_back(file_name(parent, sequence(parent), name, 3, false, content.size()));
			}
			attrs.push_back(data(content, runs, options));
			const std::uint16_t seq = records_.at(record).sequence == 0 ? 1 : records_[record].sequence;
			records_[record] = { true, seq, 0x0001, 0, std::move(attrs), options.resident ? std::vector<t_run>{} : runs };
			return { record, std::move(content), std::move(runs) };
		}

		std::uint64_t add_dir(std::uint64_t parent, std::u16string_view name) {
			const std::uint64_t record = next_++;
			records_.at(record) = { true, 1, 0x0003, 0, { standard_information(0x10), file_name(parent, sequence(parent), name, 3, true, 0), index_root() } };
			return record;
		}

		// another $DATA attribute, named, in the file's own record
		void add_stream(const t_file& file, std::u16string_view name, const std::vector<std::byte>& content) {
			records_.at(file.record).attrs.push_back(resident(0x80, name, content));
		}

		// a file whose $DATA didn't fit: it's in an extension record pointing back at the base, which lists it
		t_file add_split_file(std::uint64_t parent, std::u16string_view name, std::vector<std::byte> content, std::vector<t_run> runs) {
			const std::uint64_t base = next_++, extension = next_++;
			std::vector<std::byte> list;
			auto entry = [&](std::uint32_t type, std::uint64_t in_record) {
				const std::size_t at = list.size();
				list.resize(at + 32);
				put_le(list, at, type, 4);
				put_le(list, at + 4, 32, 2);
				put_le(list, at + 16, in_record | std::uint64_t(1) << 48, 8);
			};
			entry(0x10, base);
			entry(0x30, base);
			entry(0x80, extension);
			records_.at(base) = { true, 1, 0x0001, 0, { standard_information(0x20), file_name(parent, sequence(parent), name, 3, false, content.size()), resident(0x20, u"", list) }, runs };
			records_.at(extension) = { true, 1, 0x0001, base | std::uint64_t(1) << 48, { data(content, runs, {}) } };
			return { base, std::move(content), std::move(runs) };
		}

		// points a record's file name at another parent: damage, for tests
		void reparent(std::uint64_t record, std::uint64_t parent) {
			put_le(records_.at(record).attrs.at(1), 24, parent | std::uint64_t(sequence(parent)) << 48, 8);
		}

		// windows: the record isn't in use any more and its sequence number goes up, its clusters are free
		void remove(std::uint64_t record) {
			t_record& r = records_.at(record);
			r.flags &= static_cast<std::uint16_t>(~0x0001);
			r.sequence++;
			for (const t_run& run : r.runs)
				for (std::uint64_t c = run.lcn; !run.sparse && c < run.lcn + run.length; c++)
					bitmap_.at(c) = false;
		}
		void remove(const t_file& file) { remove(file.record); }

		t_image finish(std::uint64_t hidden_sectors) {
			// the mft's own bitmap of records in use
			std::vector<std::byte> in_use(8);
			for (std::uint64_t n = 0; n < mft_records; n++)
				if (records_[n].used && (records_[n].flags & 1))
					in_use[n / 8] |= std::byte(1u << (n % 8));
			records_[0].attrs.push_back(resident(0xB0, u"", in_use));

			for (std::uint64_t n = 0; n < mft_records; n++)
				if (records_[n].used)
					write_record(n, record_offset(n));
			for (std::uint64_t n = 0; n < 4; n++)
				std::copy_n(image_.begin() + static_cast<std::ptrdiff_t>(record_offset(n)), record_size, image_.begin() + static_cast<std::ptrdiff_t>(cluster_offset(mirror_lcn) + n * record_size));
			for (std::uint64_t c = 0; c < clusters_; c++)
				if (bitmap_[static_cast<std::size_t>(c)])
					image_.at(cluster_offset(bitmap_lcn) + static_cast<std::size_t>(c / 8)) |= std::byte(1u << (c % 8));

			for (std::size_t at : { std::size_t(0), static_cast<std::size_t>(total_sectors_ * bps) }) {
				put8(image_, at, 0xEB); put8(image_, at + 1, 0x52); put8(image_, at + 2, 0x90);
				put_text(image_, at + 3, "NTFS    ");
				put16(image_, at + 11, bps);
				put8(image_, at + 13, cluster / bps);
				put8(image_, at + 21, 0xF8);
				put16(image_, at + 24, 63);
				put16(image_, at + 26, 255);
				put32(image_, at + 28, static_cast<std::uint32_t>(hidden_sectors));
				put32(image_, at + 36, 0x00800080);
				put64(image_, at + 40, total_sectors_);
				put64(image_, at + 48, mft_lcn);
				put64(image_, at + 56, mirror_lcn);
				put8(image_, at + 64, 0xF6); // -10: records of 2^10 bytes
				put8(image_, at + 68, 1);
				put64(image_, at + 72, 0x2026A5F1C0DE4E7Full);
				put8(image_, at + 510, 0x55); put8(image_, at + 511, 0xAA);
			}
			return image_;
		}

		// 2026-10-31 12:34:56.789 utc, counted the slow way from 1601
		static std::uint64_t when() {
			std::uint64_t days = 0;
			for (int y = 1601; y < 2026; y++)
				days += (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 ? 366 : 365;
			days += 31 + 28 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 30; // to october 31st
			return ((days * 86400 + 12 * 3600 + 34 * 60 + 56) * 1000 + 789) * 10000;
		}

	private:
		struct t_record {
			bool used = false;
			std::uint16_t sequence = 0, flags = 0;
			std::uint64_t base = 0;
			std::vector<std::vector<std::byte>> attrs;
			std::vector<t_run> runs; // what remove() frees
		};

		void use(const std::vector<t_run>& runs) {
			for (const t_run& run : runs)
				for (std::uint64_t c = run.lcn; !run.sparse && c < run.lcn + run.length; c++) {
					if (bitmap_.at(c))
						throw std::logic_error("cluster already in use");
					bitmap_[c] = true;
				}
		}

		std::uint16_t sequence(std::uint64_t record) const { return records_.at(record).sequence; }

		void system(std::uint64_t n, std::u16string_view name, std::vector<std::vector<std::byte>> more) {
			std::vector<std::vector<std::byte>> attrs{ standard_information(0x06), file_name(root, 1, name, 3, false, 0) };
			for (auto& a : more)
				attrs.push_back(std::move(a));
			records_[n] = { true, 1, 0x0001, 0, std::move(attrs) };
		}

		static std::size_t align8(std::size_t n) { return (n + 7) / 8 * 8; }

		static std::vector<std::byte> resident(std::uint32_t type, std::u16string_view name, const std::vector<std::byte>& value) {
			const std::size_t value_at = align8(24 + 2 * name.size()), length = align8(value_at + value.size());
			std::vector<std::byte> a(length);
			put_le(a, 0, type, 4);
			put_le(a, 4, length, 4);
			put_le(a, 9, name.size(), 1);
			put_le(a, 10, 24, 2);
			put_le(a, 16, value.size(), 4);
			put_le(a, 20, value_at, 2);
			put_le(a, 22, type == 0x30 ? 1 : 0, 1);
			for (std::size_t i = 0; i < name.size(); i++)
				put_le(a, 24 + 2 * i, name[i], 2);
			std::copy(value.begin(), value.end(), a.begin() + static_cast<std::ptrdiff_t>(value_at));
			return a;
		}

		// each run: a header byte with the sizes of its length and offset, then both, the offset from the run before
		static std::vector<std::byte> encode(const std::vector<t_run>& runs) {
			std::vector<std::byte> out;
			std::int64_t last = 0;
			for (const t_run& run : runs) {
				std::size_t length_size = 1;
				while (length_size < 8 && (run.length >> (8 * length_size)) != 0)
					length_size++;
				std::size_t offset_size = 0;
				std::int64_t delta = 0;
				if (!run.sparse) {
					delta = static_cast<std::int64_t>(run.lcn) - last;
					last = static_cast<std::int64_t>(run.lcn);
					offset_size = 1;
					while (offset_size < 8 && (delta < -(std::int64_t(1) << (8 * offset_size - 1)) || delta >= (std::int64_t(1) << (8 * offset_size - 1))))
						offset_size++;
				}
				out.push_back(std::byte(offset_size << 4 | length_size));
				for (std::size_t i = 0; i < length_size; i++)
					out.push_back(std::byte(run.length >> (8 * i)));
				for (std::size_t i = 0; i < offset_size; i++)
					out.push_back(std::byte(static_cast<std::uint64_t>(delta) >> (8 * i)));
			}
			out.push_back(std::byte{ 0 });
			return out;
		}

		static std::vector<std::byte> nonresident(std::uint32_t type, std::u16string_view name, const std::vector<t_run>& runs, std::uint64_t size,
			std::uint64_t initialized, std::uint16_t flags) {
			const bool compressed = flags & 0x0001;
			const std::size_t name_at = compressed ? 72 : 64, runs_at = align8(name_at + 2 * name.size());
			const std::vector<std::byte> encoded = encode(runs);
			const std::size_t length = align8(runs_at + encoded.size());
			std::uint64_t clusters = 0;
			for (const t_run& run : runs)
				clusters += run.length;
			std::vector<std::byte> a(length);
			put_le(a, 0, type, 4);
			put_le(a, 4, length, 4);
			put_le(a, 8, 1, 1);
			put_le(a, 9, name.size(), 1);
			put_le(a, 10, name_at, 2);
			put_le(a, 12, flags, 2);
			put_le(a, 24, clusters == 0 ? 0 : clusters - 1, 8);
			put_le(a, 32, runs_at, 2);
			put_le(a, 34, compressed ? 4 : 0, 2);
			put_le(a, 40, clusters * cluster, 8);
			put_le(a, 48, size, 8);
			put_le(a, 56, std::min(initialized, size), 8);
			if (compressed)
				put_le(a, 64, clusters * cluster, 8);
			for (std::size_t i = 0; i < name.size(); i++)
				put_le(a, name_at + 2 * i, name[i], 2);
			std::copy(encoded.begin(), encoded.end(), a.begin() + static_cast<std::ptrdiff_t>(runs_at));
			return a;
		}

		std::vector<std::byte> data(const std::vector<std::byte>& content, const std::vector<t_run>& runs, const t_options& options) {
			if (options.resident)
				return resident(0x80, u"", content);
			use(runs);
			std::size_t at = 0;
			for (const t_run& run : runs) {
				for (std::size_t i = 0; i < run.length * cluster && at < content.size(); i++, at++)
					if (!run.sparse)
						image_.at(cluster_offset(run.lcn) + i) = content[at];
			}
			return nonresident(0x80, u"", runs, content.size(), options.initialized, options.data_flags);
		}

		static std::vector<std::byte> standard_information(std::uint32_t attributes) {
			std::vector<std::byte> v(72);
			for (std::size_t at : { 0, 8, 16, 24 })
				put_le(v, at, when(), 8);
			put_le(v, 32, attributes, 4);
			return resident(0x10, u"", v);
		}

		static std::vector<std::byte> file_name(std::uint64_t parent, std::uint16_t parent_sequence, std::u16string_view name, std::uint8_t name_space,
			bool directory, std::uint64_t size) {
			std::vector<std::byte> v(66 + 2 * name.size());
			put_le(v, 0, parent | std::uint64_t(parent_sequence) << 48, 8);
			for (std::size_t at : { 8, 16, 24, 32 })
				put_le(v, at, when(), 8);
			put_le(v, 40, (size + cluster - 1) / cluster * cluster, 8);
			put_le(v, 48, size, 8);
			put_le(v, 56, directory ? 0x10000000u : 0x20u, 4);
			put_le(v, 64, name.size(), 1);
			put_le(v, 65, name_space, 1);
			for (std::size_t i = 0; i < name.size(); i++)
				put_le(v, 66 + 2 * i, name[i], 2);
			return resident(0x30, u"", v);
		}

		// an empty index of file names, "$I30", in the directory's own record
		static std::vector<std::byte> index_root() {
			std::vector<std::byte> v(48);
			put_le(v, 0, 0x30, 4);
			put_le(v, 4, 1, 4);
			put_le(v, 8, cluster, 4);
			put_le(v, 12, 1, 1);
			put_le(v, 16, 16, 4);
			put_le(v, 20, 32, 4);
			put_le(v, 24, 32, 4);
			put_le(v, 32 + 8, 16, 2);
			put_le(v, 32 + 12, 2, 4); // the last entry
			return resident(0x90, u"$I30", v);
		}

		static void put_le(std::vector<std::byte>& v, std::size_t at, std::uint64_t value, int width) {
			for (int i = 0; i < width; i++)
				v.at(at + static_cast<std::size_t>(i)) = std::byte(value >> (8 * i));
		}

		// the header, the attributes and the end marker, then the update sequence over the last 2 bytes of every 512
		void write_record(std::uint64_t n, std::size_t at) {
			const t_record& r = records_[n];
			std::vector<std::byte> rec(record_size);
			std::size_t pos = 56;
			for (std::size_t i = 0; i < r.attrs.size(); i++) {
				std::vector<std::byte> a = r.attrs[i];
				put_le(a, 14, i, 2);
				if (pos + a.size() + 8 > record_size)
					throw std::logic_error("the record is full");
				std::copy(a.begin(), a.end(), rec.begin() + static_cast<std::ptrdiff_t>(pos));
				pos += a.size();
			}
			put_le(rec, pos, 0xFFFFFFFF, 4);
			pos += 8;
			put_text(rec, 0, "FILE");
			put_le(rec, 4, 48, 2);
			put_le(rec, 6, 3, 2);
			put_le(rec, 16, r.sequence, 2);
			put_le(rec, 18, 1, 2);
			put_le(rec, 20, 56, 2);
			put_le(rec, 22, r.flags, 2);
			put_le(rec, 24, pos, 4);
			put_le(rec, 28, record_size, 4);
			put_le(rec, 32, r.base, 8);
			put_le(rec, 40, r.attrs.size(), 2);
			put_le(rec, 44, n, 4);
			const std::uint16_t usn = static_cast<std::uint16_t>(0x0100 + n);
			put_le(rec, 48, usn, 2);
			for (std::size_t i = 1; i <= 2; i++) {
				const std::size_t end = i * 512 - 2;
				rec[48 + 2 * i] = rec[end];
				rec[48 + 2 * i + 1] = rec[end + 1];
				put_le(rec, end, usn, 2);
			}
			std::copy(rec.begin(), rec.end(), image_.begin() + static_cast<std::ptrdiff_t>(at));
		}

		static void put_text(std::vector<std::byte>& v, std::size_t at, std::string_view text) {
			for (std::size_t i = 0; i < text.size(); i++)
				v.at(at + i) = std::byte(text[i]);
		}

		t_image image_;
		std::uint64_t total_sectors_ = 0, clusters_ = 0;
		std::vector<bool> bitmap_;
		std::vector<t_record> records_;
		std::uint64_t next_ = 25;
	};

	// what a scan of ntfs_scenario() has to find, like t_fat32_expected
	using t_ntfs_expected = t_fat32_expected;
	struct t_ntfs_scenario {
		t_image volume;
		std::vector<t_ntfs_expected> expected; // in the order a scan lists them
	};

	// 16 MiB, 4 KiB clusters, 1 KiB records. every case the ntfs scanner tells apart
	inline t_ntfs_scenario ntfs_scenario(std::uint64_t hidden_sectors = 0) {
		using t_run = t_ntfs::t_run;
		t_ntfs fs(16 * mib);
		const std::uint64_t root = t_ntfs::root;
		std::vector<t_ntfs_expected> e;
		auto expect = [&e](std::string path, bool deleted, std::string integrity, std::uint64_t recoverable, const std::vector<std::byte>& original, bool matches) {
			e.push_back({ std::move(path), deleted, false, std::move(integrity), recoverable, original, matches });
		};

		const auto readme = fs.add_file(root, u"readme.txt", text("read me", 6000), { { 100, 2 } });
		expect("/readme.txt", false, "intact", 6000, readme.content, true);

		// small enough to live in the record, across the bytes the update sequence protects
		const auto tiny = fs.add_file(root, u"tiny.txt", text("tiny", 300), {}, { .resident = true });
		expect("/tiny.txt", false, "intact", 300, tiny.content, true);
		const auto gone_resident = fs.add_file(root, u"deleted resident.txt", text("in the record", 200), {}, { .resident = true });
		fs.remove(gone_resident);
		expect("/deleted resident.txt", true, "intact", 200, gone_resident.content, true);

		const auto report = fs.add_file(root, u"report.pdf", noise(21, 20000), { { 110, 5 } });
		fs.remove(report);
		expect("/report.pdf", true, "intact", 20000, report.content, true);

		// in three pieces, the last one before the second: the record still has every run
		const auto fragmented = fs.add_file(root, u"fragmented.bin", noise(22, 15000), { { 120, 2 }, { 130, 1 }, { 125, 1 } });
		fs.remove(fragmented);
		expect("/fragmented.bin", true, "intact", 15000, fragmented.content, true);

		// in two pieces, and a new file in the second one
		const auto interrupted = fs.add_file(root, u"interrupted.bin", noise(23, 15000), { { 140, 2 }, { 150, 2 } });
		fs.remove(interrupted);
		expect("/interrupted.bin", true, "partial", 12288, interrupted.content, true);
		const auto squatter = fs.add_file(root, u"squatter.txt", text("mine now", 3000), { { 151, 1 } });
		expect("/squatter.txt", false, "intact", 3000, squatter.content, true);

		const auto overwritten = fs.add_file(root, u"overwritten.bin", noise(24, 8000), { { 160, 2 } });
		fs.remove(overwritten);
		expect("/overwritten.bin", true, "unrecoverable", 0, overwritten.content, false);
		const auto fresh = fs.add_file(root, u"new.bin", noise(25, 4000), { { 160, 1 } });
		expect("/new.bin", false, "intact", 4000, fresh.content, true);

		const std::uint64_t photos = fs.add_dir(root, u"Photos");
		e.push_back({ "/Photos", false, true, "intact", 0, {}, false });
		const auto photo = fs.add_file(photos, u"IMG_0001.jpg", noise(26, 9000), { { 170, 3 } });
		fs.remove(photo);
		expect("/Photos/IMG_0001.jpg", true, "intact", 9000, photo.content, true);

		// a directory and the file in it, the file deleted first: the file's parent reference has the directory's
		// sequence number from before it was deleted
		const std::uint64_t old = fs.add_dir(root, u"old stuff");
		const auto letter = fs.add_file(old, u"a letter.txt", text("dear wolf", 1000), { { 180, 1 } });
		fs.remove(letter);
		fs.remove(old);
		e.push_back({ "/old stuff", true, true, "intact", 0, {}, false });
		expect("/old stuff/a letter.txt", true, "intact", 1000, letter.content, true);

		// a deleted directory whose record a new file took: what was in it has nowhere to go but $OrphanFiles
		const std::uint64_t gone = fs.add_dir(root, u"gone");
		const auto lost = fs.add_file(gone, u"lost.txt", text("where am i", 2000), { { 190, 1 } });
		fs.remove(lost);
		fs.remove(gone);
		const auto reused = fs.add_file_at(gone, root, u"reused.txt", text("my record now", 100), {}, { .resident = true });
		expect("/reused.txt", false, "intact", 100, reused.content, true);

		// sparse: the middle two clusters were never stored
		std::vector<std::byte> sparse = noise(27, 16384);
		std::fill(sparse.begin() + 4096, sparse.begin() + 12288, std::byte{ 0 });
		const auto vhd = fs.add_file(root, u"sparse.vhd", sparse, { { 200, 1 }, { 0, 2, true }, { 201, 1 } });
		expect("/sparse.vhd", false, "intact", 16384, vhd.content, true);

		const auto compressed = fs.add_file(root, u"compressed.log", text("squeezed", 4000), { { 210, 1 } }, { .data_flags = 0x0001 });
		fs.remove(compressed);
		expect("/compressed.log", true, "unrecoverable", 0, compressed.content, false);

		const auto download = fs.add_file(root, u"download.exe", noise(28, 3000), { { 220, 1 } });
		const std::vector<std::byte> zone = text("[ZoneTransfer] ZoneId=3", 23);
		fs.add_stream(download, u"Zone.Identifier", zone);
		expect("/download.exe", false, "intact", 3000, download.content, true);
		expect("/download.exe:Zone.Identifier", false, "intact", 23, zone, true);

		const auto rune = fs.add_file(root, u"Fenrir – ᚠ.txt", text("bound by gleipnir", 700), { { 230, 1 } }, { .dos_name = u"FENRIR~1.TXT" });
		fs.remove(rune);
		expect("/Fenrir \xE2\x80\x93 \xE1\x9A\xA0.txt", true, "intact", 700, rune.content, true);

		// deleted, then another file written to the same cluster and deleted too: both claim it
		const auto first = fs.add_file(root, u"first.log", text("first", 6000), { { 240, 2 } });
		fs.remove(first);
		const auto second = fs.add_file(root, u"second.log", text("second", 3000), { { 240, 1 } });
		fs.remove(second);
		expect("/first.log", true, "uncertain", 6000, first.content, false);
		expect("/second.log", true, "uncertain", 3000, second.content, true);

		// 12 KiB allocated, only the first 5000 bytes written: the rest reads as zeros, whatever the clusters hold
		const auto pre = fs.add_file(root, u"preallocated.dat", noise(29, 12288), { { 250, 3 } }, { .initialized = 5000 });
		std::vector<std::byte> zeroed = pre.content;
		std::fill(zeroed.begin() + 5000, zeroed.end(), std::byte{ 0 });
		expect("/preallocated.dat", false, "intact", 12288, zeroed, true);

		const auto big = fs.add_split_file(root, u"big list.txt", text("a long list", 7000), { { 260, 2 } });
		expect("/big list.txt", false, "intact", 7000, big.content, true);

		const auto secret = fs.add_file(root, u"secret.doc", noise(30, 2000), { { 270, 1 } }, { .data_flags = 0x4000 });
		expect("/secret.doc", false, "unrecoverable", 0, secret.content, false);

		expect("/$OrphanFiles/lost.txt", true, "intact", 2000, lost.content, true);
		return { fs.finish(hidden_sectors), std::move(e) };
	}

	// ntfs_scenario() in partition 1 of an 18 MiB mbr disk, at sector 2048, type 0x07
	inline t_image ntfs_disk() {
		t_image disk(18 * mib);
		const t_image volume = ntfs_scenario(2048).volume;
		put_mbr(disk, 0, { t_mbr_entry{ 0x80, 0x07, 2048, static_cast<std::uint32_t>(volume.size() / 512) } }, 0x4E544653);
		std::copy(volume.begin(), volume.end(), disk.begin() + 2048 * 512);
		return disk;
	}
}
