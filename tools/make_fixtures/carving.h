#pragma once

// files for the carver to find, written from their specs without any of storage_core's code: a baseline jpeg encoder
// (itu t.81, with annex k's tables), png with zlib's stored blocks (rfc 1950 and 1951), zip with stored and deflated
// entries (pkware's appnote) and pdf (iso 32000) laid out object by object. the image data is random, but every file is
// valid: windows' own decoders open the jpegs, pngs and zips. then a disk with them on it, whole, fragmented, cut short
// and faked, at the offsets carve_disk() lists

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "fat32.h"
#include "fixtures.h"

namespace fixtures {
	inline void append(t_image& out, std::string_view text) {
		for (char c : text)
			out.push_back(static_cast<std::byte>(c));
	}
	inline void append(t_image& out, const std::vector<std::byte>& bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }
	inline void append_be(t_image& out, std::uint64_t value, int width) {
		for (int i = width - 1; i >= 0; i--)
			out.push_back(static_cast<std::byte>(value >> (8 * i)));
	}
	inline void append_le(t_image& out, std::uint64_t value, int width) {
		for (int i = 0; i < width; i++)
			out.push_back(static_cast<std::byte>(value >> (8 * i)));
	}

	// crc-32 of a whole buffer, the slow way like fixtures.h's
	inline std::uint32_t crc32_of(const t_image& bytes) { return bytes.empty() ? 0 : crc32(bytes, 0, bytes.size()); }

	// a deterministic stream of numbers for coefficients and pixels
	class t_random {
	public:
		explicit t_random(std::uint64_t seed) : x_(seed * 0x9E3779B97F4A7C15ull + 1) {}
		std::uint32_t next() {
			x_ ^= x_ << 13;
			x_ ^= x_ >> 7;
			x_ ^= x_ << 17;
			return static_cast<std::uint32_t>(x_ >> 32);
		}
		int range(int from, int to) { return from + static_cast<int>(next() % static_cast<std::uint32_t>(to - from + 1)); }

	private:
		std::uint64_t x_;
	};

	// itu t.81 annex k.3: the example huffman tables nearly every encoder uses unchanged
	namespace annex_k {
		inline constexpr std::uint8_t dc_luma_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
		inline constexpr std::uint8_t dc_chroma_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
		inline constexpr std::uint8_t dc_values[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
		inline constexpr std::uint8_t ac_luma_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D };
		inline constexpr std::uint8_t ac_luma_values[162] = {
			0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
			0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
			0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
			0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
			0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
			0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
			0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
			0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
			0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
			0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
			0xF9, 0xFA,
		};
		inline constexpr std::uint8_t ac_chroma_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
		inline constexpr std::uint8_t ac_chroma_values[162] = {
			0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
			0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
			0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
			0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
			0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
			0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
			0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
			0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
			0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
			0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
			0xF9, 0xFA,
		};
	}

	// what a baseline jpeg encoder writes, from coefficients instead of pixels: the dct and quantisation would only
	// change which numbers get coded, not how
	class t_jpeg_encoder {
	public:
		struct t_code {
			std::uint16_t bits = 0;
			int length = 0;
		};
		using t_codes = std::array<t_code, 256>;

		// annex c.2: the codes of each length in turn, counting up, one bit longer each length
		static t_codes codes(const std::uint8_t* counts, const std::uint8_t* values) {
			t_codes out{};
			std::uint16_t code = 0;
			int k = 0;
			for (int length = 1; length <= 16; length++) {
				for (int i = 0; i < counts[length - 1]; i++)
					out[values[k++]] = { code++, length };
				code = static_cast<std::uint16_t>(code << 1);
			}
			return out;
		}

		// gray: one component; else y at 2x2 with cb and cr at 1x1 (4:2:0). restart: a RSTn every that many units
		static t_image encode(int width, int height, std::uint64_t seed, bool gray = false, int restart = 0, const t_image& app1 = {}) {
			t_image out;
			append_be(out, 0xFFD8, 2);
			append_be(out, 0xFFE0, 2); // jfif 1.01, no thumbnail
			append_be(out, 16, 2);
			append(out, std::string_view("JFIF\0\x01\x01\0\0\x01\0\x01\0\0", 14));
			if (!app1.empty()) {
				append_be(out, 0xFFE1, 2);
				append_be(out, app1.size() + 2, 2);
				append(out, app1);
			}
			const int tables = gray ? 1 : 2;
			for (int t = 0; t < tables; t++) {
				append_be(out, 0xFFDB, 2);
				append_be(out, 67, 2);
				out.push_back(static_cast<std::byte>(t));
				for (int i = 0; i < 64; i++)
					out.push_back(static_cast<std::byte>(2 + i / 2 + t * 3));
			}
			append_be(out, 0xFFC0, 2);
			const int components = gray ? 1 : 3;
			append_be(out, 8 + 3 * components, 2);
			out.push_back(std::byte{ 8 });
			append_be(out, static_cast<std::uint64_t>(height), 2);
			append_be(out, static_cast<std::uint64_t>(width), 2);
			out.push_back(static_cast<std::byte>(components));
			for (int c = 0; c < components; c++) {
				out.push_back(static_cast<std::byte>(c + 1));
				out.push_back(static_cast<std::byte>(gray ? 0x11 : (c == 0 ? 0x22 : 0x11)));
				out.push_back(static_cast<std::byte>(c == 0 ? 0 : 1));
			}
			table(out, 0x00, annex_k::dc_luma_bits, annex_k::dc_values);
			table(out, 0x10, annex_k::ac_luma_bits, annex_k::ac_luma_values);
			if (!gray) {
				table(out, 0x01, annex_k::dc_chroma_bits, annex_k::dc_values);
				table(out, 0x11, annex_k::ac_chroma_bits, annex_k::ac_chroma_values);
			}
			if (restart != 0) {
				append_be(out, 0xFFDD, 2);
				append_be(out, 4, 2);
				append_be(out, static_cast<std::uint64_t>(restart), 2);
			}
			append_be(out, 0xFFDA, 2);
			append_be(out, 6 + 2 * components, 2);
			out.push_back(static_cast<std::byte>(components));
			for (int c = 0; c < components; c++) {
				out.push_back(static_cast<std::byte>(c + 1));
				out.push_back(static_cast<std::byte>(c == 0 ? 0x00 : 0x11));
			}
			out.push_back(std::byte{ 0 });
			out.push_back(std::byte{ 63 });
			out.push_back(std::byte{ 0 });

			t_jpeg_encoder e(out, seed);
			const int unit = gray ? 8 : 16;
			const int units = ((width + unit - 1) / unit) * ((height + unit - 1) / unit);
			for (int u = 0; u < units; u++) {
				if (restart != 0 && u != 0 && u % restart == 0) {
					e.pad();
					append_be(out, 0xFFD0 + static_cast<std::uint64_t>((u / restart - 1) % 8), 2);
					e.predictors_ = {};
				}
				for (int b = 0; b < (gray ? 1 : 4); b++)
					e.block(0);
				if (!gray) {
					e.block(1);
					e.block(2);
				}
			}
			e.pad();
			append_be(out, 0xFFD9, 2);
			return out;
		}

	private:
		t_jpeg_encoder(t_image& out, std::uint64_t seed) : out_(out), random_(seed) {
			dc_[0] = codes(annex_k::dc_luma_bits, annex_k::dc_values);
			dc_[1] = codes(annex_k::dc_chroma_bits, annex_k::dc_values);
			ac_[0] = codes(annex_k::ac_luma_bits, annex_k::ac_luma_values);
			ac_[1] = codes(annex_k::ac_chroma_bits, annex_k::ac_chroma_values);
		}

		static void table(t_image& out, int slot, const std::uint8_t* counts, const std::uint8_t* values) {
			int total = 0;
			for (int i = 0; i < 16; i++)
				total += counts[i];
			append_be(out, 0xFFC4, 2);
			append_be(out, static_cast<std::uint64_t>(3 + 16 + total), 2);
			out.push_back(static_cast<std::byte>(slot));
			for (int i = 0; i < 16; i++)
				out.push_back(static_cast<std::byte>(counts[i]));
			for (int i = 0; i < total; i++)
				out.push_back(static_cast<std::byte>(values[i]));
		}

		void put(std::uint32_t bits, int count) {
			for (int i = count - 1; i >= 0; i--) {
				acc_ = (acc_ << 1) | ((bits >> i) & 1);
				if (++n_ == 8) {
					out_.push_back(static_cast<std::byte>(acc_));
					if ((acc_ & 0xFF) == 0xFF)
						out_.push_back(std::byte{ 0 }); // a stuffed zero, so the data never looks like a marker
					acc_ = 0;
					n_ = 0;
				}
			}
		}
		void pad() {
			while (n_ != 0)
				put(1, 1);
		}

		// annex f.1.2.1: a value as its category (how many bits) and those bits, negatives as the complement
		static int category(int v) {
			int a = v < 0 ? -v : v, c = 0;
			while (a != 0) {
				a >>= 1;
				c++;
			}
			return c;
		}
		void value(int v, int c) {
			if (c != 0)
				put(static_cast<std::uint32_t>(v > 0 ? v : v + (1 << c) - 1), c);
		}
		void symbol(const t_codes& codes, int s) { put(codes[static_cast<std::size_t>(s)].bits, codes[static_cast<std::size_t>(s)].length); }

		// a block of made up coefficients: a dc that wanders, a few ac values, now and then one far out (ZRL) or
		// at the very end (no EOB)
		void block(int component) {
			const std::size_t table = component == 0 ? 0 : 1;
			const int dc = random_.range(-300, 300), diff = dc - predictors_[static_cast<std::size_t>(component)];
			predictors_[static_cast<std::size_t>(component)] = dc;
			symbol(dc_[table], category(diff));
			value(diff, category(diff));

			std::array<int, 64> ac{};
			const int kind = random_.range(0, 9);
			for (int k = 1; k < 64; k++)
				if (random_.range(0, k < 10 ? 2 : 12) == 0)
					ac[static_cast<std::size_t>(k)] = random_.range(1, 60) * (random_.range(0, 1) ? 1 : -1);
			if (kind == 0) { // only far out: ZRLs
				ac = {};
				ac[40] = 3;
			} else if (kind == 1) {
				ac[63] = -1;
			} else if (kind == 2) {
				ac[5] = 700; // a big one, category 10
			}
			int run = 0;
			for (int k = 1; k < 64; k++) {
				const int v = ac[static_cast<std::size_t>(k)];
				if (v == 0) {
					run++;
					continue;
				}
				while (run > 15) {
					symbol(ac_[table], 0xF0);
					run -= 16;
				}
				const int c = category(v);
				symbol(ac_[table], run << 4 | c);
				value(v, c);
				run = 0;
			}
			if (run != 0)
				symbol(ac_[table], 0x00);
		}

		t_image& out_;
		t_random random_;
		std::array<t_codes, 2> dc_{}, ac_{};
		std::array<int, 3> predictors_{};
		std::uint32_t acc_ = 0;
		int n_ = 0;
	};

	// a progressive jpeg's markers around image data that isn't real: the carver only follows a progressive one's
	// markers, so this is all it can tell apart. no decoder would open it
	inline t_image progressive_jpeg(std::uint64_t seed, std::size_t data) {
		t_image out = t_jpeg_encoder::encode(16, 16, seed, true);
		t_image head(out.begin(), out.begin() + 2);
		std::size_t i = 2;
		for (;;) { // copy segments up to the scan, the frame marker turned into SOF2
			const int marker = std::to_integer<int>(out[i + 1]);
			const std::size_t length = std::to_integer<std::size_t>(out[i + 2]) << 8 | std::to_integer<std::size_t>(out[i + 3]);
			if (marker == 0xDA)
				break;
			head.insert(head.end(), out.begin() + static_cast<std::ptrdiff_t>(i), out.begin() + static_cast<std::ptrdiff_t>(i + 2 + length));
			if (marker == 0xC0)
				head[head.size() - 2 - length + 1] = std::byte{ 0xC2 };
			i += 2 + length;
		}
		append_be(head, 0xFFDA, 2); // the dc scan: Ss 0, Se 0
		append_be(head, 8, 2);
		append(head, std::string_view("\x01\x01\x00\x00\x00\x00", 6));
		t_random random(seed);
		for (std::size_t n = 0; n < data; n++) {
			const auto b = static_cast<std::byte>(random.next());
			head.push_back(b);
			if (b == std::byte{ 0xFF })
				head.push_back(std::byte{ 0 });
		}
		append_be(head, 0xFFD9, 2);
		return head;
	}

	// rfc 1951 stored blocks: deflate without compressing
	inline t_image deflate_stored(const t_image& data) {
		t_image out;
		std::size_t at = 0;
		do {
			const std::size_t n = std::min<std::size_t>(65535, data.size() - at);
			out.push_back(std::byte{ at + n == data.size() ? std::uint8_t(1) : std::uint8_t(0) });
			append_le(out, n, 2);
			append_le(out, ~n & 0xFFFF, 2);
			out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + n));
			at += n;
		} while (at < data.size());
		return out;
	}

	// rfc 1950 zlib around them
	inline t_image zlib_stored(const t_image& data) {
		t_image out;
		out.push_back(std::byte{ 0x78 });
		out.push_back(std::byte{ 0x01 });
		append(out, deflate_stored(data));
		std::uint32_t a = 1, b = 0;
		for (std::byte x : data) {
			a = (a + std::to_integer<std::uint32_t>(x)) % 65521;
			b = (b + a) % 65521;
		}
		append_be(out, b << 16 | a, 4);
		return out;
	}

	// an rgb png of random pixels, its image data split into idat chunks of idat bytes. chunk_ends: where each chunk
	// ends, for working out what a fragment of it keeps
	inline t_image png(int width, int height, std::uint64_t seed, std::size_t idat, std::vector<std::size_t>* chunk_ends = nullptr) {
		t_image out;
		append(out, std::string_view("\x89PNG\r\n\x1A\n", 8));
		auto chunk = [&](std::string_view type, const t_image& data) {
			append_be(out, data.size(), 4);
			t_image crced;
			append(crced, type);
			append(crced, data);
			append(out, crced);
			append_be(out, crc32_of(crced), 4);
			if (chunk_ends)
				chunk_ends->push_back(out.size());
		};
		t_image header;
		append_be(header, static_cast<std::uint64_t>(width), 4);
		append_be(header, static_cast<std::uint64_t>(height), 4);
		append(header, std::string_view("\x08\x02\x00\x00\x00", 5));
		chunk("IHDR", header);
		t_image rows;
		t_random random(seed);
		for (int y = 0; y < height; y++) {
			rows.push_back(std::byte{ 0 }); // filter: none
			for (int x = 0; x < width * 3; x++)
				rows.push_back(static_cast<std::byte>(random.next()));
		}
		const t_image z = zlib_stored(rows);
		for (std::size_t at = 0; at < z.size(); at += idat)
			chunk("IDAT", t_image(z.begin() + static_cast<std::ptrdiff_t>(at), z.begin() + static_cast<std::ptrdiff_t>(std::min(z.size(), at + idat))));
		chunk("IEND", {});
		return out;
	}

	struct t_zip_entry {
		std::string name;
		t_image data;
		bool deflate = false;    // method 8, as stored deflate blocks
		bool descriptor = false; // sizes and crc after the data, in a signed data descriptor
	};

	// local entries, the central directory and its end record. entry_ends: where each local entry ends
	inline t_image zip(const std::vector<t_zip_entry>& entries, std::vector<std::size_t>* entry_ends = nullptr) {
		constexpr std::uint16_t time = (12 << 11) | (34 << 5) | (56 / 2), date = ((2026 - 1980) << 9) | (10 << 5) | 30;
		t_image out, central;
		for (const t_zip_entry& e : entries) {
			const t_image body = e.deflate ? deflate_stored(e.data) : e.data;
			const std::uint32_t crc = crc32_of(e.data);
			const std::size_t at = out.size();
			auto header = [&](t_image& to, bool local) {
				append_le(to, local ? 0x04034B50 : 0x02014B50, 4);
				if (!local)
					append_le(to, 20, 2); // made by
				append_le(to, 20, 2);
				append_le(to, e.descriptor ? 8 : 0, 2);
				append_le(to, e.deflate ? 8 : 0, 2);
				append_le(to, time, 2);
				append_le(to, date, 2);
				const bool blank = local && e.descriptor;
				append_le(to, blank ? 0 : crc, 4);
				append_le(to, blank ? 0 : body.size(), 4);
				append_le(to, blank ? 0 : e.data.size(), 4);
				append_le(to, e.name.size(), 2);
				append_le(to, 0, 2);
				if (!local) {
					append_le(to, 0, 2);  // comment
					append_le(to, 0, 2);  // disk
					append_le(to, 0, 2);  // internal attributes
					append_le(to, 0, 4);  // external
					append_le(to, at, 4); // where its local header is
				}
				append(to, e.name);
			};
			header(out, true);
			append(out, body);
			if (e.descriptor) {
				append_le(out, 0x08074B50, 4);
				append_le(out, crc, 4);
				append_le(out, body.size(), 4);
				append_le(out, e.data.size(), 4);
			}
			if (entry_ends)
				entry_ends->push_back(out.size());
			header(central, false);
		}
		const std::size_t central_at = out.size();
		append(out, central);
		append_le(out, 0x06054B50, 4);
		append_le(out, 0, 4);
		append_le(out, entries.size(), 2);
		append_le(out, entries.size(), 2);
		append_le(out, central.size(), 4);
		append_le(out, central_at, 4);
		append_le(out, 0, 2);
		return out;
	}

	// a one page pdf with objects 1 to 7 (7 is the length of 6's stream, given after it), an xref table and a trailer.
	// update: then an incremental update replacing the page's content. object_ends: where each "endobj" ends
	inline t_image pdf(std::string_view text, bool update, std::vector<std::size_t>* object_ends = nullptr) {
		std::string s = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
		std::vector<std::size_t> offsets;
		auto object = [&](const std::string& body) {
			offsets.push_back(s.size());
			s += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj";
			if (object_ends)
				object_ends->push_back(s.size());
			s += "\n";
		};
		auto stream = [](const std::string& data, const std::string& length) { return "<< /Length " + length + " >>\nstream\n" + data + "\nendstream"; };
		const std::string content = "BT /F1 24 Tf 72 700 Td (" + std::string(text) + ") Tj ET";
		std::string binary;
		t_random random(text.size());
		for (int i = 0; i < 300; i++)
			binary += static_cast<char>(random.next());
		object("<< /Type /Catalog /Pages 2 0 R >>");
		object("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
		object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> /Thumb 6 0 R >>");
		object(stream(content, std::to_string(content.size())));
		object("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Note (a string with \\) and (nested) brackets) >>");
		object(stream(binary, "7 0 R"));
		object(std::to_string(binary.size()));
		auto entry = [](std::size_t offset, const char* type) {
			char line[21];
			std::snprintf(line, sizeof line, "%010zu %05d %s\r\n", offset, type[0] == 'f' ? 65535 : 0, type);
			return std::string(line, 20);
		};
		std::size_t xref = s.size();
		s += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n" + entry(0, "f");
		for (std::size_t o : offsets)
			s += entry(o, "n");
		s += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
		if (update) {
			const std::size_t at = s.size(), previous = xref;
			const std::string changed = "BT /F1 24 Tf 72 650 Td (" + std::string(text) + ", changed) Tj ET";
			s += "4 0 obj\n" + stream(changed, std::to_string(changed.size())) + "\nendobj";
			if (object_ends)
				object_ends->push_back(s.size());
			s += "\n";
			xref = s.size();
			s += "xref\n0 1\n" + entry(0, "f") + "4 1\n" + entry(at, "n");
			s += "trailer\n<< /Size 8 /Root 1 0 R /Prev " + std::to_string(previous) + " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
		}
		t_image out;
		append(out, s);
		return out;
	}

	// what carving carve_disk() has to find. integrity as storage::to_string() spells it
	struct t_carve_expected {
		std::uint64_t offset = 0;
		std::string format, extension, integrity;
		std::uint64_t recoverable = 0; // 0: not known in advance (a fragmented jpeg is cut where decoding fails)
		t_image original;              // the whole file as it was written
		bool in_fat32 = false;         // a file the fat32 scan of partition 1 finds too
		std::uint64_t fragment = 0;    // a fragmented file: how long its first part is
		bool matches_original() const { return recoverable != 0 && integrity != "uncertain"; }
		std::string name() const {
			char text[32];
			std::snprintf(text, sizeof text, "%010llx.", static_cast<unsigned long long>(offset));
			return text + extension;
		}
	};

	struct t_carve_disk {
		t_image image;
		std::vector<t_carve_expected> expected; // in the order they're on the disk
		std::uint64_t partition_offset = 0;
	};

	// 12 MiB. partition 1 (1 MiB to 3 MiB) is fat32 with a live jpeg and a deleted png in it. past it, space no
	// partition covers, with files whose metadata is gone: whole ones, one across the 4 MiB chunk boundary,
	// fragmented ones, cut short ones and signatures with nothing behind them
	inline t_carve_disk carve_disk() {
		constexpr std::size_t mb = mib, sector = 512;
		t_carve_disk d;
		d.image.assign(12 * mb, std::byte{ 0 });
		d.partition_offset = 1 * mb;
		std::vector<t_carve_expected>& e = d.expected;
		auto place = [&](std::size_t at, const t_image& file) { std::copy(file.begin(), file.end(), d.image.begin() + static_cast<std::ptrdiff_t>(at)); };
		auto whole = [&](std::size_t at, const t_image& file, std::string format, std::string extension, std::string integrity = "intact") {
			place(at, file);
			e.push_back({ at, std::move(format), std::move(extension), std::move(integrity), file.size(), file });
		};

		// partition 1: a fat32 volume of 512 byte clusters, so its files are at sector boundaries
		{
			t_fat32 fs(2 * mb);
			const t_image photo = t_jpeg_encoder::encode(48, 32, 11);
			const t_image chart = png(20, 12, 12, 400);
			std::vector<std::uint32_t> photo_clusters, chart_clusters;
			for (std::uint32_t c = 0; c < (photo.size() + 511) / 512; c++)
				photo_clusters.push_back(10 + c);
			for (std::uint32_t c = 0; c < (chart.size() + 511) / 512; c++)
				chart_clusters.push_back(200 + c);
			fs.add_file(fs.root(), u"photo.jpg", "PHOTO   JPG", 0, photo, photo_clusters);
			const auto deleted = fs.add_file(fs.root(), u"chart.png", "CHART   PNG", 0, chart, chart_clusters);
			fs.remove(deleted);
			const t_image volume = fs.finish(2048);
			put_mbr(d.image, 0, { t_mbr_entry{ 0x00, 0x0C, 2048, static_cast<std::uint32_t>(volume.size() / sector) } }, 0x43415256);
			place(d.partition_offset, volume);
			e.push_back({ d.partition_offset + fs.cluster_offset(10), "JPEG", "jpg", "intact", photo.size(), photo, true });
			e.push_back({ d.partition_offset + fs.cluster_offset(200), "PNG", "png", "intact", chart.size(), chart, true });
		}

		// signatures with nothing after them, in noise: none of them is a file
		std::size_t at = 3 * mb;
		place(at, noise(20, 8192));
		for (std::size_t i = 0; i < 16; i++)
			d.image[at + i * sector] = std::byte{ 0 }; // so the noise itself can't start one by chance
		place(at + 1 * sector, t_image{ std::byte{ 0xFF }, std::byte{ 0xD8 }, std::byte{ 0xFF }, std::byte{ 0xE0 } });
		place(at + 2 * sector, t_image{ std::byte{ 0x89 }, std::byte{ 'P' }, std::byte{ 'N' }, std::byte{ 'G' }, std::byte{ '\r' }, std::byte{ '\n' }, std::byte{ 0x1A }, std::byte{ '\n' } });
		place(at + 3 * sector, t_image{ std::byte{ '%' }, std::byte{ 'P' }, std::byte{ 'D' }, std::byte{ 'F' }, std::byte{ '-' }, std::byte{ '1' }, std::byte{ '.' }, std::byte{ '4' } });
		place(at + 4 * sector, t_image{ std::byte{ 'P' }, std::byte{ 'K' }, std::byte{ 3 }, std::byte{ 4 } });
		// a jpeg's headers whose image data was overwritten right after them
		{
			t_image headers = t_jpeg_encoder::encode(64, 64, 21);
			headers.resize(700);
			place(at + 5 * sector, headers);
			place(at + 5 * sector + 700, noise(22, 2000));
		}

		// whole files. the camera jpeg has a thumbnail in its exif data, which mustn't be found on its own
		t_image exif;
		append(exif, std::string_view("Exif\0\0", 6));
		append(exif, noise(23, 40));
		append(exif, t_jpeg_encoder::encode(16, 16, 24, true));
		whole(4 * mb - 6 * sector, t_jpeg_encoder::encode(160, 120, 25, false, 0, exif), "JPEG", "jpg"); // across the 4 MiB chunk boundary
		at = 4 * mb + 64 * 1024;
		auto next = [&](std::size_t size) {
			const std::size_t here = at;
			at += (size + 3 * sector + sector - 1) / sector * sector; // a gap of zeros between them
			return here;
		};
		{
			const t_image f = t_jpeg_encoder::encode(64, 40, 26, false, 3);
			whole(next(f.size()), f, "JPEG", "jpg");
		}
		{
			const t_image f = t_jpeg_encoder::encode(40, 24, 27, true);
			whole(next(f.size()), f, "JPEG", "jpg");
		}
		{
			const t_image f = progressive_jpeg(28, 3000);
			whole(next(f.size()), f, "JPEG", "jpg", "uncertain");
		}
		{
			const t_image f = png(64, 48, 29, 2000);
			whole(next(f.size()), f, "PNG", "png");
		}
		{
			const t_image f = pdf("Fenrir broke Gleipnir", false);
			whole(next(f.size()), f, "PDF", "pdf");
		}
		{
			const t_image f = pdf("and then he ate the sun", true);
			whole(next(f.size()), f, "PDF", "pdf");
		}
		{
			const t_image f = zip({ { "readme.txt", text("a stored entry", 3000) }, { "data/noise.bin", noise(30, 5000), true }, { "empty", {}, true } });
			whole(next(f.size()), f, "ZIP", "zip");
		}
		{
			const t_image f = zip({ { "streamed.txt", text("sizes come after", 2000), false, true }, { "streamed.bin", noise(31, 3000), true, true } });
			whole(next(f.size()), f, "ZIP", "zip");
		}
		{
			const t_image f = zip({ { "[Content_Types].xml", text("<Types/>", 300) }, { "_rels/.rels", text("<Relationships/>", 200) },
				{ "word/document.xml", text("<w:document/>", 4000), true } });
			whole(next(f.size()), f, "DOCX", "docx");
		}

		// fragmented: the first part, other data, then the rest
		{
			const t_image f = t_jpeg_encoder::encode(320, 240, 32);
			const std::size_t first = 16 * sector;
			const std::size_t here = next(f.size() + 8192);
			place(here, t_image(f.begin(), f.begin() + first));
			place(here + first, noise(33, 8192));
			place(here + first + 8192, t_image(f.begin() + first, f.end()));
			e.push_back({ here, "JPEG", "jpg", "partial", 0, f, false, first });
		}
		{
			std::vector<std::size_t> ends;
			const t_image f = png(80, 60, 34, 3000, &ends);
			const std::size_t first = 12 * sector; // inside the 4th or so idat chunk
			const std::size_t here = next(f.size() + 4096);
			place(here, t_image(f.begin(), f.begin() + first));
			place(here + first, noise(35, 4096));
			place(here + first + 4096, t_image(f.begin() + first, f.end()));
			std::size_t keep = 0;
			for (std::size_t end : ends)
				if (end <= first)
					keep = end;
			e.push_back({ here, "PNG", "png", "partial", keep, f, false, first });
		}
		// cut short: a zip whose central directory was overwritten, a pdf that stops after an object
		{
			std::vector<std::size_t> ends;
			const t_image f = zip({ { "one.txt", text("one", 1500) }, { "two.txt", text("two", 1500) }, { "three.txt", text("three", 1500) } }, &ends);
			const std::size_t here = next(f.size());
			place(here, t_image(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(ends.back())));
			e.push_back({ here, "ZIP", "zip", "partial", ends.back(), f });
		}
		{
			std::vector<std::size_t> ends;
			const t_image f = pdf("cut short", false, &ends);
			const std::size_t here = next(f.size());
			place(here, t_image(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(ends[4])));
			e.push_back({ here, "PDF", "pdf", "partial", ends[4], f });
		}
		return d;
	}
}
