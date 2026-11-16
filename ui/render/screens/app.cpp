#include "../../ui.h"
#include "app.h"

#include <mutex>
#include <thread>

namespace {
	enum e_dialog_key { image_to_scan = 1, image_to_copy, copy_target };

	std::shared_ptr<const storage::SessionSnapshot> current;
	std::shared_ptr<const storage::ImagingSnapshot> imaging_current;
	int taken_at = -1;
}

storage::RecoverySession& fenrir::app::session() {
	static storage::RecoverySession session; // its destructor stops the threads and waits for them, after main returns
	return session;
}

storage::ImagingSession& fenrir::app::imaging() {
	static storage::ImagingSession imaging; // its destructor stops the copy, with its map saved, after main returns
	return imaging;
}

const storage::ImagingSnapshot& fenrir::app::imaging_now() {
	if (!imaging_current)
		imaging_current = imaging().snapshot();
	return *imaging_current;
}

const storage::SessionSnapshot& fenrir::app::now() {
	if (!current)
		current = session().snapshot();
	return *current;
}

void fenrir::app::frame() {
	if (taken_at != celosia::core::frame()) {
		current = session().snapshot();
		imaging_current = imaging().snapshot();
		taken_at = celosia::core::frame();
	}
	if (const auto image = dialogs::take(image_to_scan))
		session().scan(*image, { .carve = deep_scan });
	if (auto source = dialogs::take(image_to_copy))
		imaging_source = std::move(source);
	if (auto target = dialogs::take(copy_target))
		imaging_target = std::move(target);
}

void fenrir::app::open_image() {
	if (!session().busy())
		dialogs::show(dialogs::e_kind::image, image_to_scan);
}

void fenrir::app::pick_imaging_source() {
	if (!imaging().busy())
		dialogs::show(dialogs::e_kind::image, image_to_copy);
}

void fenrir::app::pick_imaging_target() {
	if (!imaging().busy())
		dialogs::show(dialogs::e_kind::new_image, copy_target);
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

namespace {
	struct t_drives {
		std::mutex mutex;
		std::vector<storage::DiskInfo> list;
		bool listing = false;
	};
	std::shared_ptr<t_drives> drive_state = std::make_shared<t_drives>();
	std::vector<storage::DiskInfo> drives_shown; // the render thread's copy
}

void fenrir::app::drives::refresh() {
	{
		std::lock_guard lock(drive_state->mutex);
		if (drive_state->listing)
			return;
		drive_state->listing = true;
	}
	std::thread([state = drive_state] {
		auto listed = storage::list_disks();
		std::lock_guard lock(state->mutex);
		state->list = listed ? std::move(*listed) : std::vector<storage::DiskInfo>{};
		state->listing = false;
	}).detach(); // the state is shared with it, nothing it touches goes away before it's done
}

bool fenrir::app::drives::listing() {
	std::lock_guard lock(drive_state->mutex);
	return drive_state->listing;
}

const std::vector<storage::DiskInfo>& fenrir::app::drives::list() {
	std::lock_guard lock(drive_state->mutex);
	if (!drive_state->listing)
		drives_shown = drive_state->list;
	return drives_shown;
}
