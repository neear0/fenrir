#include "storage/hex_dump.h"

#include <format>

std::string storage::hex_dump(std::span<const std::byte> bytes, std::uint64_t base) {
	std::string text;
	text.reserve((bytes.size() + 15) / 16 * 86);
	for (std::size_t line = 0; line < bytes.size(); line += 16) {
		std::format_to(std::back_inserter(text), "{:016x} ", base + line);
		for (std::size_t i = 0; i < 16; i++) {
			if (i == 8)
				text += ' ';
			if (line + i < bytes.size())
				std::format_to(std::back_inserter(text), " {:02x}", std::to_integer<unsigned>(bytes[line + i]));
			else
				text += "   ";
		}
		text += "  ";
		for (std::size_t i = 0; i < 16 && line + i < bytes.size(); i++) {
			const auto c = std::to_integer<unsigned char>(bytes[line + i]);
			text += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.';
		}
		text += '\n';
	}
	return text;
}
