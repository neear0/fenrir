// pdf (iso 32000): the header, then objects ("12 0 obj ... endobj", streams skipped by their /Length), cross-reference
// tables and trailers, and startxref with %%EOF at the end, again for every incremental update after it. it's walked
// from the front, so where it stops being a pdf is where a fragment ends. at the end, startxref has to point at a
// cross-reference section, and every entry of a cross-reference table at the object it names: a piece of something
// else inside the file would move them

#include <format>
#include <optional>
#include <unordered_map>

#include "formats.h"

namespace {
	using namespace storage;
	using namespace storage::carving;

	constexpr std::size_t max_objects = std::size_t(1) << 20, max_word = 64, max_name = 128;

	bool space(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
	bool delimiter(int c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }
	bool regular(int c) { return c >= 0 && !space(c) && !delimiter(c); }
	bool digit(int c) { return c >= '0' && c <= '9'; }

	class t_pdf {
	public:
		explicit t_pdf(t_reader& reader) : r_(reader) {}

		t_verdict run() {
			if (!digit(r_.at(5)) || r_.at(6) != '.' || !digit(r_.at(7)))
				return {};
			std::uint64_t pos = 8;
			while (r_.at(pos) >= 0 && r_.at(pos) != '\r' && r_.at(pos) != '\n' && pos < 64)
				pos++;
			last_good_ = pos;

			for (;;) {
				pos = skip_space(pos, false);
				const int c = r_.at(pos);
				if (c < 0)
					return finish(stopped(r_, pos));
				if (digit(c)) {
					const std::uint64_t start = pos;
					const std::optional<std::uint64_t> number = object_header(pos);
					if (!number)
						return finish(std::format("something that isn't a PDF object starts at byte {} of it", start));
					if (!skip_object(pos))
						return finish(why_);
					if (objects_.size() < max_objects)
						objects_[start] = *number;
					count_++;
					last_good_ = pos;
				} else if (keyword(pos, "xref")) {
					const std::uint64_t start = pos;
					if (!table(pos))
						return finish(why_.empty() ? std::format("its cross-reference table at byte {} of it doesn't make sense", start) : why_);
					tables_++;
					last_good_ = pos;
				} else if (keyword(pos, "startxref")) {
					const std::uint64_t start = pos;
					pos = skip_space(pos + 9);
					const std::optional<std::uint64_t> target = integer(pos);
					pos = skip_space(pos);
					if (!target || !r_.matches(pos, "%%EOF"))
						return finish(r_.at(pos) < 0 ? stopped(r_, pos) : std::format("its startxref at byte {} of it isn't followed by an end marker", start));
					pos += 5;
					if (r_.at(pos) == '\r')
						pos++;
					if (r_.at(pos) == '\n')
						pos++;
					// 0 is a linearized file's first page section, the real one comes at the end
					if (*target != 0) {
						if (!keyword(*target, "xref") && !objects_.contains(*target))
							return finish(std::format("its startxref points at byte {}, where there's no cross-reference section: the file was fragmented or overwritten before that", *target));
						streams_ += !keyword(*target, "xref");
						end_ = pos;
						ends_++;
					}
					last_good_ = pos;
					// an incremental update goes on after the end marker, anything else isn't this file's
					std::uint64_t next = pos;
					while (space(r_.at(next)) && r_.at(next) != 0)
						next++;
					std::uint64_t probe = next;
					if (*target != 0 && !keyword(next, "xref") && !object_header(probe))
						return done();
					pos = next;
				} else {
					return finish(std::format("something that isn't PDF starts at byte {} of it", pos));
				}
			}
		}

	private:
		// whitespace and comments. between objects a NUL isn't skipped: pdf allows it, but it's what follows a pdf on a
		// disk, and the walk has to stop there rather than at the end of the zeros
		std::uint64_t skip_space(std::uint64_t pos, bool nul = true) {
			for (;;) {
				const int c = r_.at(pos);
				if (space(c) && (nul || c != 0)) {
					pos++;
				} else if (c == '%' && !r_.matches(pos, "%%EOF")) {
					while (r_.at(pos) >= 0 && r_.at(pos) != '\r' && r_.at(pos) != '\n')
						pos++;
				} else {
					return pos;
				}
			}
		}

		bool keyword(std::uint64_t pos, std::string_view word) { return r_.matches(pos, word) && !regular(r_.at(pos + word.size())); }

		std::optional<std::uint64_t> integer(std::uint64_t& pos) {
			std::uint64_t value = 0;
			int n = 0;
			for (; digit(r_.at(pos)) && n < 19; n++, pos++)
				value = value * 10 + static_cast<std::uint64_t>(r_.at(pos) - '0');
			if (n == 0 || regular(r_.at(pos)))
				return std::nullopt;
			return value;
		}

		// "12 0 obj": the object's number, and pos after it
		std::optional<std::uint64_t> object_header(std::uint64_t& pos) {
			std::uint64_t p = pos;
			const std::optional<std::uint64_t> number = integer(p);
			if (!number || !space(r_.at(p)))
				return std::nullopt;
			p = skip_space(p);
			if (!integer(p) || !space(r_.at(p)))
				return std::nullopt;
			p = skip_space(p);
			if (!keyword(p, "obj"))
				return std::nullopt;
			pos = p + 3;
			return number;
		}

		// (a literal string), with nested brackets and escapes
		bool skip_string(std::uint64_t& pos) {
			int depth = 0;
			for (;;) {
				const int c = r_.at(pos++);
				if (c < 0)
					return false;
				if (c == '\\')
					pos++;
				else if (c == '(')
					depth++;
				else if (c == ')' && --depth == 0)
					return true;
			}
		}

		// tokens until endobj, streams skipped. false with why_ set when it doesn't get there
		bool skip_object(std::uint64_t& pos) {
			int depth = 0;
			std::optional<std::uint64_t> length;
			for (;;) {
				pos = skip_space(pos);
				const int c = r_.at(pos);
				if (c < 0)
					return fail(stopped(r_, pos));
				if (c == '(') {
					if (!skip_string(pos))
						return fail(stopped(r_, pos));
				} else if (c == '<' && r_.at(pos + 1) == '<') {
					depth++;
					pos += 2;
				} else if (c == '>' && r_.at(pos + 1) == '>') {
					depth--;
					pos += 2;
				} else if (c == '<') {
					while (r_.at(pos) >= 0 && r_.at(pos) != '>')
						pos++;
					pos++;
				} else if (c == '/') {
					std::string name;
					for (pos++; regular(r_.at(pos)) && name.size() < max_name; pos++)
						name += static_cast<char>(r_.at(pos));
					if (name == "Length" && depth == 1) {
						std::uint64_t p = skip_space(pos);
						length = integer(p);
						std::uint64_t q = skip_space(p);
						if (length && integer(q) && keyword(skip_space(q), "R"))
							length.reset(); // 5 0 R, in another object
					}
				} else if (c == ')' || c == '>') {
					return fail(std::format("a stray '{}' at byte {} of it", static_cast<char>(c), pos));
				} else if (delimiter(c)) {
					pos++;
				} else {
					std::string word;
					for (; regular(r_.at(pos)) && word.size() < max_word; pos++)
						word += static_cast<char>(r_.at(pos));
					if (word == "endobj")
						return true;
					if (word == "obj")
						return fail(std::format("an object starts inside another, at byte {} of it", pos));
					if (word == "stream" && !skip_stream(pos, length))
						return false;
				}
			}
		}

		// pos is just after "stream": past the data and endstream. by the dictionary's /Length when it's given there,
		// else by finding endstream
		bool skip_stream(std::uint64_t& pos, std::optional<std::uint64_t> length) {
			if (r_.at(pos) == '\r')
				pos++;
			if (r_.at(pos) != '\n' && r_.at(pos - 1) != '\r')
				return fail(std::format("a stream at byte {} of it doesn't start on a new line", pos));
			if (r_.at(pos) == '\n')
				pos++;
			if (length) {
				std::uint64_t end = pos + *length;
				while (space(r_.at(end)) && r_.at(end) != 0)
					end++;
				if (keyword(end, "endstream")) {
					pos = end + 9;
					return true;
				}
			}
			for (std::uint64_t q = pos;; q++) {
				const int c = r_.at(q);
				if (c < 0)
					return fail(stopped(r_, q));
				if (c == 'e' && keyword(q, "endstream")) {
					pos = q + 9;
					return true;
				}
			}
		}

		// xref, subsections of "first count" and 20 byte entries, then trailer and its dictionary
		bool table(std::uint64_t& pos) {
			pos += 4;
			for (;;) {
				pos = skip_space(pos);
				if (keyword(pos, "trailer"))
					break;
				const std::optional<std::uint64_t> first = integer(pos);
				pos = skip_space(pos);
				const std::optional<std::uint64_t> count = integer(pos);
				if (!first || !count)
					return false;
				for (std::uint64_t i = 0; i < *count; i++) {
					pos = skip_space(pos);
					const std::optional<std::uint64_t> offset = integer(pos);
					pos = skip_space(pos);
					const std::optional<std::uint64_t> generation = integer(pos);
					pos = skip_space(pos);
					const int type = r_.at(pos++);
					if (!offset || !generation || (type != 'n' && type != 'f'))
						return r_.at(pos - 1) < 0 ? fail(stopped(r_, pos)) : false;
					if (type == 'n' && xref_.size() < max_objects)
						xref_.emplace_back(*offset, *first + i);
				}
			}
			pos = skip_space(pos + 7);
			if (!r_.matches(pos, "<<"))
				return false;
			int depth = 0;
			for (;;) {
				const int c = r_.at(pos);
				if (c < 0)
					return fail(stopped(r_, pos));
				if (c == '(') {
					if (!skip_string(pos))
						return fail(stopped(r_, pos));
				} else if (c == '<' && r_.at(pos + 1) == '<') {
					depth++;
					pos += 2;
				} else if (c == '>' && r_.at(pos + 1) == '>') {
					pos += 2;
					if (--depth == 0)
						return true;
				} else {
					pos++;
				}
			}
		}

		bool fail(std::string why) {
			why_ = std::move(why);
			return false;
		}

		// the walk stopped. after an end marker, what came after just isn't part of it
		t_verdict finish(std::string why) {
			if (end_ != 0)
				return done();
			if (count_ == 0)
				return {};
			t_verdict v{ .file = true, .recoverable = last_good_, .integrity = Integrity::partial };
			v.reasons.push_back(std::format("{} objects check out, then {}. those are exported; without the cross-reference table at a PDF's end, a reader has to rebuild it", count_, why));
			return v;
		}

		t_verdict done() {
			std::size_t wrong = 0;
			for (const auto& [offset, number] : xref_) {
				const auto found = objects_.find(offset);
				wrong += found == objects_.end() || found->second != number;
			}
			t_verdict v{ .file = true, .recoverable = end_, .size = end_ };
			if (wrong != 0 && objects_.size() < max_objects) {
				v.integrity = Integrity::uncertain;
				v.reasons.push_back(std::format("{} objects, but {} of its {} cross-reference entries don't point at the object they name: something in it moved", count_, wrong, xref_.size()));
				return v;
			}
			v.integrity = Integrity::intact;
			if (ends_ == 1)
				v.reasons.push_back(std::format("{} objects, then a startxref that points at its cross-reference section and the end marker", count_));
			else
				v.reasons.push_back(std::format("{} objects in {} revisions (incremental updates), each ending in a startxref that points at its cross-reference section", count_, ends_));
			if (tables_ != 0)
				v.reasons.push_back(std::format("every one of the {} entries in its cross-reference tables points at the object it names", xref_.size()));
			if (streams_ != 0)
				v.reasons.push_back("a cross-reference stream isn't unpacked, so only where it is was checked, not what it says");
			if (last_good_ > end_)
				v.reasons.push_back("something after its last end marker started like an update to it and didn't hold up, it's left out");
			return v;
		}

		t_reader& r_;
		std::unordered_map<std::uint64_t, std::uint64_t> objects_; // where each object starts, its number
		std::vector<std::pair<std::uint64_t, std::uint64_t>> xref_; // in-use table entries: offset, number
		std::uint64_t count_ = 0, last_good_ = 0, end_ = 0;
		std::size_t tables_ = 0, streams_ = 0, ends_ = 0;
		std::string why_;
	};
}

storage::carving::t_verdict storage::carving::validate_pdf(t_reader& reader) { return t_pdf(reader).run(); }
