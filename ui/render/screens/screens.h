#pragma once

#include "icons.h"

// fenrir's pages and the shell around them. render::main_window() draws the shell and the open page, each page fills the
// space right of the sidebar with render::groupbox's. none of them reads or writes storage on the render thread: the
// recovery session (app.h) does that on its own threads, and nothing in this build can write to a drive
namespace fenrir::screens {
	void home();
	void recover();
	void images();
	void erase();
	void reports();
	void settings();

	struct t_page {
		const char* name;    // the tab, what ui::tab_active holds
		const char* caption; // under the name in the sidebar
		const char* section; // the heading it starts, nullptr to go on under the last one
		const char* badge;   // a pill after the name, nullptr for none
		icons::e_icon icon;
		bool bottom;         // kept at the bottom of the sidebar instead of in the list
		void (*draw)();
	};

	// the sidebar's order, ctrl+1 to ctrl+6 go to them in it
	inline constexpr t_page pages[] = {
		{ "Home", "Overview", nullptr, nullptr, icons::e_icon::home, false, home },
		{ "Recover", "Deleted and carved files", "Recovery", nullptr, icons::e_icon::recover, false, recover },
		{ "Images", "Read-only disk images", nullptr, nullptr, icons::e_icon::images, false, images },
		{ "Erase", "Not in this build", "Erasure", "P7", icons::e_icon::erase, false, erase },
		{ "Reports", "Local logs", "Records", nullptr, icons::e_icon::reports, false, reports },
		{ "Settings", "Look and feel", nullptr, nullptr, icons::e_icon::settings, true, settings },
	};
}

// the window around the pages: titlebar, sidebar and the window's buttons, see shell.cpp
namespace fenrir::shell {
	void titlebar();
	void sidebar();
	void window_buttons(); // after the page, so the keyboard reaches them last
}
