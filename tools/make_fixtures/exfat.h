#pragma once

// an exfat volume put together from microsoft's "exFAT file system specification", then files written to it and
// deleted the way windows deletes them: the in-use bit of each of the file's directory entries is cleared and its
// clusters are freed in the allocation bitmap. the fat is left as it was. none of storage_core's code is used

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "fat32.h"
#include "fixtures.h"

namespace fixtures {
	class t_exfat {
	public:
		static constexpr std::uint32_t bps = 512, fat_offset = 128, end_of_chain = 0xFFFFFFFF;

		struct t_dir {
			std::vector<std::uint32_t> clusters;
			bool contiguous = true;
			std::size_t used = 0;
			std::vector<std::size_t> entries; // its own entry set in its parent
		};
		struct t_file {
			std::vector<std::size_t> entries; // byte offsets in the volume of each entry of its set
			std::vector<std::uint32_t> clusters;
			std::vector<std::byte> content;   // what's on the disk in its clusters
		};

		explicit t_exfat(std::size_t bytes, std::uint32_t sectors_per_cluster = 8) : spc_(sectors_per_cluster) {
			image_.assign(bytes, std::byte{ 0 });
			total_ = bytes / bps;
			for (fat_length_ = 1;; fat_length_++) {
				heap_ = (fat_offset + fat_length_ + spc_ - 1) / spc_ * spc_;
				clusters_ = static_cast<std::uint32_t>((total_ - heap_) / spc_);
				if ((std::uint64_t(clusters_) + 2) * 4 <= std::uint64_t(fat_length_) * bps)
					break;
			}
			fat_.assign(clusters_ + 2, 0);
			fat_[0] = 0xFFFFFFF8;
			fat_[1] = end_of_chain;
			bitmap_.assign(clusters_ + 2, false);
			use({ 2 }, true);  // the allocation bitmap
			use({ 3 }, true);  // the up-case table
			root_.clusters = { 4 };
			root_.contiguous = false;
			root_.used = 2; // the bitmap's and the up-case table's entries, written by finish()
			use(root_.clusters, true);
		}

		t_dir& root() { return root_; }
		std::uint32_t cluster_size() const { return bps * spc_; }
		std::uint32_t cluster_count() const { return clusters_; }
		std::size_t cluster_offset(std::uint32_t c) const { return static_cast<std::size_t>((heap_ + std::uint64_t(c - 2) * spc_) * bps); }

		void put_label(std::u16string_view label) {
			const std::size_t at = slot(root_);
			put8(image_, at, 0x83);
			put8(image_, at + 1, static_cast<std::uint8_t>(label.size()));
			for (std::size_t i = 0; i < label.size(); i++)
				put16(image_, at + 2 + 2 * i, label[i]);
		}

		// content is what lands in the clusters, valid how much of it the file says was written (the rest of a file
		// reads as zeros). contiguous files get no fat chain, the way windows writes them
		t_file add_file(t_dir& dir, std::u16string_view name, std::vector<std::byte> content, std::vector<std::uint32_t> clusters, bool contiguous,
			std::uint64_t valid = UINT64_MAX) {
			if (clusters.size() * cluster_size() < content.size())
				throw std::logic_error("not enough clusters for the content");
			use(clusters, !contiguous);
			for (std::size_t i = 0; i < content.size(); i++)
				image_.at(cluster_offset(clusters[i / cluster_size()]) + i % cluster_size()) = content[i];
			t_file file;
			file.entries = put_set(dir, name, 0x20, clusters.empty() ? 0 : clusters[0], content.size(), std::min<std::uint64_t>(valid, content.size()), contiguous);
			file.clusters = std::move(clusters);
			file.content = std::move(content);
			return file;
		}

		t_dir add_dir(t_dir& parent, std::u16string_view name, std::uint32_t cluster) {
			t_dir dir;
			dir.clusters = { cluster };
			use(dir.clusters, false);
			dir.entries = put_set(parent, name, 0x10, cluster, cluster_size(), cluster_size(), true);
			return dir;
		}

		// windows: the entries' in-use bits and the bitmap. clear_fat for a driver that also frees the fat chain
		void remove(const std::vector<std::size_t>& entries, const std::vector<std::uint32_t>& clusters, bool clear_fat = false) {
			for (std::size_t at : entries)
				image_.at(at) &= std::byte{ 0x7F };
			for (std::uint32_t c : clusters) {
				bitmap_.at(c) = false;
				if (clear_fat)
					fat_.at(c) = 0;
			}
		}
		void remove(const t_file& file, bool clear_fat = false) { remove(file.entries, file.clusters, clear_fat); }
		void remove(const t_dir& dir) { remove(dir.entries, dir.clusters); }

		// both boot regions, the fat, the bitmap, the up-case table and the root's entries for them
		t_image finish(std::uint64_t partition_offset_sectors) {
			// a minimal up-case table: a to z map to A to Z, the rest of the first 128 characters to themselves
			std::vector<std::byte> upcase(256);
			for (std::uint16_t c = 0; c < 128; c++) {
				const auto up = static_cast<std::uint16_t>(c >= 'a' && c <= 'z' ? c - 32 : c);
				upcase[2 * c] = static_cast<std::byte>(up & 0xFF);
				upcase[2 * c + 1] = static_cast<std::byte>(up >> 8);
			}
			std::uint32_t upcase_sum = 0;
			for (std::byte b : upcase)
				upcase_sum = ((upcase_sum & 1) ? 0x80000000u : 0u) + (upcase_sum >> 1) + std::to_integer<std::uint32_t>(b);
			std::copy(upcase.begin(), upcase.end(), image_.begin() + static_cast<std::ptrdiff_t>(cluster_offset(3)));

			const std::size_t bitmap_bytes = (clusters_ + 7) / 8;
			for (std::uint32_t c = 2; c < clusters_ + 2; c++)
				if (bitmap_[c])
					image_.at(cluster_offset(2) + (c - 2) / 8) |= static_cast<std::byte>(1u << ((c - 2) % 8));

			const std::size_t r = cluster_offset(4);
			put8(image_, r, 0x81);
			put32(image_, r + 20, 2);
			put64(image_, r + 24, bitmap_bytes);
			put8(image_, r + 32, 0x82);
			put32(image_, r + 32 + 4, upcase_sum);
			put32(image_, r + 32 + 20, 3);
			put64(image_, r + 32 + 24, upcase.size());

			for (std::size_t c = 0; c < fat_.size(); c++)
				put32(image_, fat_offset * bps + c * 4, fat_[c]);

			std::uint32_t in_use = 0;
			for (std::uint32_t c = 2; c < clusters_ + 2; c++)
				in_use += bitmap_[c];
			for (std::size_t region : { std::size_t(0), std::size_t(12) }) {
				const std::size_t at = region * bps;
				put8(image_, at, 0xEB); put8(image_, at + 1, 0x76); put8(image_, at + 2, 0x90);
				put_text(image_, at + 3, "EXFAT   ");
				put64(image_, at + 64, partition_offset_sectors);
				put64(image_, at + 72, total_);
				put32(image_, at + 80, fat_offset);
				put32(image_, at + 84, fat_length_);
				put32(image_, at + 88, static_cast<std::uint32_t>(heap_));
				put32(image_, at + 92, clusters_);
				put32(image_, at + 96, 4);
				put32(image_, at + 100, 0x2026E7FA);
				put16(image_, at + 104, 0x0100);
				put16(image_, at + 106, 0);
				put8(image_, at + 108, 9);
				std::uint8_t shift = 0;
				while ((1u << shift) < spc_)
					shift++;
				put8(image_, at + 109, shift);
				put8(image_, at + 110, 1);
				put8(image_, at + 111, 0x80);
				put8(image_, at + 112, static_cast<std::uint8_t>(in_use * 100 / clusters_));
				put8(image_, at + 510, 0x55); put8(image_, at + 511, 0xAA);
				for (std::size_t s = 1; s <= 8; s++)
					put32(image_, at + s * bps + bps - 4, 0xAA550000);
				std::uint32_t sum = 0;
				for (std::size_t i = 0; i < 11 * bps; i++) {
					if (i == 106 || i == 107 || i == 112)
						continue;
					sum = ((sum & 1) ? 0x80000000u : 0u) + (sum >> 1) + std::to_integer<std::uint32_t>(image_[at + i]);
				}
				for (std::size_t i = 0; i < bps; i += 4)
					put32(image_, at + 11 * bps + i, sum);
			}
			return image_;
		}

	private:
		void use(const std::vector<std::uint32_t>& clusters, bool chained) {
			for (std::size_t i = 0; i < clusters.size(); i++) {
				if (bitmap_.at(clusters[i]))
					throw std::logic_error("cluster already in use");
				bitmap_[clusters[i]] = true;
				if (chained)
					fat_.at(clusters[i]) = i + 1 < clusters.size() ? clusters[i + 1] : end_of_chain;
			}
		}

		std::size_t slot(t_dir& dir) {
			const std::size_t per_cluster = cluster_size() / 32, index = dir.used++;
			if (index / per_cluster >= dir.clusters.size())
				throw std::logic_error("the directory is full");
			return cluster_offset(dir.clusters[index / per_cluster]) + index % per_cluster * 32;
		}

		// a file entry, its stream extension and its name entries, with the set's checksum. 2026-10-31 12:34:56 for
		// every time, 10 ms fields of 150 on created (so 57.5 s) and 0 on modified
		std::vector<std::size_t> put_set(t_dir& dir, std::u16string_view name, std::uint16_t attributes, std::uint32_t first, std::uint64_t length,
			std::uint64_t valid, bool contiguous) {
			const std::size_t names = (name.size() + 14) / 15;
			std::vector<std::size_t> at;
			for (std::size_t i = 0; i < 2 + names; i++)
				at.push_back(slot(dir));
			const std::uint32_t stamp = (2026u - 1980u) << 25 | 10u << 21 | 31u << 16 | 12u << 11 | 34u << 5 | 56u / 2;
			put8(image_, at[0], 0x85);
			put8(image_, at[0] + 1, static_cast<std::uint8_t>(1 + names));
			put16(image_, at[0] + 4, attributes);
			put32(image_, at[0] + 8, stamp);
			put32(image_, at[0] + 12, stamp);
			put32(image_, at[0] + 16, stamp);
			put8(image_, at[0] + 20, 150);
			put8(image_, at[0] + 21, 0);
			std::uint16_t hash = 0;
			for (char16_t c : name) {
				const auto up = static_cast<std::uint16_t>(c >= u'a' && c <= u'z' ? c - 32 : c);
				for (std::uint8_t b : { std::uint8_t(up & 0xFF), std::uint8_t(up >> 8) })
					hash = static_cast<std::uint16_t>(((hash & 1) ? 0x8000u : 0u) + (hash >> 1) + b);
			}
			const bool allocated = length != 0;
			put8(image_, at[1], 0xC0);
			put8(image_, at[1] + 1, static_cast<std::uint8_t>((allocated ? 1 : 0) | (allocated && contiguous ? 2 : 0)));
			put8(image_, at[1] + 3, static_cast<std::uint8_t>(name.size()));
			put16(image_, at[1] + 4, hash);
			put64(image_, at[1] + 8, valid);
			put32(image_, at[1] + 20, allocated ? first : 0);
			put64(image_, at[1] + 24, length);
			for (std::size_t n = 0; n < names; n++) {
				put8(image_, at[2 + n], 0xC1);
				for (std::size_t c = 0; c < 15 && n * 15 + c < name.size(); c++)
					put16(image_, at[2 + n] + 2 + 2 * c, name[n * 15 + c]);
			}
			std::uint16_t sum = 0;
			for (std::size_t e = 0; e < at.size(); e++)
				for (std::size_t i = 0; i < 32; i++) {
					if (e == 0 && (i == 2 || i == 3))
						continue;
					sum = static_cast<std::uint16_t>(((sum & 1) ? 0x8000u : 0u) + (sum >> 1) + std::to_integer<std::uint8_t>(image_[at[e] + i]));
				}
			put16(image_, at[0] + 2, sum);
			return at;
		}

		t_image image_;
		std::uint32_t spc_;
		std::uint64_t total_ = 0, heap_ = 0;
		std::uint32_t fat_length_ = 0, clusters_ = 0;
		std::vector<std::uint32_t> fat_;
		std::vector<bool> bitmap_;
		t_dir root_;
	};

	// what a scan of exfat_scenario() has to find, like t_fat32_expected
	using t_exfat_expected = t_fat32_expected;
	struct t_exfat_scenario {
		t_image volume;
		std::vector<t_exfat_expected> expected; // in the order a scan lists them
	};

	// 8 MiB, 4 KiB clusters. every case the exfat scanner tells apart, in the root, a live directory and a deleted one
	inline t_exfat_scenario exfat_scenario(std::uint64_t partition_offset_sectors = 0) {
		t_exfat fs(8 * mib);
		t_exfat::t_dir& root = fs.root();
		fs.put_label(u"FENRIR EX");
		std::vector<t_exfat_expected> e;
		auto expect = [&e](std::string path, bool deleted, std::string integrity, std::uint64_t recoverable, const std::vector<std::byte>& original, bool matches) {
			e.push_back({ std::move(path), deleted, false, std::move(integrity), recoverable, original, matches });
		};

		const auto readme = fs.add_file(root, u"readme.txt", text("read me", 6000), { 10, 11 }, true);
		expect("/readme.txt", false, "intact", 6000, readme.content, true);

		// in one piece, the entry says so
		const auto report = fs.add_file(root, u"report.pdf", noise(11, 20000), { 20, 21, 22, 23, 24 }, true);
		fs.remove(report);
		expect("/report.pdf", true, "intact", 20000, report.content, true);

		// in two pieces: the fat still has the chain
		const auto fragmented = fs.add_file(root, u"fragmented.bin", noise(12, 15000), { 30, 31, 50, 51 }, false);
		fs.remove(fragmented);
		expect("/fragmented.bin", true, "intact", 15000, fragmented.content, true);

		// in two pieces, and a new file in the second one
		const auto interrupted = fs.add_file(root, u"interrupted.bin", noise(13, 15000), { 60, 61, 70, 71 }, false);
		fs.remove(interrupted);
		expect("/interrupted.bin", true, "partial", 8192, interrupted.content, true);
		const auto squatter = fs.add_file(root, u"squatter.txt", text("mine now", 3000), { 70 }, true);
		expect("/squatter.txt", false, "intact", 3000, squatter.content, true);

		// deleted, a new file written over its start
		const auto overwritten = fs.add_file(root, u"overwritten.bin", noise(14, 8000), { 80, 81 }, true);
		fs.remove(overwritten);
		expect("/overwritten.bin", true, "unrecoverable", 0, overwritten.content, false);
		const auto fresh = fs.add_file(root, u"new.bin", noise(15, 4000), { 80 }, true);
		expect("/new.bin", false, "intact", 4000, fresh.content, true);

		t_exfat::t_dir photos = fs.add_dir(root, u"Photos", 5);
		e.push_back({ "/Photos", false, true, "intact", 0, {}, false });
		const auto photo = fs.add_file(photos, u"IMG_0001.jpg", noise(16, 9000), { 90, 91, 92 }, true);
		fs.remove(photo);
		expect("/Photos/IMG_0001.jpg", true, "intact", 9000, photo.content, true);

		// a directory and the file in it, the file deleted first
		t_exfat::t_dir old = fs.add_dir(root, u"old stuff", 6);
		const auto letter = fs.add_file(old, u"a letter.txt", text("dear wolf", 1000), { 100 }, true);
		fs.remove(letter);
		fs.remove(old);
		e.push_back({ "/old stuff", true, true, "intact", 0, {}, false });
		expect("/old stuff/a letter.txt", true, "intact", 1000, letter.content, true);

		// 12 KiB allocated, only the first 5000 bytes written: the rest reads as zeros, whatever the clusters hold
		const auto preallocated = fs.add_file(root, u"preallocated.dat", noise(17, 12288), { 110, 111, 112 }, true, 5000);
		std::vector<std::byte> zeroed = preallocated.content;
		std::fill(zeroed.begin() + 5000, zeroed.end(), std::byte{ 0 });
		expect("/preallocated.dat", false, "intact", 12288, zeroed, true);

		const auto rune = fs.add_file(root, u"Ein ziemlich langer Dateiname – ᚠ.txt", text("bound by gleipnir", 3000), { 120 }, true);
		fs.remove(rune);
		expect("/Ein ziemlich langer Dateiname \xE2\x80\x93 \xE1\x9A\xA0.txt", true, "intact", 3000, rune.content, true);

		const auto empty = fs.add_file(root, u"empty.txt", {}, {}, true);
		fs.remove(empty);
		expect("/empty.txt", true, "intact", 0, {}, true);

		// deleted, then another file written to the same clusters and deleted too: both claim them
		const auto first = fs.add_file(root, u"first.log", text("first", 6000), { 130, 131 }, true);
		fs.remove(first);
		const auto second = fs.add_file(root, u"second.log", text("second", 3000), { 130 }, true);
		fs.remove(second);
		expect("/first.log", true, "uncertain", 6000, first.content, false);
		expect("/second.log", true, "uncertain", 3000, second.content, true);

		// in two clusters with a fat chain, deleted by something that also cleared the chain: one piece is assumed
		const auto cleared = fs.add_file(root, u"cleared.bin", noise(18, 8192), { 140, 141 }, false);
		fs.remove(cleared, true);
		expect("/cleared.bin", true, "intact", 8192, cleared.content, true);

		return { fs.finish(partition_offset_sectors), std::move(e) };
	}

	// exfat_scenario() in partition 1 of a 10 MiB mbr disk, at sector 2048, type 0x07
	inline t_image exfat_disk() {
		t_image disk(10 * mib);
		const t_image volume = exfat_scenario(2048).volume;
		put_mbr(disk, 0, { t_mbr_entry{ 0x00, 0x07, 2048, static_cast<std::uint32_t>(volume.size() / 512) } }, 0x45584641);
		std::copy(volume.begin(), volume.end(), disk.begin() + 2048 * 512);
		return disk;
	}
}
