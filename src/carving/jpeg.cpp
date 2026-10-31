// jpeg (itu t.81): the markers from the start of the image to its end, and for sequential huffman jpegs (baseline and
// extended, the kind cameras write) the image data too, every block's huffman codes decoded without the dct. bytes that
// aren't this image's stop decoding within a few blocks, which is how a fragment's end is found. progressive, lossless
// and arithmetic coded ones are only followed by their markers

#include <array>
#include <format>
#include <optional>

#include "formats.h"

namespace {
	using namespace storage;
	using namespace storage::carving;

	constexpr int max_segments = 4096, max_scans = 256, max_fill = 1024;
	constexpr std::uint64_t sector = 512;

	// annex c and f.2.2.3: for each code length, the largest code of that length (-1 for none), and where its values are
	struct t_table {
		std::array<std::int32_t, 17> maxcode{}, valptr{}, mincode{};
		std::array<std::uint8_t, 256> values{};
		bool defined = false;
	};

	bool build(t_table& table, std::span<const std::uint8_t> counts, std::span<const std::uint8_t> values) {
		std::int32_t code = 0, k = 0;
		for (int length = 1; length <= 16; length++) {
			const int n = counts[static_cast<std::size_t>(length - 1)];
			table.maxcode[static_cast<std::size_t>(length)] = -1;
			if (n != 0) {
				table.valptr[static_cast<std::size_t>(length)] = k;
				table.mincode[static_cast<std::size_t>(length)] = code;
				code += n;
				k += n;
				table.maxcode[static_cast<std::size_t>(length)] = code - 1;
			}
			if (code >= (std::int32_t(1) << length)) // more codes than fit in this many bits (or the all ones one, which isn't used)
				return false;
			code <<= 1;
		}
		std::ranges::copy(values, table.values.begin());
		table.defined = true;
		return true;
	}

	// the image data's bits. a 0xFF in it is followed by 0x00, anything else after one is a marker, and the data ends there
	class t_bits {
	public:
		t_bits(t_reader& reader, std::uint64_t pos) : reader_(reader), pos_(pos) {}

		int bit() {
			if (count_ == 0 && !fill())
				return -1;
			return (byte_ >> --count_) & 1;
		}

		bool skip(int n) {
			for (int i = 0; i < n; i++)
				if (bit() < 0)
					return false;
			return true;
		}

		int decode(const t_table& table) {
			std::int32_t code = 0;
			for (std::size_t length = 1; length <= 16; length++) {
				const int b = bit();
				if (b < 0)
					return -1;
				code = (code << 1) | b;
				if (code <= table.maxcode[length])
					return table.values[static_cast<std::size_t>(table.valptr[length] + code - table.mincode[length]) & 0xFF];
			}
			return -1; // no code is 16 ones
		}

		// what's left of the byte is padding, and RSTn has to come next
		bool restart(int n) {
			count_ = 0;
			if (reader_.at(pos_) != 0xFF)
				return false;
			for (int i = 0; reader_.at(pos_ + 1) == 0xFF && i < max_fill; i++)
				pos_++;
			if (reader_.at(pos_ + 1) != 0xD0 + n)
				return false;
			pos_ += 2;
			return true;
		}

		std::uint64_t end() {
			count_ = 0;
			return pos_;
		}
		std::uint64_t pos() const { return pos_; }

	private:
		bool fill() {
			const int b = reader_.at(pos_);
			if (b < 0)
				return false;
			if (b == 0xFF) {
				if (reader_.at(pos_ + 1) != 0x00)
					return false;
				pos_ += 2;
			} else {
				pos_++;
			}
			byte_ = b;
			count_ = 8;
			return true;
		}

		t_reader& reader_;
		std::uint64_t pos_;
		int byte_ = 0, count_ = 0;
	};

	struct t_component {
		int id = 0, h = 1, v = 1;
	};

	class t_jpeg {
	public:
		explicit t_jpeg(t_reader& reader) : r_(reader) {}

		t_verdict run() {
			std::uint64_t pos = 2;
			bool first = true;
			for (int segments = 0; segments < max_segments; segments++) {
				if (r_.at(pos) != 0xFF)
					return broken(pos, "a marker should be there and isn't");
				for (int i = 0; r_.at(pos + 1) == 0xFF && i < max_fill; i++)
					pos++;
				const int marker = r_.at(pos + 1);
				if (marker < 0)
					return broken(pos, "");
				const std::uint64_t marker_at = pos;
				pos += 2;

				const bool segment = (marker >= 0xC0 && marker <= 0xCF && marker != 0xC8) || (marker >= 0xDA && marker <= 0xDF) || (marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE;
				if (marker == 0xD9) {
					if (scans_ == 0)
						return {}; // an image with no image in it
					return done(pos);
				}
				if (!segment || (first && marker == 0xDA))
					return first ? t_verdict{} : broken(marker_at, std::format("marker {:02X} doesn't belong there", marker));
				first = false;

				const std::int64_t length = r_.be16(pos);
				if (length < 2)
					return length < 0 ? broken(pos, "") : broken(marker_at, "a segment is shorter than its own length field");
				std::vector<std::uint8_t> body(static_cast<std::size_t>(length - 2));
				if (!r_.read(pos + 2, std::as_writable_bytes(std::span(body))))
					return broken(pos + static_cast<std::uint64_t>(length), "");

				bool ok = true;
				if (marker == 0xC4)
					ok = tables(body);
				else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xCC)
					ok = start_of_frame(marker, body);
				else if (marker == 0xDD) {
					ok = body.size() == 2;
					if (ok)
						restart_interval_ = std::uint32_t(body[0]) << 8 | body[1];
				}
				if (!ok)
					return broken(marker_at, std::format("its {:02X} segment doesn't make sense", marker));
				pos += static_cast<std::uint64_t>(length);

				if (marker == 0xDA) {
					if (!frame_ || ++scans_ > max_scans)
						return broken(marker_at, "image data comes before the frame header");
					if (data_start_ == 0)
						data_start_ = pos;
					const std::optional<std::uint64_t> after = decodable_ ? scan(body, pos) : skip_entropy(pos);
					if (!after)
						return failed_;
					pos = *after;
				}
			}
			return broken(pos, "it has more segments than any real JPEG");
		}

	private:
		bool tables(std::span<const std::uint8_t> body) {
			for (std::size_t i = 0; i < body.size();) {
				const int type = body[i] >> 4, slot = body[i] & 15;
				if (type > 1 || slot > 3 || body.size() - i < 17)
					return false;
				const std::span<const std::uint8_t> counts = body.subspan(i + 1, 16);
				std::size_t total = 0;
				for (std::uint8_t n : counts)
					total += n;
				if (total > 256 || body.size() - i - 17 < total)
					return false;
				if (!build(type == 0 ? dc_[static_cast<std::size_t>(slot)] : ac_[static_cast<std::size_t>(slot)], counts, body.subspan(i + 17, total)))
					return false;
				i += 17 + total;
			}
			return true;
		}

		bool start_of_frame(int marker, std::span<const std::uint8_t> body) {
			if (frame_ || body.size() < 6)
				return false;
			precision_ = body[0];
			height_ = std::uint32_t(body[1]) << 8 | body[2];
			width_ = std::uint32_t(body[3]) << 8 | body[4];
			const std::size_t n = body[5];
			if (width_ == 0 || n == 0 || n > 4 || body.size() != 6 + 3 * n || precision_ < 2 || precision_ > 16)
				return false;
			for (std::size_t i = 0; i < n; i++) {
				const t_component c{ body[6 + 3 * i], body[7 + 3 * i] >> 4, body[7 + 3 * i] & 15 };
				if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || body[8 + 3 * i] > 3)
					return false;
				hmax_ = std::max(hmax_, c.h);
				vmax_ = std::max(vmax_, c.v);
				components_.push_back(c);
			}
			frame_ = true;
			switch (marker) {
			case 0xC0: kind_ = "baseline"; break;
			case 0xC1: kind_ = "extended sequential"; break;
			case 0xC2: kind_ = "progressive"; break;
			case 0xC3: kind_ = "lossless"; break;
			default: kind_ = marker >= 0xC9 ? "arithmetic coded" : "hierarchical"; break;
			}
			// height 0 is given after the image data, in a DNL segment: there's no block count to check against
			decodable_ = (marker == 0xC0 && precision_ == 8) || (marker == 0xC1 && (precision_ == 8 || precision_ == 12));
			decodable_ = decodable_ && height_ != 0;
			return true;
		}

		static std::uint64_t ceil_div(std::uint64_t a, std::uint64_t b) { return (a + b - 1) / b; }

		// one sequential scan's blocks, every code decoded. where the data after it starts, or nothing with failed_ set
		std::optional<std::uint64_t> scan(std::span<const std::uint8_t> sos, std::uint64_t pos) {
			const std::size_t n = sos.empty() ? 0 : sos[0];
			if (n < 1 || n > 4 || sos.size() != 4 + 2 * n || sos[1 + 2 * n] != 0 || sos[2 + 2 * n] != 63 || sos[3 + 2 * n] != 0) {
				failed_ = broken(pos, "its scan header doesn't make sense");
				return std::nullopt;
			}
			struct t_part {
				int blocks;
				const t_table *dc, *ac;
			};
			std::vector<t_part> parts;
			int per_mcu = 0;
			for (std::size_t i = 0; i < n; i++) {
				const auto c = std::ranges::find(components_, int(sos[1 + 2 * i]), &t_component::id);
				const t_table& dc = dc_[static_cast<std::size_t>(sos[2 + 2 * i] >> 4) & 3];
				const t_table& ac = ac_[static_cast<std::size_t>(sos[2 + 2 * i] & 15) & 3];
				if (c == components_.end() || !dc.defined || !ac.defined || (sos[2 + 2 * i] >> 4) > 3 || (sos[2 + 2 * i] & 15) > 3) {
					failed_ = broken(pos, "its scan uses a component or a table that isn't defined");
					return std::nullopt;
				}
				parts.push_back({ n == 1 ? 1 : c->h * c->v, &dc, &ac });
				per_mcu += parts.back().blocks;
			}
			if (per_mcu > 10) {
				failed_ = broken(pos, "its scan has more blocks to a unit than JPEG allows");
				return std::nullopt;
			}

			std::uint64_t mcus;
			if (n == 1) {
				const auto c = std::ranges::find(components_, int(sos[1]), &t_component::id);
				mcus = ceil_div(ceil_div(std::uint64_t(width_) * std::uint64_t(c->h), std::uint64_t(hmax_)), 8)
					* ceil_div(ceil_div(std::uint64_t(height_) * std::uint64_t(c->v), std::uint64_t(vmax_)), 8);
			} else {
				mcus = ceil_div(width_, 8 * std::uint64_t(hmax_)) * ceil_div(height_, 8 * std::uint64_t(vmax_));
			}

			const int max_dc = precision_ == 8 ? 11 : 15, max_ac = precision_ == 8 ? 10 : 14;
			t_bits bits(r_, pos);
			for (std::uint64_t m = 0; m < mcus; m++) {
				if (restart_interval_ != 0 && m != 0 && m % restart_interval_ == 0 && !bits.restart(static_cast<int>((m / restart_interval_ - 1) % 8))) {
					failed_ = stop(bits.pos(), "a restart marker is missing where its image data needs one");
					return std::nullopt;
				}
				for (const t_part& part : parts)
					for (int b = 0; b < part.blocks; b++)
						if (!block(bits, *part.dc, *part.ac, max_dc, max_ac)) {
							failed_ = stop(bits.pos(), std::format("its image data stops decoding at byte {} of it, {} blocks in", bits.pos(), blocks_));
							return std::nullopt;
						}
			}
			return bits.end();
		}

		bool block(t_bits& bits, const t_table& dc, const t_table& ac, int max_dc, int max_ac) {
			const int category = bits.decode(dc);
			if (category < 0 || category > max_dc || !bits.skip(category))
				return false;
			for (int k = 1; k < 64;) {
				const int rs = bits.decode(ac);
				if (rs < 0)
					return false;
				const int run = rs >> 4, size = rs & 15;
				if (size == 0) {
					if (run == 0)
						break; // end of block
					if (run != 15)
						return false;
					k += 16;
					continue;
				}
				k += run;
				if (size > max_ac || k > 63 || !bits.skip(size))
					return false;
				k++;
			}
			blocks_++;
			return true;
		}

		// image data that isn't decoded: on to the next marker that isn't a stuffed 0xFF or a restart
		std::optional<std::uint64_t> skip_entropy(std::uint64_t pos) {
			for (;;) {
				const int b = r_.at(pos);
				if (b < 0) {
					failed_ = broken(pos, "");
					return std::nullopt;
				}
				if (b != 0xFF) {
					pos++;
					continue;
				}
				const int next = r_.at(pos + 1);
				if (next == 0x00 || (next >= 0xD0 && next <= 0xD7))
					pos += 2;
				else if (next == 0xFF)
					pos++;
				else if (next < 0) {
					failed_ = broken(pos + 1, "");
					return std::nullopt;
				} else
					return pos;
			}
		}

		// the image data stopped making sense at `at`: decoding failed there or a marker was wrong. the file was
		// fragmented or overwritten somewhere before, so what's kept is cut at a sector before it
		t_verdict stop(std::uint64_t at, std::string why) {
			if (r_.at(at) < 0)
				return broken(at, "");
			return partial(at - at % sector, std::move(why) + ": the file was fragmented or overwritten there, what's exported ends at the sector before, so its last few hundred bytes may not be its own");
		}

		// why empty: the bytes ran out (end of the source, the limit, a read error), and everything before was fine
		t_verdict broken(std::uint64_t at, std::string why) {
			if (why.empty())
				return partial(at, stopped(r_, at));
			return partial(at - at % sector, std::move(why) + " (at byte " + std::to_string(at) + " of it): the file was fragmented or overwritten there, what's exported ends at the sector before");
		}

		t_verdict partial(std::uint64_t recoverable, std::string why) {
			if (data_start_ == 0 || recoverable <= data_start_)
				return {}; // nothing of the image itself is left, only its headers
			t_verdict v{ .file = true, .recoverable = recoverable, .integrity = Integrity::partial };
			v.reasons.push_back(std::format("{}x{}, {}: {}", width_, height_, kind_, why));
			return v;
		}

		t_verdict done(std::uint64_t end) {
			t_verdict v{ .file = true, .recoverable = end, .size = end };
			if (decodable_) {
				v.integrity = Integrity::intact;
				v.reasons.push_back(std::format("{}x{}, {}: all {} blocks of its image data decode and it ends with an end-of-image marker, so it's in one piece", width_, height_, kind_, blocks_));
			} else {
				v.integrity = Integrity::uncertain;
				v.reasons.push_back(std::format("{}x{}, {}: its markers check out from start to end, but its image data isn't decoded, so a piece of another file inside it wouldn't show", width_, height_, kind_));
			}
			return v;
		}

		t_reader& r_;
		std::array<t_table, 4> dc_{}, ac_{};
		bool frame_ = false, decodable_ = false;
		int precision_ = 8, hmax_ = 1, vmax_ = 1, scans_ = 0;
		std::uint32_t width_ = 0, height_ = 0, restart_interval_ = 0;
		std::vector<t_component> components_;
		std::string_view kind_;
		std::uint64_t data_start_ = 0, blocks_ = 0;
		t_verdict failed_;
	};
}

storage::carving::t_verdict storage::carving::validate_jpeg(t_reader& reader) { return t_jpeg(reader).run(); }
