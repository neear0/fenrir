#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "storage/session.h"

// what fenrir's pages share: the recovery session (the engine, working on threads of its own) and the snapshot of it
// this frame draws. pages only read snapshots and ask the session to start or stop things, which returns at once: no
// page ever reads the image or writes a file on the render thread
namespace fenrir::app {
	inline constexpr const char* build_stage = "P4"; // the milestone this build is at, in the titlebar

	storage::RecoverySession& session();
	const storage::SessionSnapshot& now(); // the same snapshot for every page drawn in a frame
	void frame();                          // before the pages each frame: takes the snapshot, picks up what a dialog returned

	inline bool deep_scan = true;          // the next scan carves the image after reading its filesystems
	void open_image();                     // the image dialog, then a scan of what was picked
	void open_argument(int argc, char** argv); // a scan of the image on the command line, if there is one: "open with"

	// native file dialogs, each on a thread of its own so the window keeps drawing while one is open. the window is
	// disabled under it, like under any modal dialog. take() hands what was picked over once, to whoever asked
	namespace dialogs {
		enum class e_kind { image, folder };
		bool available();  // false where there's no dialog yet: the glfw build
		bool showing();
		void show(e_kind kind, int key);
		std::optional<std::filesystem::path> take(int key);
	}

	// text for drawing
	std::string utf8(const std::filesystem::path& path);
	std::string human_size(std::uint64_t bytes);   // 1.5 MiB
	std::string thousands(std::uint64_t value);    // 1,234,567
	std::string duration(double seconds);          // 1:05, 1:02:03
	std::string fit(const std::string& text, float width); // cut from the front with "..." until it fits: a path keeps its end
}
