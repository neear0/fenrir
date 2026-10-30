#include "ui/ui.h"

int main() {
#ifdef _WIN32
    // dlls loaded at runtime only come from system32, never from next to the executable or the working directory
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    // windows doesn't stretch the window on scaled displays, platform::create() scales the ui to the monitor instead
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

    celosia::initialize::context();

    if (!celosia::platform::create("Celosia UI", ImVec2(0, 0), celosia::ui::size))
        return 1;
    celosia::initialize::fonts(); // at the scale create() picked for the monitor
    celosia::initialize::images();

    while (celosia::platform::poll())
    {
        celosia::ui::render();

        celosia::platform::bring_to_top();
    }

    // unload everything
    celosia::platform::destroy();
    ImGui::DestroyContext();
	return 0;
}