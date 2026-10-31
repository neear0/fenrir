// zip (pkware's APPNOTE 6.3): local entries one after another, each a header, its data and maybe a data descriptor,
// then the central directory, which lists them again with where each one is, then the end of central directory
// record. it's walked from the front: every entry has to start where the one before ends, every central entry has to
// point at one of them, and the end record has to agree with where the central directory is and how long it is.
// stored entries are checked against their crc; compressed ones aren't unpacked, the reasons say so

#include <format>
#include <memory>
#include <unordered_set>

#include "formats.h"
#include "storage/hash.h"

namespace {
	using namespace storage;
	using namespace storage::carving;

	constexpr std::uint32_t local_sig = 0x04034B50, central_sig = 0x02014B50, end_sig = 0x06054B50, descriptor_sig = 0x08074B50;
	constexpr std::uint32_t zip64_end_sig = 0x06064B50, zip64_locator_sig = 0x07064B50;
	constexpr std::size_t max_entries = std::size_t(1) << 20, max_name = 4096, piece = std::size_t(64) << 10;

	struct t_entry {
		std::uint64_t at = 0, end = 0;
		std::string name;
		bool verified = false; // its data matched its crc
		std::uint16_t method = 0;
		std::uint64_t size = 0; // compressed
	};

	class t_zip {
	public:
		explicit t_zip(t_reader& reader) : r_(reader) {}

		t_verdict run() {
			std::uint64_t pos = 0;
			for (;;) {
				const std::int64_t sig = r_.le32(pos);
				if (sig == local_sig && entries_.size() < max_entries) {
					std::string why;
					if (!local(pos, why))
						return entries_.empty() ? t_verdict{} : broken(why);
					pos = entries_.back().end;
				} else if (sig == central_sig && !entries_.empty()) {
					return central(pos);
				} else {
					return broken(sig < 0 ? stopped(r_, pos)
						: std::format("the next entry should start at byte {} of it and doesn't: the file was fragmented or overwritten there", pos));
				}
			}
		}

	private:
		// the local entry at pos, onto entries_. false with why when it isn't one, or its data doesn't hold up
		bool local(std::uint64_t pos, std::string& why) {
			const std::int64_t flags = r_.le16(pos + 6), method = r_.le16(pos + 8);
			std::int64_t crc = r_.le32(pos + 14);
			std::int64_t compressed = r_.le32(pos + 18), uncompressed = r_.le32(pos + 22);
			const std::int64_t name_length = r_.le16(pos + 26), extra_length = r_.le16(pos + 28), version = r_.at(pos + 4);
			if (extra_length < 0) {
				why = stopped(r_, pos + 30);
				return false;
			}
			const bool known_method = (method >= 0 && method <= 9) || method == 12 || method == 14 || method == 93 || method == 95 || method == 98 || method == 99;
			if (!known_method || version > 63 || name_length == 0 || static_cast<std::size_t>(name_length) > max_name) {
				why = std::format("the entry at byte {} of it has a header no zip writer makes", pos);
				return false;
			}
			t_entry e{ .at = pos };
			for (std::int64_t i = 0; i < name_length; i++) {
				const int c = r_.at(pos + 30 + static_cast<std::uint64_t>(i));
				if (c <= 0) {
					why = c < 0 ? stopped(r_, pos + 30 + static_cast<std::uint64_t>(i)) : std::format("the entry at byte {} of it has a header no zip writer makes", pos);
					return false;
				}
				e.name += static_cast<char>(c);
			}
			const std::uint64_t extra = pos + 30 + static_cast<std::uint64_t>(name_length);
			const std::uint64_t data = extra + static_cast<std::uint64_t>(extra_length);
			if (compressed == 0xFFFFFFFF || uncompressed == 0xFFFFFFFF)
				zip64_sizes(extra, static_cast<std::uint64_t>(extra_length), uncompressed, compressed);

			const bool encrypted = (flags & 1) != 0, later = (flags & 8) != 0;
			std::uint64_t descriptor = 0;
			if (later && !find_descriptor(data, compressed, uncompressed, descriptor)) {
				why = std::format("the entry at byte {} of it doesn't end anywhere it can be found: the file was fragmented or overwritten there", pos);
				return false;
			}
			if (later) { // the crc is in the descriptor, after its signature when it has one
				const std::uint64_t at = data + static_cast<std::uint64_t>(compressed);
				crc = r_.le32(at + (descriptor == 16 ? 4 : 0));
			}
			e.method = static_cast<std::uint16_t>(method);
			e.size = static_cast<std::uint64_t>(compressed);
			e.end = data + static_cast<std::uint64_t>(compressed) + descriptor;
			if (r_.at(e.end - 1) < 0) {
				why = stopped(r_, e.end);
				return false;
			}
			if (method == 0 && !encrypted) {
				if (compressed != uncompressed || data_crc(data, static_cast<std::uint64_t>(compressed)) != crc) {
					why = std::format("the data of the entry at byte {} of it doesn't match its CRC: the file was fragmented or overwritten there", pos);
					return false;
				}
				e.verified = true;
				verified_++;
			}
			if (entries_.empty() || e.name.size() < 64)
				names_.insert(e.name);
			entries_.push_back(std::move(e));
			return true;
		}

		// the 64 bit sizes in a zip64 extra field (0x0001), when the header's are 0xFFFFFFFF
		void zip64_sizes(std::uint64_t at, std::uint64_t length, std::int64_t& uncompressed, std::int64_t& compressed) {
			for (std::uint64_t i = 0; i + 4 <= length;) {
				const std::int64_t id = r_.le16(at + i), size = r_.le16(at + i + 2);
				if (id < 0 || size < 0)
					return;
				if (id == 1 && size >= 16) {
					const std::int64_t u = r_.le32(at + i + 4) | r_.le32(at + i + 8) << 32, c = r_.le32(at + i + 12) | r_.le32(at + i + 16) << 32;
					if (u >= 0 && c >= 0) {
						uncompressed = u;
						compressed = c;
					}
					return;
				}
				i += 4 + static_cast<std::uint64_t>(size);
			}
		}

		// an entry whose sizes come after its data: the first descriptor whose compressed size is the distance from data
		// to it, signed (PK 07 08) or not (then a header has to follow it)
		bool find_descriptor(std::uint64_t data, std::int64_t& compressed, std::int64_t& uncompressed, std::uint64_t& length) {
			for (std::uint64_t q = data;; q++) {
				const int c = r_.at(q);
				if (c < 0)
					return false;
				if (c != 'P' || r_.at(q + 1) != 'K')
					continue;
				const std::int64_t sig = r_.le32(q), distance = static_cast<std::int64_t>(q - data);
				if (sig == descriptor_sig && r_.le32(q + 8) == distance) {
					compressed = distance;
					uncompressed = r_.le32(q + 12);
					length = 16;
					return true;
				}
				if ((sig == local_sig || sig == central_sig) && q >= data + 12 && r_.le32(q - 8) == static_cast<std::int64_t>(q - 12 - data)) {
					compressed = static_cast<std::int64_t>(q - 12 - data);
					uncompressed = r_.le32(q - 4);
					length = 12;
					return true;
				}
			}
		}

		std::int64_t data_crc(std::uint64_t at, std::uint64_t length) {
			std::unique_ptr<std::byte[]> buffer(new std::byte[piece]);
			std::uint32_t crc = 0;
			for (std::uint64_t done = 0; done < length;) {
				const std::span<std::byte> part(buffer.get(), static_cast<std::size_t>(std::min<std::uint64_t>(piece, length - done)));
				if (!r_.read(at + done, part))
					return -1;
				crc = crc32(part, crc);
				done += part.size();
			}
			return crc;
		}

		// the central directory at pos and the end record after it, which close the file
		t_verdict central(std::uint64_t pos) {
			const std::uint64_t start = pos;
			std::unordered_set<std::uint64_t> locals;
			for (const t_entry& e : entries_)
				locals.insert(e.at);
			std::uint64_t count = 0;
			bool mismatched = false;
			while (r_.le32(pos) == central_sig && count < max_entries) {
				const std::int64_t name = r_.le16(pos + 28), extra = r_.le16(pos + 30), comment = r_.le16(pos + 32);
				std::int64_t offset = r_.le32(pos + 42);
				if (comment < 0)
					return broken(stopped(r_, pos + 46));
				if (offset == 0xFFFFFFFF)
					offset = -1; // in a zip64 extra field; not followed
				mismatched |= offset >= 0 && !locals.contains(static_cast<std::uint64_t>(offset));
				pos += 46 + static_cast<std::uint64_t>(name + extra + comment);
				count++;
			}
			const std::uint64_t central_size = pos - start;

			bool zip64 = false;
			if (r_.le32(pos) == zip64_end_sig) {
				const std::int64_t size = r_.le32(pos + 4) | r_.le32(pos + 8) << 32;
				if (size < 44 || size > 0xFFFF)
					return broken(std::format("its zip64 end record at byte {} of it doesn't make sense", pos));
				pos += 12 + static_cast<std::uint64_t>(size);
				if (r_.le32(pos) != zip64_locator_sig)
					return broken(std::format("its zip64 end record isn't followed by its locator, at byte {} of it", pos));
				pos += 20;
				zip64 = true;
			}
			if (r_.le32(pos) != end_sig)
				return broken(r_.at(pos) < 0 ? stopped(r_, pos) : std::format("its end record should be at byte {} of it and isn't: the file was fragmented or overwritten there", pos));
			const std::int64_t entries = r_.le16(pos + 10), size = r_.le32(pos + 12), offset = r_.le32(pos + 16), comment = r_.le16(pos + 20);
			if (comment < 0 || r_.at(pos + 22 + static_cast<std::uint64_t>(comment) - 1) < 0)
				return broken(stopped(r_, pos + 22));
			const std::uint64_t end = pos + 22 + static_cast<std::uint64_t>(comment);

			const bool agrees = (zip64 || (entries == static_cast<std::int64_t>(count) && size == static_cast<std::int64_t>(central_size) && offset == static_cast<std::int64_t>(start)))
				&& count == entries_.size() && !mismatched;
			if (!agrees) {
				t_verdict v = broken(std::format("its central directory doesn't agree with the {} entries before it: the file was fragmented or overwritten there, or it's a zip with something in front of it", entries_.size()));
				return v;
			}
			t_verdict v{ .file = true, .recoverable = end, .size = end, .integrity = Integrity::intact };
			identify(v);
			const std::size_t compressed = entries_.size() - verified_;
			v.reasons.push_back(std::format("{} {}, each where the one before ends, and the central directory and end record agree with them{}", entries_.size(), entries_.size() == 1 ? "entry" : "entries",
				zip64 ? " (zip64: the end record's sizes aren't checked)" : ""));
			if (verified_ == entries_.size())
				v.reasons.push_back("every entry is stored, and matches its CRC");
			else if (verified_ != 0)
				v.reasons.push_back(std::format("{} of them {} stored and {} {} CRC", verified_, verified_ == 1 ? "is" : "are", verified_ == 1 ? "matches" : "match",
					verified_ == 1 ? "its" : "their"));
			if (compressed != 0)
				v.reasons.push_back(std::format("{} compressed, and not unpacked, so damage inside {} wouldn't show", compressed == 1 ? "1 is" : std::format("{} are", compressed),
					compressed == 1 ? "it" : "one of them"));
			return v;
		}

		// the structure stopped: everything up to the last entry it's sure of. an entry whose data wasn't checked against
		// its crc could have the break inside it, so that one's left out
		t_verdict broken(std::string why) {
			std::size_t keep = entries_.size();
			if (keep != 0 && !entries_.back().verified)
				keep--;
			if (keep == 0)
				return {};
			const std::uint64_t end = entries_[keep - 1].end;
			t_verdict v{ .file = true, .recoverable = end, .integrity = Integrity::partial };
			identify(v);
			const std::string kept = keep == entries_.size() ? std::format("all {} of its entries are", keep)
				: std::format("the first {} of its {} entries {}", keep, entries_.size(), keep == 1 ? "is" : "are");
			v.reasons.push_back(std::format("{}. {} exported, without a central directory, so most tools need to repair it first", why, kept));
			return v;
		}

		// a zip's names say what it is: office documents, opendocument, epub, java and android packages
		void identify(t_verdict& v) {
			auto any = [&](std::string_view prefix) { return std::ranges::any_of(names_, [&](const std::string& n) { return n.starts_with(prefix); }); };
			auto mimetype = [&]() -> std::string {
				const t_entry& first = entries_.front();
				if (first.name != "mimetype" || first.method != 0 || first.size > 100)
					return {};
				std::string text;
				const std::uint64_t data = first.end - first.size;
				for (std::uint64_t i = 0; i < first.size; i++)
					text += static_cast<char>(r_.at(data + i));
				return text;
			};
			if (names_.contains("[Content_Types].xml")) {
				if (any("word/"))
					v.format = "DOCX", v.extension = "docx";
				else if (any("xl/"))
					v.format = "XLSX", v.extension = "xlsx";
				else if (any("ppt/"))
					v.format = "PPTX", v.extension = "pptx";
				return;
			}
			const std::string type = mimetype();
			if (type == "application/vnd.oasis.opendocument.text")
				v.format = "ODT", v.extension = "odt";
			else if (type == "application/vnd.oasis.opendocument.spreadsheet")
				v.format = "ODS", v.extension = "ods";
			else if (type == "application/vnd.oasis.opendocument.presentation")
				v.format = "ODP", v.extension = "odp";
			else if (type == "application/epub+zip")
				v.format = "EPUB", v.extension = "epub";
			else if (names_.contains("AndroidManifest.xml") && names_.contains("classes.dex"))
				v.format = "APK", v.extension = "apk";
			else if (names_.contains("META-INF/MANIFEST.MF"))
				v.format = "JAR", v.extension = "jar";
		}

		t_reader& r_;
		std::vector<t_entry> entries_;
		std::unordered_set<std::string> names_; // short ones, enough to tell what kind of zip it is
		std::size_t verified_ = 0;
	};
}

storage::carving::t_verdict storage::carving::validate_zip(t_reader& reader) { return t_zip(reader).run(); }
