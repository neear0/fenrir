#pragma once

// fenrir's pages, one per sidebar tab. render::main_window() draws the open one next to the sidebar, each fills the space
// with render::groupbox's. none of them reads or writes storage on the render thread: the recovery list is mock data
// until the scans run on workers (milestone P4), and nothing in this build can write to a drive
namespace fenrir::screens {
	inline constexpr const char* tabs[] = { "Home", "Recover", "Images", "Erase", "Reports", "Settings" };

	void home();
	void recover();
	void images();
	void erase();
	void reports();
	void settings();
}
