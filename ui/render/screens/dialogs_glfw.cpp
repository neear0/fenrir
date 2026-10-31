#include "../../ui.h"
#include "app.h"

// no file dialogs on the glfw build yet: CelosUI has no text field to type a path into, and linux has no one dialog
// api (portals over d-bus, gtk, zenity). the pages say so where they'd offer one

bool fenrir::app::dialogs::available() { return false; }
bool fenrir::app::dialogs::showing() { return false; }
void fenrir::app::dialogs::show(e_kind, int) {}
std::optional<std::filesystem::path> fenrir::app::dialogs::take(int) { return std::nullopt; }
