#include "../../ui.h"
#include "app.h"
#include "screens.h"

// recover: open an image and scan it, look through what was found and why each file has its status, export some.
// everything that touches the image runs in the session (app.h), this page draws its snapshots and starts and stops
// its jobs. the 100,000 made up files from P0 are still one click away, to try the list at a size scans rarely reach

using namespace celosia;
namespace app = fenrir::app;
using storage::FoundFile;
using storage::Integrity;
using storage::Origin;

namespace {
    enum e_dialog_key { dialog_destination = 2 };
    using t_files = std::vector<FoundFile>;

    float px(float v) { return v * style::scale; }
    ImColor dim(float alpha = 0.6f) { ImVec4 c = style::themes::active.text.Value; c.w *= alpha; return ImColor(c); }

    const char* status_name(Integrity integrity) {
        static const char* names[] = { "Intact", "Partial", "Uncertain", "Unrecoverable" };
        return names[(int)integrity];
    }

    ImColor status_color(Integrity integrity) { // readable against the panels, whichever theme is on
        static const ImColor colors[] = { { 110, 200, 90, 255 }, { 255, 176, 64, 255 }, { 150, 165, 200, 255 }, { 255, 96, 96, 255 } };
        return style::themes::readable(colors[(int)integrity], style::themes::active.background_darker, style::limits::accent_contrast_min);
    }

    // made up files in the shape a scan gives them, generated once and the same every run
    std::shared_ptr<const t_files> mock_files() {
        static const std::shared_ptr<const t_files> list = [] {
            auto out = std::make_shared<t_files>();
            out->reserve(100000);
            std::uint32_t state = 0x9E3779B9u; // xorshift
            const auto next = [&state] { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
            const char* named[] = { "/DCIM/IMG_%04u.JPG", "/DCIM/DSC%05u.JPG", "/Screenshot_%u.png", "/Documents/invoice_%u.pdf", "/notes_%u.txt", "/backup_%u.zip" };
            const char* formats[] = { "JPEG", "PNG", "PDF", "ZIP" }, * extensions[] = { "jpg", "png", "pdf", "zip" };
            char text[64];
            for (std::uint32_t i = 0; i < 100000; i++) {
                FoundFile f;
                storage::RecoveryCandidate& c = f.file;
                c.id = i;
                c.size = 200 + (std::uint64_t)(next() % 6000000u);
                const std::uint32_t roll = next() % 100;
                c.integrity = roll < 55 ? Integrity::intact : roll < 75 ? Integrity::partial : roll < 93 ? Integrity::uncertain : Integrity::unrecoverable;
                c.recoverable_bytes = c.integrity == Integrity::partial ? c.size / 2 : c.integrity == Integrity::unrecoverable ? 0 : c.size;
                if (next() % 3 == 0) {
                    const std::uint32_t kind = next() % 4;
                    const std::uint64_t at = (std::uint64_t)(next() % 4000000u) * 512u;
                    snprintf(text, sizeof(text), "%010llx.%s", (unsigned long long)at, extensions[kind]);
                    c.origin = Origin::carved;
                    c.format = formats[kind];
                    c.name = text;
                    c.path = "/carved/" + c.name;
                    c.entry_offset = at;
                } else {
                    snprintf(text, sizeof(text), named[next() % 6], next() % 10000u);
                    c.path = text;
                    c.name = c.path.substr(c.path.rfind('/') + 1);
                    c.deleted = true;
                    c.first_cluster = 2 + next() % 900000u;
                    f.volume = 1;
                }
                c.reasons = { "made up, like everything else in this list" };
                out->push_back(std::move(f));
            }
            return out;
        }();
        return list;
    }

    bool mock = false;

    // the rows shown: indexes into the files that pass the filters, rebuilt when either changes
    int status_filter = 0; // all, then one per Integrity
    int origin_filter = 0; // deleted and carved, deleted, carved, everything
    std::shared_ptr<const t_files> built_files;
    int built_status = -1, built_origin = -1;
    std::vector<std::uint32_t> shown;
    int selected = -1;
    std::uint64_t selected_id = UINT64_MAX, previewed = UINT64_MAX;

    std::optional<std::filesystem::path> destination;

    bool passes(const FoundFile& f) {
        const storage::RecoveryCandidate& c = f.file;
        if (status_filter != 0 && (int)c.integrity != status_filter - 1)
            return false;
        switch (origin_filter) {
        case 0: return c.deleted || c.origin == Origin::carved;
        case 1: return c.deleted;
        case 2: return c.origin == Origin::carved;
        default: return true;
        }
    }

    void rebuild(const std::shared_ptr<const t_files>& files) {
        if (files == built_files && status_filter == built_status && origin_filter == built_origin)
            return;
        shown.clear();
        for (std::uint32_t i = 0; i < (std::uint32_t)files->size(); i++)
            if (passes((*files)[i]))
                shown.push_back(i);
        built_files = files;
        built_status = status_filter;
        built_origin = origin_filter;
        // the same file stays picked: ids only grow while a scan adds to the list
        selected = -1;
        const auto at = std::ranges::lower_bound(shown, selected_id, {}, [&](std::uint32_t i) { return (*files)[i].file.id; });
        if (at != shown.end() && (*files)[*at].file.id == selected_id)
            selected = (int)(at - shown.begin());
    }

    std::string found_by(const FoundFile& f, const storage::SessionSnapshot& s) {
        if (f.file.origin == Origin::carved)
            return "Carved: " + f.file.format;
        const std::string where = mock ? "partition 1" : f.volume < s.volumes.size() ? s.volumes[f.volume].name : "?";
        const std::string kind = mock || f.volume >= s.volumes.size() ? "FAT32" : std::string(storage::to_string(s.volumes[f.volume].filesystem));
        return kind + (f.file.deleted ? ", deleted, " : ", ") + where;
    }

    // the words of text in lines no wider than width
    std::vector<std::string> wrap(const std::string& text, float width) {
        std::vector<std::string> lines;
        std::string line;
        std::size_t at = 0;
        while (at < text.size()) {
            std::size_t end = text.find(' ', at);
            if (end == std::string::npos)
                end = text.size();
            const std::string word = text.substr(at, end - at);
            const std::string longer = line.empty() ? word : line + " " + word;
            if (!line.empty() && render::text::size(longer.c_str()).x > width) {
                lines.push_back(line);
                line = word;
            } else {
                line = longer;
            }
            at = end + 1;
        }
        if (!line.empty())
            lines.push_back(line);
        return lines;
    }

    // the columns' share of a row: name, size, found by, status
    constexpr float columns[] = { 0.44f, 0.12f, 0.26f, 0.18f };
    ImRect cell(const ImRect& row, int column) {
        float x = row.Min.x;
        for (int i = 0; i < column; i++)
            x += row.GetWidth() * columns[i];
        const float pad = style::frame::size_padding.x;
        return ImRect(ImVec2(x + pad, row.Min.y), ImVec2(x + row.GetWidth() * columns[column] - pad, row.Max.y));
    }

    // a bar for the running job: how far along, and what it is
    void progress(const storage::SessionSnapshot& s, float right_gap) {
        const bool counting_bytes = s.phase != storage::SessionPhase::filesystems;
        std::string right;
        if (s.total != 0)
            right = counting_bytes ? app::human_size(s.done) + " of " + app::human_size(s.total) : std::to_string(s.done) + " of " + std::to_string(s.total);
        right += (right.empty() ? "" : "   ") + app::duration(s.seconds);

        const ImVec2 pos = core::cursor();
        const float height = core::font_size() + px(10.f);
        const ImRect bb(pos, pos + ImVec2(core::available().x + right_gap, height));
        core::layout(bb.GetSize(), 0.f);
        if (!core::add(bb, 0))
            return;
        ImDrawList* drawlist = core::drawlist();
        const float round = px(6.f);
        drawlist->AddRectFilled(bb.Min, bb.Max, dim(0.07f), round);
        const float fraction = s.total == 0 ? 0.f : ImClamp((float)((double)s.done / (double)s.total), 0.f, 1.f);
        if (fraction > 0.f) {
            ImVec4 a = style::general::accent.Value;
            a.w = 0.35f;
            drawlist->AddRectFilled(bb.Min, ImVec2(bb.Min.x + bb.GetWidth() * fraction, bb.Max.y), ImColor(a), round);
        }
        const ImVec2 pad(px(10.f), 0.f);
        const float right_width = render::text::size(right.c_str()).x;
        const std::string status = app::fit(s.status, bb.GetWidth() - pad.x * 2.f - right_width - px(16.f));
        render::text::draw_aligned(drawlist, bb.Min + pad, bb.Max - pad, ImVec2(0.f, 0.5f), style::themes::active.text, status.c_str());
        render::text::draw_aligned(drawlist, bb.Min + pad, bb.Max - pad, ImVec2(1.f, 0.5f), dim(0.8f), right.c_str());
    }

    // the picked file: what it is and why it has its status on the left, its first bytes on the right
    void details(const FoundFile& f, const storage::SessionSnapshot& s, float height, float right_gap) {
        const ImVec2 pos = core::cursor();
        const ImRect bb(pos, pos + ImVec2(core::available().x + right_gap, height));
        core::layout(bb.GetSize(), 0.f);
        if (!core::add(bb, 0))
            return;
        ImDrawList* drawlist = core::drawlist();
        const storage::RecoveryCandidate& c = f.file;
        const float line = core::font_size() + px(3.f), label_width = px(78.f);
        const float preview_width = px(300.f), left_width = bb.GetWidth() - preview_width - px(16.f);
        const ImRect left(bb.Min, ImVec2(bb.Min.x + left_width, bb.Max.y));
        float y = bb.Min.y;
        auto field = [&](const char* label, const std::string& value, ImColor color) {
            if (y + line > bb.Max.y)
                return;
            render::text::draw(drawlist, ImVec2(left.Min.x, y), dim(), label);
            const ImRect clip(ImVec2(left.Min.x + label_width, y), ImVec2(left.Max.x, y + line));
            render::text::draw_aligned(drawlist, clip.Min, clip.Max, ImVec2(0.f, 0.f), color, value.c_str(), nullptr, nullptr, &clip);
            y += line;
        };
        const ImColor text = style::themes::active.text;
        field("Name", c.origin == Origin::carved ? c.name + "   (carved files have no name, this is where it starts)" : c.path, text);
        std::string where;
        if (c.origin == Origin::carved)
            where = "byte " + app::thousands(c.entry_offset) + " of the image";
        else if (!c.directory)
            where = "cluster " + app::thousands(c.first_cluster) + ", its directory entry at byte " + app::thousands(c.entry_offset) + " of the volume";
        field("Found", found_by(f, s) + (where.empty() ? "" : ", " + where), text);
        std::string size = c.directory ? "a folder" : app::human_size(c.size) + " (" + app::thousands(c.size) + " bytes)";
        if (!c.directory && c.recoverable_bytes != c.size)
            size += ", " + app::thousands(c.recoverable_bytes) + " of them recoverable";
        field("Size", size, text);
        if (c.modified.valid) {
            char date[96];
            snprintf(date, sizeof(date), "%04u-%02u-%02u %02u:%02u:%02u, %s", c.modified.year, c.modified.month, c.modified.day,
                c.modified.hour, c.modified.minute, c.modified.second, c.modified.utc ? "UTC" : "local time of the computer that wrote it");
            field("Modified", date, text);
        }
        field("Status", status_name(c.integrity), status_color(c.integrity));
        // why: every reason, wrapped, as far as there's room
        bool first = true;
        for (const std::string& reason : c.reasons)
            for (const std::string& part : wrap(reason, left_width - label_width)) {
                field(first ? "Why" : "", part, dim(0.85f));
                first = false;
            }

        // the first bytes, 8 to a row, as hex and as text
        const ImVec2 right(bb.Max.x - preview_width, bb.Min.y);
        render::text::draw(drawlist, right, dim(), "First bytes");
        if (mock) {
            render::text::draw(drawlist, right + ImVec2(0.f, line), dim(0.5f), "none, it's made up");
            return;
        }
        const storage::FilePreview p = app::session().preview_result();
        if (p.id != c.id || !p.ready) {
            render::text::draw(drawlist, right + ImVec2(0.f, line), dim(0.5f), "reading...");
            return;
        }
        if (!p.error.empty()) {
            render::text::draw(drawlist, right + ImVec2(0.f, line), dim(0.5f), p.error.c_str());
            return;
        }
        ImFont* font_small = resources::fonts::get("small");
        const float cell_w = px(23.f), text_x = right.x + cell_w * 8.f + px(8.f), char_w = px(8.5f);
        const int rows = (int)ImMin((float)((p.bytes.size() + 7) / 8), (bb.Max.y - right.y - line) / line);
        for (int r = 0; r < rows; r++) {
            const float ry = right.y + line * (float)(r + 1);
            for (int i = 0; i < 8 && (std::size_t)(r * 8 + i) < p.bytes.size(); i++) {
                const unsigned b = std::to_integer<unsigned>(p.bytes[(std::size_t)(r * 8 + i)]);
                char hex[4], ch[2] = { b >= 0x20 && b < 0x7F ? (char)b : '.', 0 };
                snprintf(hex, sizeof(hex), "%02X", b);
                render::text::draw(drawlist, ImVec2(right.x + cell_w * (float)i, ry), b == 0 ? dim(0.35f) : text, hex, nullptr, font_small);
                render::text::draw(drawlist, ImVec2(text_x + char_w * (float)i, ry), dim(0.7f), ch, nullptr, font_small);
            }
        }
    }

    // nothing open yet: what the page does, and the ways in
    void empty_state() {
        widgets::text("Open a disk image to look for deleted files in it. It's only ever read, never written to.");
        widgets::text("The scan reads the partition table and every FAT32 and exFAT volume's deleted files, then (the deep scan)");
        widgets::text("looks through the whole image for JPEG, PNG, PDF and ZIP files by their content.");
        core::layout(ImVec2(0.f, core::font_size() * 0.5f));
        if (app::dialogs::available()) {
            if (widgets::button("Open an image..."))
                app::open_image();
            core::same_line();
        }
        widgets::toggle("Deep scan", &app::deep_scan);
        if (!app::dialogs::available())
            widgets::text("This build has no file dialog yet (the GLFW one): use fenrir-cli files / carve / recover.");
        core::layout(ImVec2(0.f, core::font_size()));
        widgets::text("Or try the list on 100,000 made up files, to see how it handles a big scan:");
        if (widgets::button("Show made up files"))
            mock = true;
    }
}

void fenrir::screens::recover() {
    const storage::SessionSnapshot& s = app::now();
    const bool busy = s.phase != storage::SessionPhase::idle;
    if (const auto picked = app::dialogs::take(dialog_destination))
        destination = picked;

    std::string description = mock ? "Made up files, nothing was scanned" : s.open || busy ? app::utf8(s.image) : "No image open";
    render::groupbox::begin("Recover", description.c_str(), render::groupbox::e_group_layout::fullscreen);
    const float right_gap = -style::frame::size_padding.x; // the same room on the right as the groupbox has on the left
    if (!mock && !s.open && !busy) {
        if (s.error)
            widgets::text(s.status.c_str());
        empty_state();
        render::groupbox::end();
        return;
    }

    // the source and what's running
    if (mock) {
        if (widgets::button("Back to the scan"))
            mock = false;
        core::same_line();
        widgets::text("100,000 made up files: the list, the filters and the keyboard work, there's nothing to export.");
    } else {
        if (!busy) {
            if (app::dialogs::available() && widgets::button("Open another image..."))
                app::open_image();
            core::same_line();
            if (s.can_resume && widgets::button("Go on with the deep scan"))
                app::session().resume();
            else if (!s.can_resume && widgets::button("Scan again"))
                app::session().scan(s.image, { .carve = app::deep_scan });
            core::same_line();
            widgets::toggle("Deep scan", &app::deep_scan);
        } else if (widgets::button(s.phase == storage::SessionPhase::exporting ? "Stop exporting" : "Stop")) {
            app::session().cancel();
        }
        progress(s, right_gap);
    }

    // which files
    const std::shared_ptr<const t_files> files = mock ? mock_files() : s.files;
    const char* statuses[] = { "All", "Intact", "Partial", "Uncertain", "Unrecoverable" };
    const char* origins[] = { "Deleted and carved", "Deleted", "Carved", "Everything" };
    { // side by side: the statuses as segments, which files as a combo, there's no room for both as segments
        const float combo_width = px(190.f);
        const float labels = render::text::size("Show").x + render::text::size("Found").x + style::general::inner_spacing.x * 2.f + style::general::spacing.x;
        const float was = core::set_item_width(IM_TRUNC(core::available().x + right_gap - labels - combo_width));
        widgets::segmented("Show", &status_filter, statuses, IM_ARRAYSIZE(statuses));
        core::same_line();
        core::set_item_width(combo_width);
        widgets::combo("Found", &origin_filter, origins, IM_ARRAYSIZE(origins));
        core::set_item_width(was);
    }
    rebuild(files);

    std::string count = app::thousands(shown.size()) + " of " + app::thousands(files->size()) + " files";
    if (!mock && s.carve_dropped != 0)
        count += "   (the deep scan found " + app::thousands(s.carve_dropped) + " of the filesystems' ones again, they're listed once)";
    widgets::text(count.c_str());

    // the column titles, over the list's columns
    const ImVec2 header_pos = core::cursor();
    const ImRect header(header_pos, header_pos + ImVec2(core::available().x + right_gap, core::font_size()));
    core::layout(header.GetSize(), 0.f);
    if (core::add(header, 0)) {
        const char* titles[] = { "Name", "Size", "Found by", "Status" };
        for (int c = 0; c < 4; c++) {
            const ImRect r = cell(header, c);
            render::text::draw_aligned(core::drawlist(), r.Min, r.Max, ImVec2(c == 1 ? 1.f : 0.f, 0.5f), dim(), titles[c]);
        }
    }

    const float line = core::font_size() + px(3.f);
    const float details_height = line * 8.f;
    const float export_height = core::frame_height() + style::general::spacing.y * 2.f;
    const float row_height = core::frame_height();
    const ImVec4 accent = style::general::accent.Value, text = style::themes::active.text.Value;
    const float rounding = (float)style::general::rounding;
    widgets::virtual_list("##files", ImVec2(right_gap, -(details_height + export_height + style::general::spacing.y)), (int)shown.size(), row_height, &selected,
        [&](int index, const ImRect& row, bool is_selected, bool is_hovered) {
            const FoundFile& f = (*files)[shown[(std::size_t)index]];
            ImDrawList* drawlist = core::drawlist();
            if (is_selected)
                drawlist->AddRectFilled(row.Min, row.Max, ImColor(accent.x, accent.y, accent.z, 0.22f), rounding);
            else if (is_hovered)
                drawlist->AddRectFilled(row.Min, row.Max, ImColor(text.x, text.y, text.z, 0.06f), rounding);
            const std::string& name = f.file.origin == Origin::carved ? f.file.name : f.file.path;
            const std::string size = f.file.directory ? "folder" : app::human_size(f.file.size);
            const std::string by = found_by(f, s);
            ImRect r = cell(row, 0);
            render::text::draw_aligned(drawlist, r.Min, r.Max, ImVec2(0.f, 0.5f), style::themes::active.text, name.c_str(), nullptr, nullptr, &r);
            r = cell(row, 1);
            render::text::draw_aligned(drawlist, r.Min, r.Max, ImVec2(1.f, 0.5f), style::themes::active.text, size.c_str());
            r = cell(row, 2);
            render::text::draw_aligned(drawlist, r.Min, r.Max, ImVec2(0.f, 0.5f), dim(0.85f), by.c_str(), nullptr, nullptr, &r);
            r = cell(row, 3);
            render::text::draw_aligned(drawlist, r.Min, r.Max, ImVec2(0.f, 0.5f), status_color(f.file.integrity), status_name(f.file.integrity));
        });

    // the picked one
    core::layout(ImVec2(0.f, style::general::spacing.y));
    const FoundFile* picked = selected >= 0 && selected < (int)shown.size() ? &(*files)[shown[(std::size_t)selected]] : nullptr;
    selected_id = picked ? picked->file.id : UINT64_MAX;
    if (picked) {
        if (!mock && previewed != picked->file.id) {
            app::session().preview(picked->file.id);
            previewed = picked->file.id;
        }
        details(*picked, s, details_height, right_gap);
    } else {
        const ImVec2 pos = core::cursor();
        core::layout(ImRect(pos, pos + ImVec2(core::available().x + right_gap, details_height)), 0.f);
        render::text::draw(core::drawlist(), pos, dim(), "Pick a file to see where it was found, why it has its status and its first bytes.");
    }

    // export: where to, and what
    if (!mock && !busy) {
        if (app::dialogs::available() && widgets::button(destination ? "Another folder..." : "Choose where to export..."))
            app::dialogs::show(app::dialogs::e_kind::folder, dialog_destination);
        if (destination) {
            std::vector<std::uint64_t> ids;
            for (std::uint32_t i : shown)
                ids.push_back((*files)[i].file.id);
            const std::string all = "Export the " + app::thousands(shown.size()) + " shown";
            core::same_line();
            if (!shown.empty() && widgets::button(all.c_str()))
                app::session().export_files(ids, *destination);
            core::same_line();
            if (picked && widgets::button("Export the picked one"))
                app::session().export_files({ picked->file.id }, *destination);
            core::same_line();
            const std::string to = "to " + app::fit(app::utf8(*destination), core::available().x + right_gap - render::text::size("to ").x);
            widgets::text(to.c_str());
        } else {
            core::same_line();
            widgets::text("Use a folder on a different drive than the one the image came from, if that's the drive being recovered.");
        }
    }
    render::groupbox::end();
}
