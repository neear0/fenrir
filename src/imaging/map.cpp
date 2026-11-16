// the ddrescue mapfile: parsing it (hostile input, like any file read from a disk) and writing it the way ddrescue does

#include <algorithm>
#include <charconv>
#include <format>
#include <iterator>
#include <new>

#include "storage/bytes.h"
#include "storage/imaging.h"

namespace {
	using namespace storage;

	constexpr std::size_t max_blocks = 50'000'000; // a mapfile this big would be over a gigabyte of text

	int index_of(BlockStatus status) noexcept {
		switch (status) {
		case BlockStatus::non_tried: return 0;
		case BlockStatus::non_trimmed: return 1;
		case BlockStatus::non_scraped: return 2;
		case BlockStatus::bad: return 3;
		case BlockStatus::finished: return 4;
		}
		return 0;
	}

	std::optional<BlockStatus> block_status(char c) noexcept {
		switch (c) {
		case '?': return BlockStatus::non_tried;
		case '*': return BlockStatus::non_trimmed;
		case '/': return BlockStatus::non_scraped;
		case '-': return BlockStatus::bad;
		case '+': return BlockStatus::finished;
		}
		return std::nullopt;
	}

	std::optional<ImagingPhase> phase_of(char c) noexcept {
		switch (c) {
		case '?': return ImagingPhase::copying;
		case '*': return ImagingPhase::trimming;
		case '/': return ImagingPhase::scraping;
		case '-': return ImagingPhase::retrying;
		case '+': return ImagingPhase::finished;
		}
		return std::nullopt; // ddrescue's 'F' (filling) and 'G' (generating) maps aren't copies Fenrir can go on with
	}

	// a line split at spaces and tabs
	std::vector<std::string_view> fields(std::string_view line) {
		std::vector<std::string_view> out;
		std::size_t i = 0;
		while (i < line.size()) {
			while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
				i++;
			const std::size_t start = i;
			while (i < line.size() && line[i] != ' ' && line[i] != '\t')
				i++;
			if (i > start)
				out.push_back(line.substr(start, i - start));
		}
		return out;
	}

	// 0x... in hex, otherwise decimal. a leading 0 that isn't 0x would be octal to ddrescue's scanf: refused rather than misread
	std::optional<std::uint64_t> number(std::string_view text) noexcept {
		int base = 10;
		if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
			text.remove_prefix(2);
			base = 16;
		} else if (text.size() > 1 && text[0] == '0')
			return std::nullopt;
		std::uint64_t value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
		if (error != std::errc() || end != text.data() + text.size() || text.empty())
			return std::nullopt;
		return value;
	}

	Result<ImageMap> parse_map(std::string_view text) {
		ImageMap map;
		bool status_line = false;
		std::uint64_t next = 0; // where the next block has to start
		std::vector<MapBlock> blocks;

		while (!text.empty()) {
			const std::size_t newline = text.find('\n');
			std::string_view line = text.substr(0, newline);
			text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			const std::vector<std::string_view> f = fields(line);
			if (f.empty() || f[0].starts_with('#'))
				continue;

			if (!status_line) {
				if (f.size() < 2 || f.size() > 3 || f[1].size() != 1)
					return fail(Errc::invalid_argument, "reading a mapfile: its status line isn't one");
				const std::optional<std::uint64_t> pos = number(f[0]);
				const std::optional<ImagingPhase> phase = phase_of(f[1][0]);
				const std::optional<std::uint64_t> pass = f.size() == 3 ? number(f[2]) : std::optional<std::uint64_t>(1);
				if (!pos || !phase || !pass || *pass > 1000)
					return fail(Errc::invalid_argument, "reading a mapfile: its status line isn't one");
				map.current_pos = *pos;
				map.phase = *phase;
				map.pass = static_cast<unsigned>(*pass);
				status_line = true;
				continue;
			}

			if (f.size() != 3 || f[2].size() != 1)
				return fail(Errc::invalid_argument, "reading a mapfile: a block line isn't one");
			const std::optional<std::uint64_t> pos = number(f[0]), size = number(f[1]);
			const std::optional<BlockStatus> status = block_status(f[2][0]);
			if (!pos || !size || !status)
				return fail(Errc::invalid_argument, "reading a mapfile: a block line isn't one");
			if (*pos != next || *size == 0)
				return fail(Errc::invalid_argument, "reading a mapfile: its blocks have gaps, overlap or are empty");
			const std::optional<std::uint64_t> end = checked_add(*pos, *size);
			if (!end)
				return fail(Errc::overflow, "reading a mapfile: a block ends past 2^64");
			if (blocks.size() >= max_blocks)
				return fail(Errc::invalid_argument, "reading a mapfile: too many blocks");
			blocks.push_back({ *pos, *size, *status });
			next = *end;
		}
		if (!status_line)
			return fail(Errc::invalid_argument, "reading a mapfile: it has no status line");
		if (blocks.empty())
			return fail(Errc::invalid_argument, "reading a mapfile: it has no blocks");

		ImageMap out(next);
		for (const MapBlock& block : blocks)
			if (block.status != BlockStatus::non_tried)
				out.set(block.pos, block.size, block.status);
		out.current_pos = map.current_pos;
		out.phase = map.phase;
		out.pass = map.pass;
		return out;
	}
}

storage::ImageMap::ImageMap(std::uint64_t size) : size_(size) {
	if (size != 0) {
		blocks_.emplace(0, t_block{ size, BlockStatus::non_tried });
		count(BlockStatus::non_tried, size, true);
	}
}

void storage::ImageMap::count(BlockStatus status, std::uint64_t size, bool add) noexcept {
	std::uint64_t& total = totals_[index_of(status)];
	total = add ? total + size : total - size;
}

// a block boundary at `at`, splitting the block it's inside of
void storage::ImageMap::split(std::uint64_t at) {
	if (at == 0 || at >= size_)
		return;
	auto it = std::prev(blocks_.upper_bound(at));
	if (it->first == at)
		return;
	const std::uint64_t first_size = at - it->first;
	const t_block rest{ it->second.size - first_size, it->second.status };
	it->second.size = first_size;
	blocks_.emplace_hint(std::next(it), at, rest);
}

void storage::ImageMap::set(std::uint64_t pos, std::uint64_t size, BlockStatus status) {
	if (pos >= size_ || size == 0)
		return;
	const std::uint64_t end = std::min(checked_add(pos, size).value_or(size_), size_);
	split(pos);
	split(end);

	auto it = blocks_.find(pos);
	while (it != blocks_.end() && it->first < end) {
		count(it->second.status, it->second.size, false);
		it = blocks_.erase(it);
	}
	it = blocks_.emplace_hint(it, pos, t_block{ end - pos, status });
	count(status, end - pos, true);

	// merge with the neighbours that have the same status
	if (auto after = std::next(it); after != blocks_.end() && after->second.status == status) {
		it->second.size += after->second.size;
		blocks_.erase(after);
	}
	if (it != blocks_.begin()) {
		auto before = std::prev(it);
		if (before->second.status == status) {
			before->second.size += it->second.size;
			blocks_.erase(it);
		}
	}
}

storage::MapBlock storage::ImageMap::block_at(std::uint64_t pos) const noexcept {
	if (pos >= size_)
		return { size_, 0, BlockStatus::non_tried };
	const auto it = std::prev(blocks_.upper_bound(pos));
	return { it->first, it->second.size, it->second.status };
}

std::optional<storage::MapBlock> storage::ImageMap::find(BlockStatus status, std::uint64_t from) const noexcept {
	if (from >= size_)
		return std::nullopt;
	for (auto it = std::prev(blocks_.upper_bound(from)); it != blocks_.end(); ++it)
		if (it->second.status == status) {
			const std::uint64_t start = std::max(it->first, from);
			return MapBlock{ start, it->first + it->second.size - start, status };
		}
	return std::nullopt;
}

std::uint64_t storage::ImageMap::bytes(BlockStatus status) const noexcept { return totals_[index_of(status)]; }

std::vector<storage::MapBlock> storage::ImageMap::blocks() const {
	std::vector<MapBlock> out;
	out.reserve(blocks_.size());
	for (const auto& [pos, block] : blocks_)
		out.push_back({ pos, block.size, block.status });
	return out;
}

std::string storage::ImageMap::serialize(std::string_view comment) const {
	std::string out = "# Mapfile. Created by Fenrir\n";
	while (!comment.empty()) {
		const std::size_t newline = comment.find('\n');
		out += "# ";
		out += comment.substr(0, newline);
		out += '\n';
		comment.remove_prefix(newline == std::string_view::npos ? comment.size() : newline + 1);
	}
	out += "# current_pos  current_status  current_pass\n";
	out += std::format("0x{:08X}     {}               {}\n", current_pos, static_cast<char>(phase), pass);
	out += "#      pos        size  status\n";
	for (const auto& [pos, block] : blocks_)
		std::format_to(std::back_inserter(out), "0x{:08X}  0x{:08X}  {}\n", pos, block.size, static_cast<char>(block.status));
	return out;
}

storage::Result<storage::ImageMap> storage::ImageMap::parse(std::string_view text) noexcept {
	try {
		return parse_map(text);
	} catch (const std::bad_alloc&) {
		return fail(Errc::out_of_memory, "reading a mapfile");
	}
}
