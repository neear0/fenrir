#include "../../ui.h"
#include "app.h"

namespace {
	enum e_dialog_key { image_to_scan = 1 };

	std::shared_ptr<const storage::SessionSnapshot> current;
	int taken_at = -1;
}

storage::RecoverySession& fenrir::app::session() {
	static storage::RecoverySession session; // its destructor stops the threads and waits for them, after main returns
	return session;
}

const storage::SessionSnapshot& fenrir::app::now() {
	if (!current)
		current = session().snapshot();
	return *current;
}

void fenrir::app::frame() {
	if (taken_at != celosia::core::frame()) {
		current = session().snapshot();
		taken_at = celosia::core::frame();
	}
	if (const auto image = dialogs::take(image_to_scan))
		session().scan(*image, { .carve = deep_scan });
}

void fenrir::app::open_image() {
	if (!session().busy())
		dialogs::show(dialogs::e_kind::image, image_to_scan);
}

std::string fenrir::app::utf8(const std::filesystem::path& path) {
	const std::u8string text = path.u8string();
	return { reinterpret_cast<const char*>(text.data()), text.size() };
}

std::string fenrir::app::human_size(std::uint64_t bytes) {
	const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
	double value = (double)bytes;
	int unit = 0;
	while (value >= 1024.0 && unit < 5) {
		value /= 1024.0;
		unit++;
	}
	char text[32];
	snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
	return text;
}

std::string fenrir::app::thousands(std::uint64_t value) {
	std::string digits = std::to_string(value), out;
	for (std::size_t i = 0; i < digits.size(); i++) {
		if (i > 0 && (digits.size() - i) % 3 == 0)
			out += ',';
		out += digits[i];
	}
	return out;
}

std::string fenrir::app::duration(double seconds) {
	const auto s = (std::uint64_t)(seconds < 0.0 ? 0.0 : seconds);
	char text[32];
	if (s >= 3600)
		snprintf(text, sizeof(text), "%llu:%02llu:%02llu", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
	else
		snprintf(text, sizeof(text), "%llu:%02llu", (unsigned long long)(s / 60), (unsigned long long)(s % 60));
	return text;
}

#ifdef _WIN32
#include <shellapi.h>
#endif

void fenrir::app::open_argument([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
#ifdef _WIN32
	int count = 0;
	LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count); // utf-16, argv is in the ansi code page
	if (!args)
		return;
	if (count >= 2)
		session().scan(std::filesystem::path(args[1]), { .carve = deep_scan });
	LocalFree(args);
#else
	if (argc >= 2)
		session().scan(std::filesystem::path(argv[1]), { .carve = deep_scan });
#endif
}

std::string fenrir::app::fit(const std::string& text, float width) {
	if (celosia::render::text::size(text.c_str()).x <= width)
		return text;
	std::size_t from = 0;
	std::string out;
	do {
		from++;
		while (from < text.size() && (static_cast<unsigned char>(text[from]) & 0xC0) == 0x80)
			from++; // whole utf-8 characters
		out = "..." + text.substr(from);
	} while (from < text.size() && celosia::render::text::size(out.c_str()).x > width);
	return out;
}
