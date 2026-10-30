#include "../../ui.h"
#include "screens.h"

// erase: no buttons on purpose. until the simulator (P7) there's nothing here that could erase anything, and the page says
// so instead of showing controls that look like they would

void fenrir::screens::erase() {
    using namespace celosia;
    render::groupbox::begin("Erase", "Not available in this build");
    widgets::text("There is no erasure code in this build, so nothing on this page can touch a drive.");
    widgets::text("It will show what a drive supports and plan a method for it, simulated first (P7),");
    widgets::text("then on disposable image files. Real drives come last, after an independent review (P8).");
    render::groupbox::end();

    render::groupbox::begin("What it will be honest about", "From NIST SP 800-88r2");
    widgets::text("Clear, purge and destroy are different levels, a method's level depends on the media.");
    widgets::text("Verified (the method completed) isn't validated (it's enough for the data's sensitivity).");
    widgets::text("Overwriting a file on an SSD isn't sanitization: wear leveling and spare blocks keep copies.");
    widgets::text("Cryptographic erase only counts when the data was encrypted from the start.");
    widgets::text("The system drive is never erased from a running Windows.");
    render::groupbox::end();
}
