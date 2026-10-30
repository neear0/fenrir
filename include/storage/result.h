#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

// what can go wrong, and a value-or-error for returning it: std::expected with the project's Error.
// an Error only holds trivially copyable fields, so making one can't throw and noexcept paths can return them
namespace storage {
	enum class Errc : std::uint8_t {
		invalid_argument,
		not_found,
		access_denied,
		busy,         // another program has it open in a way that conflicts, e.g. for writing
		unsupported,  // e.g. device paths, which only a read-only device source will open
		out_of_range, // an offset or length past the end
		overflow,     // offset arithmetic didn't fit in 64 bits
		io_error,
		out_of_memory,
		cancelled,    // the job's CancellationToken was set
	};

	std::string_view to_string(Errc code) noexcept;

	struct Error {
		Errc code;
		const char* context = "";   // a string literal saying what was being done, never built at runtime
		std::uint32_t os_error = 0; // GetLastError() or errno when the os reported it, 0 otherwise
	};

	// value() on an error throws bad_expected_access, error() on a value is undefined: check first
	template <class T>
	using Result = std::expected<T, Error>;

	// return fail(Errc::io_error, "reading the header") from anything returning a Result
	constexpr std::unexpected<Error> fail(Errc code, const char* context, std::uint32_t os_error = 0) noexcept {
		return std::unexpected(Error{ code, context, os_error });
	}
}
