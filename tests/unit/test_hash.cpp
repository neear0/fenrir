#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "storage/fault_injecting_source.h"
#include "storage/hash.h"
#include "storage/memory_source.h"

using storage::Errc;

namespace {
	std::span<const std::byte> as_bytes(std::string_view text) { return std::as_bytes(std::span(text)); }
	std::string sha256(std::string_view text) { return storage::to_hex(storage::Sha256::of(as_bytes(text))); }

	std::vector<std::byte> pattern(std::size_t size) {
		std::vector<std::byte> bytes(size);
		for (std::size_t i = 0; i < size; i++)
			bytes[i] = static_cast<std::byte>((i * 7919 + i / 251) & 0xFF);
		return bytes;
	}

	struct t_progress final : storage::ProgressSink {
		std::uint64_t calls = 0, last_done = 0, last_total = 0;
		bool went_down = false;
		void progress(std::uint64_t done, std::uint64_t total) noexcept override {
			went_down |= done < last_done;
			calls++;
			last_done = done;
			last_total = total;
		}
	};
}

TEST_CASE("crc-32 matches the standard check value") {
	CHECK(storage::crc32(as_bytes("123456789")) == 0xCBF43926u);
	CHECK(storage::crc32({}) == 0u);
	// in two pieces, the first one's crc passed back in
	CHECK(storage::crc32(as_bytes("6789"), storage::crc32(as_bytes("12345"))) == 0xCBF43926u);
}

TEST_CASE("sha-256 matches the fips 180-4 examples") {
	CHECK(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(sha256(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha-256 gives the same digest however the input is split") {
	const auto bytes = pattern(300);
	const auto whole = storage::Sha256::of(bytes);
	for (std::size_t split = 0; split <= bytes.size(); split++) {
		CAPTURE(split);
		storage::Sha256 hasher;
		hasher.update(std::span(bytes).first(split));
		hasher.update(std::span(bytes).subspan(split));
		REQUIRE(hasher.finish() == whole);
	}
	// byte by byte, over every padding length from 55 to 64
	for (std::size_t length = 50; length <= 70; length++) {
		CAPTURE(length);
		storage::Sha256 hasher;
		for (std::size_t i = 0; i < length; i++)
			hasher.update(std::span(bytes).subspan(i, 1));
		REQUIRE(hasher.finish() == storage::Sha256::of(std::span(bytes).first(length)));
	}
}

TEST_CASE("a hasher starts over after finish") {
	storage::Sha256 hasher;
	hasher.update(as_bytes("something else"));
	(void)hasher.finish();
	hasher.update(as_bytes("abc"));
	CHECK(storage::to_hex(hasher.finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("hashing a source reads all of it, even in short reads") {
	const auto bytes = pattern(3 * (1 << 20) + 12345); // more than one 1 MiB piece, and not a whole number of them
	storage::MemorySource memory(bytes);
	storage::FaultInjectingSource flaky(memory);
	flaky.set_max_read(4093);
	t_progress progress;

	auto digest = storage::sha256_source(flaky, nullptr, &progress);
	REQUIRE(digest.has_value());
	CHECK(*digest == storage::Sha256::of(bytes));
	CHECK(progress.calls == 4);
	CHECK(progress.last_done == bytes.size());
	CHECK(progress.last_total == bytes.size());
	CHECK_FALSE(progress.went_down);
}

TEST_CASE("an empty source hashes to the empty digest") {
	storage::MemorySource empty({});
	auto digest = storage::sha256_source(empty);
	REQUIRE(digest.has_value());
	CHECK(storage::to_hex(*digest) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("an unreadable region fails the hash instead of skipping it") {
	storage::MemorySource memory(pattern(2 << 20));
	storage::FaultInjectingSource bad(memory);
	bad.add_bad_region((1 << 20) + 4096, 512);
	auto digest = storage::sha256_source(bad);
	REQUIRE_FALSE(digest.has_value());
	CHECK(digest.error().code == Errc::io_error);
}

TEST_CASE("a cancelled hash stops with cancelled") {
	storage::MemorySource memory(pattern(4 << 20));
	storage::CancellationToken cancel;

	struct t_cancel_after_first final : storage::ProgressSink {
		storage::CancellationToken& token;
		explicit t_cancel_after_first(storage::CancellationToken& t) : token(t) {}
		void progress(std::uint64_t, std::uint64_t) noexcept override { token.cancel(); }
	} cancel_soon(cancel);

	storage::FaultInjectingSource counting(memory);
	auto digest = storage::sha256_source(counting, &cancel, &cancel_soon);
	REQUIRE_FALSE(digest.has_value());
	CHECK(digest.error().code == Errc::cancelled);
	CHECK(counting.reads() == 1); // the first piece, then it stopped
}
