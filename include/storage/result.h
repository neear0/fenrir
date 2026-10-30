#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

// what can go wrong, and a value-or-error for returning it. std::expected is c++23, so this is the project's own.
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
	};

	std::string_view to_string(Errc code) noexcept;

	struct Error {
		Errc code;
		const char* context = "";   // a string literal saying what was being done, never built at runtime
		std::uint32_t os_error = 0; // GetLastError() or errno when the os reported it, 0 otherwise
	};

	template <class T>
	class [[nodiscard]] Result {
	public:
		Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>) : state_(std::in_place_index<0>, std::move(value)) {}
		Result(Error error) noexcept : state_(std::in_place_index<1>, error) {}

		bool ok() const noexcept { return state_.index() == 0; }
		explicit operator bool() const noexcept { return ok(); }

		// value() on an error and error() on a value are bugs, std::get throws bad_variant_access for them
		T& value() & { return std::get<0>(state_); }
		const T& value() const& { return std::get<0>(state_); }
		T&& value() && { return std::get<0>(std::move(state_)); }
		const Error& error() const { return std::get<1>(state_); }

	private:
		std::variant<T, Error> state_;
	};

	template <>
	class [[nodiscard]] Result<void> {
	public:
		Result() noexcept = default;
		Result(Error error) noexcept : error_(error) {}

		bool ok() const noexcept { return !error_.has_value(); }
		explicit operator bool() const noexcept { return ok(); }
		const Error& error() const { return error_.value(); }

	private:
		std::optional<Error> error_;
	};
}
