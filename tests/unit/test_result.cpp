#include <doctest/doctest.h>

#include <memory>

#include "storage/result.h"

using storage::Errc;
using storage::Error;
using storage::Result;

TEST_CASE("a result holds a value or an error") {
	Result<int> value = 42;
	CHECK(value.ok());
	CHECK(static_cast<bool>(value));
	CHECK(value.value() == 42);

	Result<int> error = Error{ Errc::not_found, "looking" };
	CHECK_FALSE(error.ok());
	CHECK(error.error().code == Errc::not_found);
	CHECK(std::string_view(error.error().context) == "looking");
}

TEST_CASE("result<void> is ok unless it got an error") {
	Result<void> fine;
	CHECK(fine.ok());
	Result<void> failed = Error{ Errc::io_error, "reading", 5 };
	CHECK_FALSE(failed.ok());
	CHECK(failed.error().os_error == 5);
}

TEST_CASE("move-only values move out") {
	Result<std::unique_ptr<int>> result = std::make_unique<int>(7);
	REQUIRE(result.ok());
	std::unique_ptr<int> taken = std::move(result).value();
	CHECK(*taken == 7);
}

TEST_CASE("every error code has a name") {
	for (int code = 0; code <= static_cast<int>(Errc::out_of_memory); code++)
		CHECK(storage::to_string(static_cast<Errc>(code)) != "unknown error");
}
