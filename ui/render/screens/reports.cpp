#include "../../ui.h"
#include "app.h"
#include "screens.h"

// reports: what the last export wrote, and where its report is. the report itself is a text file next to the
// exported files, written by the exporter: every file, its status, its SHA-256 and why it has that status

using namespace celosia;
namespace app = fenrir::app;

void fenrir::screens::reports() {
    const storage::SessionSnapshot& s = app::now();
    render::groupbox::begin("Reports", "Kept on this computer only", render::groupbox::e_group_layout::fullscreen);
    if (s.destination.empty()) {
        widgets::text("No exports yet. Each export leaves a report next to the files it wrote: the image it read,");
        widgets::text("every file with its status, its SHA-256 and the reasons for its status.");
        widgets::text("Nothing is uploaded anywhere, and nothing is logged about the files apart from that report.");
        render::groupbox::end();
        return;
    }

    std::size_t written = 0, failed = 0, partial = 0;
    std::uint64_t bytes = 0;
    for (const storage::ExportRecord& r : *s.exported) {
        if (!r.error.empty()) {
            failed++;
            continue;
        }
        written++;
        bytes += r.bytes;
        partial += !r.complete;
    }
    const float width = core::available().x - style::frame::size_padding.x;
    const std::string to = "Last export: " + app::fit(app::utf8(s.destination), width - render::text::size("Last export: ").x);
    const std::string counts = app::thousands(written) + " written (" + app::human_size(bytes) + "), " + app::thousands(partial) + " of them partial, " +
        app::thousands(failed) + " couldn't be";
    const std::string report = s.report.empty() ? "The report wasn't written: " + s.status : "Report: " + app::fit(app::utf8(s.report), width - render::text::size("Report: ").x);
    widgets::text(to.c_str());
    widgets::text(counts.c_str());
    widgets::text(report.c_str());
    core::layout(ImVec2(0.f, core::font_size() * 0.5f));

    // what couldn't be written, and why
    if (failed != 0) {
        widgets::text("Not written:");
        for (const storage::ExportRecord& r : *s.exported)
            if (!r.error.empty() && r.id < s.files->size()) {
                const std::string line = (*s.files)[(std::size_t)r.id].file.path + ": " + r.error;
                widgets::text(line.c_str());
            }
    }
    render::groupbox::end();
}
