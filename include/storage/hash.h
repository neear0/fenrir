#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "block_source.h"
#include "jobs.h"

namespace storage {
	// crc-32 as gpt uses it (ieee 802.3, reflected, 0xEDB88320). continue a running crc by passing the last one back in
	std::uint32_t crc32(std::span<const std::byte> bytes, std::uint32_t crc = 0) noexcept;

	// sha-256 (fips 180-4), fed in pieces of any size
	class Sha256 {
	public:
		using Digest = std::array<std::byte, 32>;

		Sha256() noexcept;
		void update(std::span<const std::byte> bytes) noexcept;
		Digest finish() noexcept; // the hasher starts over afterwards

		static Digest of(std::span<const std::byte> bytes) noexcept;

	private:
		void block(const std::byte* data) noexcept;

		std::array<std::uint32_t, 8> state_{};
		std::array<std::byte, 64> buffer_{};
		std::size_t buffered_ = 0;
		std::uint64_t length_ = 0; // bytes hashed so far
	};

	// lowercase hex, the way sha256sum and Get-FileHash -Algorithm SHA256 (uppercased) print it
	std::string to_hex(std::span<const std::byte> bytes);

	// sha-256 of everything in source, read in 1 MiB pieces. cancel is checked and progress is told after each piece,
	// both can be null. any read error stops it: a hash over a source with unreadable parts would mean nothing
	Result<Sha256::Digest> sha256_source(IBlockSource& source, const CancellationToken* cancel = nullptr, ProgressSink* progress = nullptr) noexcept;
}
