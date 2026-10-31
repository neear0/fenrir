// png (iso 15948 / w3c png): the signature, then chunks of length, type, data and a crc over type and data, from IHDR
// to IEND. every chunk's crc is checked, so where a fragment ends is known to the chunk: everything before the first
// chunk that doesn't match is this file's

#include <format>
#include <memory>

#include "formats.h"
#include "storage/hash.h"

namespace {
	using namespace storage;
	using namespace storage::carving;

	constexpr std::uint64_t signature = 8, max_chunks = std::uint64_t(1) << 20;
	constexpr std::size_t piece = std::size_t(64) << 10;

	bool letter(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

	// the crc of the type and data of the chunk at pos, or -1 when not all of it can be read
	std::int64_t chunk_crc(t_reader& r, std::uint64_t pos, std::uint64_t length) {
		std::unique_ptr<std::byte[]> buffer(new std::byte[piece]);
		std::uint32_t crc = 0;
		for (std::uint64_t done = 0; done < length + 4;) {
			const std::span<std::byte> part(buffer.get(), static_cast<std::size_t>(std::min<std::uint64_t>(piece, length + 4 - done)));
			if (!r.read(pos + 4 + done, part))
				return -1;
			crc = crc32(part, crc);
			done += part.size();
		}
		return crc;
	}

	// png's own rules for the header: a size, and a bit depth that goes with the colour type
	bool header(t_reader& r, std::uint32_t& width, std::uint32_t& height) {
		const std::int64_t w = r.be32(16), h = r.be32(20);
		const int depth = r.at(24), colour = r.at(25);
		if (w <= 0 || h <= 0 || w > 0x7FFFFFFF || h > 0x7FFFFFFF || r.at(26) != 0 || r.at(27) != 0 || (r.at(28) != 0 && r.at(28) != 1))
			return false;
		const bool valid = (colour == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16))
			|| (colour == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8))
			|| ((colour == 2 || colour == 4 || colour == 6) && (depth == 8 || depth == 16));
		width = static_cast<std::uint32_t>(w);
		height = static_cast<std::uint32_t>(h);
		return valid;
	}
}

storage::carving::t_verdict storage::carving::validate_png(t_reader& r) {
	std::uint32_t width = 0, height = 0;
	if (r.be32(signature) != 13 || !r.matches(signature + 4, "IHDR") || !header(r, width, height) || chunk_crc(r, signature, 13) != r.be32(signature + 21))
		return {};

	std::uint64_t pos = signature, chunks = 0, images = 0;
	auto partial = [&](std::uint64_t at, std::string why) {
		if (images == 0)
			return t_verdict{}; // not one chunk of image data checks out
		t_verdict v{ .file = true, .recoverable = at, .integrity = Integrity::partial };
		v.reasons.push_back(std::format("{}x{}: {} chunks check out, {}. what's before that is exported", width, height, chunks, why));
		return v;
	};

	for (; chunks < max_chunks; chunks++) {
		const std::int64_t length = r.be32(pos);
		const int t[4] = { r.at(pos + 4), r.at(pos + 5), r.at(pos + 6), r.at(pos + 7) };
		if (length < 0 || t[3] < 0)
			return partial(pos, stopped(r, pos));
		if (length > 0x7FFFFFFF || !letter(t[0]) || !letter(t[1]) || !letter(t[2]) || !letter(t[3]))
			return partial(pos, std::format("the next chunk, at byte {} of it, isn't one: the file was fragmented or overwritten there", pos));
		const std::uint64_t ulength = static_cast<std::uint64_t>(length);
		const std::int64_t stored = r.be32(pos + 8 + ulength);
		const std::int64_t crc = stored < 0 ? -1 : chunk_crc(r, pos, ulength);
		if (crc < 0)
			return partial(pos, stopped(r, pos + 12 + ulength));
		const std::string type{ static_cast<char>(t[0]), static_cast<char>(t[1]), static_cast<char>(t[2]), static_cast<char>(t[3]) };
		if (crc != stored)
			return partial(pos, std::format("the {} chunk at byte {} of it doesn't match its CRC: the file was fragmented or overwritten there", type, pos));
		pos += 12 + ulength;
		images += type == "IDAT";
		if (type == "IEND") {
			t_verdict v{ .file = true, .recoverable = pos, .size = pos };
			if (images == 0)
				return {};
			v.integrity = Integrity::intact;
			v.reasons.push_back(std::format("{}x{}: all {} chunks match their CRCs, from the header to the end chunk, so it's in one piece", width, height, chunks + 1));
			return v;
		}
	}
	return partial(pos, "and then it goes on for more chunks than any real PNG has");
}
