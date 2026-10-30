#include "storage/result.h"

std::string_view storage::to_string(Errc code) noexcept {
	switch (code) {
	case Errc::invalid_argument: return "invalid argument";
	case Errc::not_found: return "not found";
	case Errc::access_denied: return "access denied";
	case Errc::busy: return "in use by another program";
	case Errc::unsupported: return "unsupported";
	case Errc::out_of_range: return "out of range";
	case Errc::overflow: return "offset overflow";
	case Errc::io_error: return "i/o error";
	case Errc::out_of_memory: return "out of memory";
	}
	return "unknown error";
}
