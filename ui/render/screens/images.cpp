#include "../../ui.h"
#include "screens.h"

// images: picking and opening disk images. the engine's reader exists (storage::FileImageSource), the picker and the
// worker that opens it off the render thread come with P4

void fenrir::screens::images() {
    using namespace celosia;
    render::groupbox::begin("Disk images", "Raw images (.img, .dd, .bin), opened read-only", render::groupbox::e_group_layout::fullscreen);
    widgets::text("Opening images from this window comes with milestone P4.");
    widgets::text("Until then the command line does it:");
    widgets::text("    fenrir-cli info <image>");
    widgets::text("Images are opened read-only, and on Windows nothing else can write to them while they're open.");
    widgets::text("Drives and volumes aren't opened as images. Imaging a drive is milestone P6.");
    render::groupbox::end();
}
