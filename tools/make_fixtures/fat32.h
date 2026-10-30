#pragma once

// a fat32 volume put together the way windows lays one out (microsoft's "FAT: General Overview of On-Disk Format"),
// then files written to it and deleted the way windows deletes them: the first byte of each of the file's directory
// entries becomes 0xE5 and its clusters are freed in the fat, nothing else changes. none of storage_core's code is used

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "fixtures.h"

namespace fixtures {
	class t_fat32 {
	public:
		static constexpr std::uint32_t bps = 512, reserved = 32, fats = 2, end_of_chain = 0x0FFFFFFF;

		struct t_dir {
			std::vector<std::uint32_t> clusters;
			std::size_t used = 0;               // entries written so far
			std::vector<std::size_t> entries;   // where its own entries in its parent are, for deleting it
		};
		struct t_file {
			std::vector<std::size_t> entries;   // byte offsets in the volume: long name entries, then the 8.3 one
			std::vector<std::uint32_t> clusters;
			std::vector<std::byte> content;
		};

		explicit t_fat32(std::size_t bytes, std::uint32_t sectors_per_cluster = 1) : spc_(sectors_per_cluster) {
			image_.assign(bytes, std::byte{ 0 });
			total_ = bytes / bps;
			// the smallest fat that has an entry for every cluster the rest of the volume holds
			for (fat_sectors_ = 1;; fat_sectors_++) {
				data_start_ = reserved + fats * fat_sectors_;
				clusters_ = static_cast<std::uint32_t>((total_ - data_start_) / spc_);
				if ((std::uint64_t(clusters_) + 2) * 4 <= std::uint64_t(fat_sectors_) * bps)
					break;
			}
			fat_.assign(clusters_ + 2, 0);
			fat_[0] = 0x0FFFFFF8;
			fat_[1] = end_of_chain;
			root_.clusters = { 2 };
			chain(root_.clusters);
		}

		t_dir& root() { return root_; }
		std::uint32_t cluster_size() const { return bps * spc_; }
		std::uint32_t cluster_count() const { return clusters_; }
		std::size_t cluster_offset(std::uint32_t c) const { return static_cast<std::size_t>((data_start_ + std::uint64_t(c - 2) * spc_) * bps); }

		// another cluster for a directory, linked onto its chain
		void grow(t_dir& dir, std::uint32_t cluster) {
			dir.clusters.push_back(cluster);
			chain(dir.clusters);
		}

		void put_label(std::string_view label11) {
			const std::size_t at = slot(root_);
			put_text(image_, at, label11);
			put8(image_, at + 11, 0x08);
		}

		t_file add_file(t_dir& dir, std::u16string_view long_name, std::string_view short11, std::uint8_t case_bits, std::vector<std::byte> content, std::vector<std::uint32_t> clusters) {
			if (clusters.size() * cluster_size() < content.size())
				throw std::logic_error("not enough clusters for the content");
			chain(clusters);
			for (std::size_t i = 0; i < content.size(); i++)
				image_.at(cluster_offset(clusters[i / cluster_size()]) + i % cluster_size()) = content[i];
			t_file file;
			file.entries = put_entries(dir, long_name, short11, 0x20, case_bits, clusters.empty() ? 0 : clusters[0], static_cast<std::uint32_t>(content.size()));
			file.clusters = std::move(clusters);
			file.content = std::move(content);
			return file;
		}

		t_dir add_dir(t_dir& parent, std::u16string_view long_name, std::string_view short11, std::uint8_t case_bits, std::uint32_t cluster) {
			t_dir dir;
			dir.clusters = { cluster };
			chain(dir.clusters);
			const std::uint32_t parent_cluster = &parent == &root_ ? 0 : parent.clusters[0]; // ".." of a root child is 0
			put_short_entry(slot(dir), ".          ", 0x10, 0, cluster, 0);
			put_short_entry(slot(dir), "..         ", 0x10, 0, parent_cluster, 0);
			dir.entries = put_entries(parent, long_name, short11, 0x10, case_bits, cluster, 0);
			return dir;
		}

		void remove(const std::vector<std::size_t>& entries, const std::vector<std::uint32_t>& clusters) {
			for (std::size_t at : entries)
				put8(image_, at, 0xE5);
			for (std::uint32_t c : clusters)
				fat_.at(c) = 0;
		}
		void remove(const t_file& file) { remove(file.entries, file.clusters); }
		void remove(const t_dir& dir) { remove(dir.entries, dir.clusters); }

		// the boot sector and its backup, fsinfo, and both fats
		t_image finish(std::uint32_t hidden_sectors) {
			for (std::size_t at : { std::size_t(0), std::size_t(6 * bps) }) {
				put8(image_, at, 0xEB); put8(image_, at + 1, 0x58); put8(image_, at + 2, 0x90);
				put_text(image_, at + 3, "MSWIN4.1");
				put16(image_, at + 11, bps);
				put8(image_, at + 13, static_cast<std::uint8_t>(spc_));
				put16(image_, at + 14, reserved);
				put8(image_, at + 16, fats);
				put8(image_, at + 21, 0xF8);
				put16(image_, at + 24, 63);
				put16(image_, at + 26, 255);
				put32(image_, at + 28, hidden_sectors);
				put32(image_, at + 32, static_cast<std::uint32_t>(total_));
				put32(image_, at + 36, fat_sectors_);
				put32(image_, at + 44, 2);
				put16(image_, at + 48, 1);
				put16(image_, at + 50, 6);
				put8(image_, at + 64, 0x80);
				put8(image_, at + 66, 0x29);
				put32(image_, at + 67, 0x2026F3E4);
				put_text(image_, at + 71, "FENRIR TEST");
				put_text(image_, at + 82, "FAT32   ");
				put8(image_, at + 510, 0x55); put8(image_, at + 511, 0xAA);
			}
			std::uint32_t free = 0;
			for (std::uint32_t c = 2; c < clusters_ + 2; c++)
				free += fat_[c] == 0;
			for (std::size_t at : { std::size_t(bps), std::size_t(7 * bps) }) {
				put32(image_, at, 0x41615252);
				put32(image_, at + 484, 0x61417272);
				put32(image_, at + 488, free);
				put32(image_, at + 492, 0xFFFFFFFF);
				put32(image_, at + 508, 0xAA550000);
			}
			for (std::uint32_t copy = 0; copy < fats; copy++)
				for (std::size_t c = 0; c < fat_.size(); c++)
					put32(image_, (reserved + copy * fat_sectors_) * bps + c * 4, fat_[c]);
			return image_;
		}

		static std::uint8_t checksum(std::string_view short11) {
			std::uint8_t sum = 0;
			for (char c : short11)
				sum = static_cast<std::uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + static_cast<std::uint8_t>(c));
			return sum;
		}

	private:
		void chain(const std::vector<std::uint32_t>& clusters) {
			for (std::size_t i = 0; i < clusters.size(); i++)
				fat_.at(clusters[i]) = i + 1 < clusters.size() ? clusters[i + 1] : end_of_chain;
		}

		std::size_t slot(t_dir& dir) {
			const std::size_t per_cluster = cluster_size() / 32, index = dir.used++;
			if (index / per_cluster >= dir.clusters.size())
				throw std::logic_error("the directory is full, grow() it");
			return cluster_offset(dir.clusters[index / per_cluster]) + index % per_cluster * 32;
		}

		// 2026-10-30 12:34:56 for everything
		void put_short_entry(std::size_t at, std::string_view short11, std::uint8_t attr, std::uint8_t case_bits, std::uint32_t first, std::uint32_t size) {
			const std::uint16_t date = (2026 - 1980) << 9 | 10 << 5 | 30, time = 12 << 11 | 34 << 5 | 56 / 2;
			put_text(image_, at, short11);
			put8(image_, at + 11, attr);
			put8(image_, at + 12, case_bits);
			put8(image_, at + 13, 0);
			put16(image_, at + 14, time);
			put16(image_, at + 16, date);
			put16(image_, at + 18, date);
			put16(image_, at + 20, static_cast<std::uint16_t>(first >> 16));
			put16(image_, at + 22, time);
			put16(image_, at + 24, date);
			put16(image_, at + 26, static_cast<std::uint16_t>(first & 0xFFFF));
			put32(image_, at + 28, size);
		}

		std::vector<std::size_t> put_entries(t_dir& dir, std::u16string_view long_name, std::string_view short11, std::uint8_t attr, std::uint8_t case_bits, std::uint32_t first, std::uint32_t size) {
			std::vector<std::size_t> written;
			const std::size_t parts = (long_name.size() + 12) / 13;
			const std::uint8_t sum = checksum(short11);
			for (std::size_t part = parts; part >= 1; part--) { // the last part first
				const std::size_t at = slot(dir);
				put8(image_, at, static_cast<std::uint8_t>(part | (part == parts ? 0x40 : 0)));
				put8(image_, at + 11, 0x0F);
				put8(image_, at + 13, sum);
				const std::size_t offsets[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
				for (std::size_t i = 0; i < 13; i++) {
					const std::size_t c = (part - 1) * 13 + i;
					const std::uint16_t unit = c < long_name.size() ? long_name[c] : c == long_name.size() ? 0x0000 : 0xFFFF;
					put16(image_, at + offsets[i], unit);
				}
				written.push_back(at);
			}
			const std::size_t at = slot(dir);
			put_short_entry(at, short11, attr, case_bits, first, size);
			written.push_back(at);
			return written;
		}

		t_image image_;
		std::uint32_t spc_;
		std::uint64_t total_ = 0, data_start_ = 0;
		std::uint32_t fat_sectors_ = 0, clusters_ = 0;
		std::vector<std::uint32_t> fat_;
		t_dir root_;
	};

	// deterministic content: a xorshift stream, or lines of text
	inline std::vector<std::byte> noise(std::uint64_t seed, std::size_t size) {
		std::vector<std::byte> bytes(size);
		std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
		for (std::byte& b : bytes) {
			x ^= x << 13;
			x ^= x >> 7;
			x ^= x << 17;
			b = static_cast<std::byte>(x >> 32);
		}
		return bytes;
	}
	inline std::vector<std::byte> text(std::string_view what, std::size_t size) {
		std::string s;
		for (int line = 1; s.size() < size; line++)
			s += std::string(what) + ", line " + std::to_string(line) + "\n";
		s.resize(size);
		std::vector<std::byte> bytes(size);
		for (std::size_t i = 0; i < size; i++)
			bytes[i] = static_cast<std::byte>(s[i]);
		return bytes;
	}

	// what a scan of fat32_volume() has to find. integrity as storage::to_string() spells it
	struct t_fat32_expected {
		std::string path;
		bool deleted = false, directory = false;
		std::string integrity;
		std::uint64_t recoverable = 0;
		std::vector<std::byte> original;   // what was written to it
		bool matches_original = false;     // an export of it must hash the same as the first `recoverable` bytes of original
	};

	struct t_fat32_scenario {
		t_image volume;
		std::vector<t_fat32_expected> expected; // in the order a scan lists them
	};

	// 8 MiB, 512 byte clusters. every case the fat32 scanner tells apart, in the root directory (2 clusters, 2 and
	// 3000, so it takes the fat to read all of it), a live subdirectory and a deleted one
	inline t_fat32_scenario fat32_scenario(std::uint32_t hidden_sectors = 0) {
		t_fat32 fs(8 * mib);
		t_fat32::t_dir& root = fs.root();
		fs.grow(root, 3000);
		fs.put_label("FENRIR TEST");
		std::vector<t_fat32_expected> e;
		auto expect = [&e](std::string path, bool deleted, std::string integrity, std::uint64_t recoverable, const std::vector<std::byte>& original, bool matches) {
			e.push_back({ std::move(path), deleted, false, std::move(integrity), recoverable, original, matches });
		};

		// a live file, in clusters a fragmented file left a gap for
		const auto keep = fs.add_file(root, u"", "KEEP    TXT", 0x18, text("keep", 1800), { 22, 23, 24, 25 });
		expect("/keep.txt", false, "intact", 1800, keep.content, true);

		// fragmented around keep.txt, then deleted: the clusters after its first two are keep's
		const auto fragmented = fs.add_file(root, u"fragmented.dat", "FRAGME~1DAT", 0, noise(1, 2500), { 20, 21, 30, 31, 32 });
		fs.remove(fragmented);
		expect("/fragmented.dat", true, "partial", 1024, fragmented.content, true);

		// deleted, no long name: the first letter of its 8.3 name is gone
		const auto notes = fs.add_file(root, u"", "NOTES   TXT", 0x18, text("the notes", 3000), { 40, 41, 42, 43, 44, 45 });
		fs.remove(notes);
		expect("/_otes.txt", true, "intact", 3000, notes.content, true);

		t_fat32::t_dir dcim = fs.add_dir(root, u"", "DCIM       ", 0, 3);
		e.push_back({ "/DCIM", false, true, "intact", 0, {}, false });
		std::vector<std::byte> jpeg = noise(2, 20000);
		const std::byte start_of_image[] = { std::byte{ 0xFF }, std::byte{ 0xD8 }, std::byte{ 0xFF }, std::byte{ 0xE0 } };
		std::copy(std::begin(start_of_image), std::end(start_of_image), jpeg.begin());
		jpeg[19998] = std::byte{ 0xFF };
		jpeg[19999] = std::byte{ 0xD9 };
		std::vector<std::uint32_t> photo_clusters;
		for (std::uint32_t c = 50; c < 90; c++)
			photo_clusters.push_back(c);
		const auto photo = fs.add_file(dcim, u"Holiday photo 2026.jpg", "HOLIDA~1JPG", 0, jpeg, photo_clusters);
		fs.remove(photo);
		expect("/DCIM/Holiday photo 2026.jpg", true, "intact", 20000, photo.content, true);

		// deleted, then a new file written over its first clusters
		const auto overwritten = fs.add_file(root, u"overwritten.bin", "OVERWR~1BIN", 0, noise(3, 4000), { 100, 101, 102, 103, 104, 105, 106, 107 });
		fs.remove(overwritten);
		expect("/overwritten.bin", true, "unrecoverable", 0, overwritten.content, false);
		const auto fresh = fs.add_file(root, u"", "NEW     BIN", 0x18, noise(4, 2048), { 100, 101, 102, 103 });
		expect("/new.bin", false, "intact", 2048, fresh.content, true);

		// a directory and the file in it, deleted the way windows does it: the file first
		t_fat32::t_dir old = fs.add_dir(root, u"old stuff", "OLDSTU~1   ", 0, 4);
		const auto letter = fs.add_file(old, u"a letter.txt", "ALETTE~1TXT", 0, text("dear wolf", 1000), { 120, 121 });
		fs.remove(letter);
		fs.remove(old);
		e.push_back({ "/old stuff", true, true, "intact", 0, {}, false });
		expect("/old stuff/a letter.txt", true, "intact", 1000, letter.content, true);

		const auto empty = fs.add_file(root, u"", "EMPTY   TXT", 0x18, {}, {});
		fs.remove(empty);
		expect("/_mpty.txt", true, "intact", 0, {}, true);

		const auto rune = fs.add_file(root, u"Fenrir – ᚠ.txt", "FENRIR~1TXT", 0, text("bound by gleipnir", 700), { 130, 131 });
		fs.remove(rune);
		expect("/Fenrir \xE2\x80\x93 \xE1\x9A\xA0.txt", true, "intact", 700, rune.content, true);

		// deleted, then another file written to the same clusters and deleted too: both claim them
		const auto first = fs.add_file(root, u"", "FIRST   LOG", 0x18, text("first", 1500), { 140, 141, 142 });
		fs.remove(first);
		const auto second = fs.add_file(root, u"", "SECOND  LOG", 0x18, text("second", 1000), { 140, 141 });
		fs.remove(second);
		expect("/_irst.log", true, "uncertain", 1500, first.content, false);
		expect("/_econd.log", true, "uncertain", 1000, second.content, true);

		return { fs.finish(hidden_sectors), std::move(e) };
	}

	// fat32_scenario() in partition 1 of a 10 MiB mbr disk, at sector 2048
	inline t_image fat32_disk() {
		t_image disk(10 * mib);
		const t_image volume = fat32_scenario(2048).volume;
		put_mbr(disk, 0, { t_mbr_entry{ 0x80, 0x0C, 2048, static_cast<std::uint32_t>(volume.size() / 512) } }, 0x46415432);
		std::copy(volume.begin(), volume.end(), disk.begin() + 2048 * 512);
		return disk;
	}
}
