#include "../../ui.h"
#include "screens.h"

// reports: local json and text logs of what an operation did, once there are operations to report on

void fenrir::screens::reports() {
    using namespace celosia;
    render::groupbox::begin("Reports", "Kept on this computer only", render::groupbox::e_group_layout::fullscreen);
    widgets::text("No reports yet: nothing in this build scans, exports or erases.");
    widgets::text("Each operation will leave a local report: the exact source and target, the bytes read,");
    widgets::text("the method, the errors, what was verified and what is still unknown.");
    render::groupbox::end();
}
