#include <doctest/doctest.h>

#include <memory>

#include "storage/result.h"

using storage::Errc;
using storage::Result;

TEST_CASE("a result holds a value or an error") {
	Result<int> value = 42;
	CHECK(value.has_value());
	CHECK(static_cast<bool>(value));
	CHECK(value.value() == 42);

	Result<int> error = storage::fail(Errc::not_found, "looking");
	CHECK_FALSE(error.has_value());
	CHECK(error.error().code == Errc::not_found);
	CHECK(std::string_view(error.error().context) == "looking");
	CHECK(error.error().os_error == 0);
}

TEST_CASE("result<void> has a value unless it got an error") {
	Result<void> fine;
	CHECK(fine.has_value());
	Result<void> failed = storage::fail(Errc::io_error, "reading", 5);
	CHECK_FALSE(failed.has_value());
	CHECK(failed.error().os_error == 5);
}

TEST_CASE("move-only values move out") {
	Result<std::unique_ptr<int>> result = std::make_unique<int>(7);
	REQUIRE(result.has_value());
	std::unique_ptr<int> taken = std::move(result).value();
	CHECK(*taken == 7);
}

TEST_CASE("errors chain through and_then and transform") {
	auto half = [](int v) -> Result<int> {
		if (v % 2)
			return storage::fail(Errc::invalid_argument, "halving an odd number");
		return v / 2;
	};
	CHECK(Result<int>(8).and_then(half).and_then(half).value() == 2);
	auto odd = Result<int>(6).and_then(half).and_then(half);
	REQUIRE_FALSE(odd.has_value());
	CHECK(odd.error().code == Errc::invalid_argument);
	CHECK(Result<int>(3).transform([](int v) { return v * 2; }).value() == 6);
}

TEST_CASE("every error code has a name") {
	for (int code = 0; code <= static_cast<int>(Errc::cancelled); code++)
		CHECK(storage::to_string(static_cast<Errc>(code)) != "unknown error");
}
