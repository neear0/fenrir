#include "storage/hash.h"

#include <algorithm>
#include <bit>
#include <memory>
#include <new>

namespace {
	constexpr std::array<std::uint32_t, 256> crc_table = [] {
		std::array<std::uint32_t, 256> table{};
		for (std::uint32_t i = 0; i < 256; i++) {
			std::uint32_t c = i;
			for (int bit = 0; bit < 8; bit++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		return table;
	}();

	// fips 180-4 section 4.2.2 and 5.3.3
	constexpr std::array<std::uint32_t, 64> k = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};
	constexpr std::array<std::uint32_t, 8> initial = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
}

std::uint32_t storage::crc32(std::span<const std::byte> bytes, std::uint32_t crc) noexcept {
	crc = ~crc;
	for (std::byte b : bytes)
		crc = crc_table[(crc ^ std::to_integer<std::uint32_t>(b)) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

storage::Sha256::Sha256() noexcept : state_(initial) {}

void storage::Sha256::block(const std::byte* data) noexcept {
	std::array<std::uint32_t, 64> w{};
	for (int i = 0; i < 16; i++)
		w[i] = std::to_integer<std::uint32_t>(data[i * 4]) << 24 | std::to_integer<std::uint32_t>(data[i * 4 + 1]) << 16
			| std::to_integer<std::uint32_t>(data[i * 4 + 2]) << 8 | std::to_integer<std::uint32_t>(data[i * 4 + 3]);
	for (int i = 16; i < 64; i++) {
		const std::uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const std::uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	auto [a, b, c, d, e, f, g, h] = state_;
	for (int i = 0; i < 64; i++) {
		const std::uint32_t t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
		const std::uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	const std::array<std::uint32_t, 8> add = { a, b, c, d, e, f, g, h };
	for (int i = 0; i < 8; i++)
		state_[i] += add[i];
}

void storage::Sha256::update(std::span<const std::byte> bytes) noexcept {
	length_ += bytes.size();
	if (buffered_ > 0) {
		const std::size_t take = std::min(bytes.size(), buffer_.size() - buffered_);
		std::copy_n(bytes.begin(), take, buffer_.begin() + buffered_);
		buffered_ += take;
		bytes = bytes.subspan(take);
		if (buffered_ < buffer_.size())
			return;
		block(buffer_.data());
		buffered_ = 0;
	}
	while (bytes.size() >= 64) {
		block(bytes.data());
		bytes = bytes.subspan(64);
	}
	std::copy(bytes.begin(), bytes.end(), buffer_.begin());
	buffered_ = bytes.size();
}

storage::Sha256::Digest storage::Sha256::finish() noexcept {
	const std::uint64_t bits = length_ * 8;
	// a 1 bit, zeros up to 56 bytes into a block, then the length in bits, big-endian
	std::array<std::byte, 72> padding{};
	padding[0] = std::byte{ 0x80 };
	const std::size_t zeros = (buffered_ < 56 ? 56 : 120) - buffered_;
	for (int i = 0; i < 8; i++)
		padding[zeros + i] = static_cast<std::byte>(bits >> (56 - 8 * i));
	update(std::span(padding).first(zeros + 8));

	Digest digest{};
	for (int i = 0; i < 8; i++)
		for (int j = 0; j < 4; j++)
			digest[i * 4 + j] = static_cast<std::byte>(state_[i] >> (24 - 8 * j));
	*this = Sha256();
	return digest;
}

storage::Sha256::Digest storage::Sha256::of(std::span<const std::byte> bytes) noexcept {
	Sha256 hasher;
	hasher.update(bytes);
	return hasher.finish();
}

std::string storage::to_hex(std::span<const std::byte> bytes) {
	constexpr char digits[] = "0123456789abcdef";
	std::string text;
	text.reserve(bytes.size() * 2);
	for (std::byte b : bytes) {
		text += digits[std::to_integer<int>(b) >> 4];
		text += digits[std::to_integer<int>(b) & 0xF];
	}
	return text;
}

storage::Result<storage::Sha256::Digest> storage::sha256_source(IBlockSource& source, const CancellationToken* cancel, ProgressSink* progress) noexcept {
	constexpr std::size_t piece = std::size_t(1) << 20;
	std::unique_ptr<std::byte[]> buffer(new (std::nothrow) std::byte[piece]);
	if (!buffer)
		return fail(Errc::out_of_memory, "hashing the source");

	const std::uint64_t size = source.size_bytes();
	Sha256 hasher;
	for (std::uint64_t done = 0; done < size;) {
		if (cancel && cancel->cancelled())
			return fail(Errc::cancelled, "hashing the source");
		const std::span<std::byte> part(buffer.get(), static_cast<std::size_t>(std::min<std::uint64_t>(piece, size - done)));
		if (Result<void> read = read_exact(source, done, part); !read)
			return std::unexpected(read.error());
		hasher.update(part);
		done += part.size();
		if (progress)
			progress->progress(done, size);
	}
	return hasher.finish();
}
