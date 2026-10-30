#include "debug.h"

void celosia::debug::log([[maybe_unused]] std::string str) { // prints nothing in release builds
#ifdef _DEBUG
	std::cout << "\033[1;35m" << "[DEBUG] " << "\033[0m" << str << "\n";
#endif
}
