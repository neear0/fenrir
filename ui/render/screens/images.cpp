#include "../../ui.h"
#include "app.h"
#include "screens.h"

// images: the open image, its partition table, what was found in each part of it, and everything the scan noticed
// that's damaged or odd. read from the session's snapshot, the image itself is only read by the session

using namespace celosia;
namespace app = fenrir::app;

namespace {
    float px(float v) { return v * style::scale; }
    ImColor dim(float alpha = 0.6f) { ImVec4 c = style::themes::active.text.Value; c.w *= alpha; return ImColor(c); }

    // a line of cells at fixed x positions, so the table's columns line up
    void row(const std::vector<std::string>& cells, const float* x, ImColor color) {
        const ImVec2 pos = core::cursor();
        const ImRect bb(pos, pos + ImVec2(core::available().x, core::font_size() + px(4.f)));
        core::layout(bb.GetSize(), 0.f);
        if (!core::add(bb, 0))
            return;
        for (std::size_t i = 0; i < cells.size(); i++) {
            const ImRect clip(ImVec2(pos.x + x[i], bb.Min.y), ImVec2(i + 1 < cells.size() ? pos.x + x[i + 1] - px(8.f) : bb.Max.x, bb.Max.y));
            render::text::draw_aligned(core::drawlist(), clip.Min, clip.Max, ImVec2(0.f, 0.f), color, cells[i].c_str(), nullptr, nullptr, &clip);
        }
    }

    ImColor copied_color(float share) { ImVec4 a = style::general::accent.Value; a.w = 0.25f + 0.6f * share; return ImColor(a); }
    ImColor bad_color() { return ImColor(232, 72, 85, 220); }

    // a map as a bar, start of the disk on the left: copied, bad (any bad sector in a slice shows), not read yet
    void map_bar(const std::vector<storage::MapCell>& cells) {
        const ImVec2 pos = core::cursor();
        const ImRect bb(pos, pos + ImVec2(core::available().x, px(20.f)));
        core::layout(bb.GetSize(), 0.f);
        if (!core::add(bb, 0))
            return;
        ImDrawList* drawlist = core::drawlist();
        drawlist->AddRectFilled(bb.Min, bb.Max, dim(0.07f), px(4.f));
        const float width = bb.GetWidth() / (float)(cells.empty() ? 1 : cells.size());
        for (std::size_t i = 0; i < cells.size(); i++) {
            const storage::MapCell& c = cells[i];
            if (c.finished <= 0.f && c.bad <= 0.f)
                continue;
            const ImVec2 a(bb.Min.x + width * (float)i, bb.Min.y), b(bb.Min.x + width * (float)(i + 1), bb.Max.y);
            drawlist->AddRectFilled(a, b, c.bad > 0.f ? bad_color() : copied_color(c.finished));
        }

        // the legend under it
        const ImVec2 at = core::cursor();
        const float line = core::font_size() + px(6.f), box = core::font_size() * 0.6f;
        const ImRect legend(at, at + ImVec2(core::available().x, line));
        core::layout(legend.GetSize(), 0.f);
        if (!core::add(legend, 0))
            return;
        float x = legend.Min.x;
        const std::pair<ImColor, const char*> items[] = { { copied_color(1.f), "copied" }, { bad_color(), "bad" }, { dim(0.15f), "not read yet" } };
        for (const auto& [color, label] : items) {
            const float y = legend.Min.y + (line - box) * 0.5f;
            drawlist->AddRectFilled(ImVec2(x, y), ImVec2(x + box, y + box), color, px(2.f));
            x += box + px(6.f);
            render::text::draw_aligned(drawlist, ImVec2(x, legend.Min.y), ImVec2(legend.Max.x, legend.Max.y), ImVec2(0.f, 0.5f), dim(0.8f), label);
            x += render::text::size(label).x + px(18.f);
        }
    }

    std::string phase_text(storage::ImagingPhase phase) {
        switch (phase) {
        case storage::ImagingPhase::copying: return "copying";
        case storage::ImagingPhase::trimming: return "trimming around what failed";
        case storage::ImagingPhase::scraping: return "reading what failed sector by sector";
        case storage::ImagingPhase::retrying: return "retrying the bad sectors";
        case storage::ImagingPhase::finished: return "finished";
        }
        return "";
    }

    std::string amounts(const storage::ImagingStatus& st) {
        const std::uint64_t percent = st.size ? st.finished * 100 / st.size : 0;
        return std::to_string(percent) + "% copied (" + app::human_size(st.finished) + " of " + app::human_size(st.size) + "), " + app::human_size(st.bad) + " bad, " +
            app::human_size(st.unknown) + " not read yet";
    }

    void drives();

    // making an image: a copy of a disk image into a new file with its map, on the imaging session's thread
    void make_image() {
        const storage::ImagingSnapshot& m = app::imaging_now();
        render::groupbox::begin("Make an image", "A copy of a disk that reads what it can and keeps a map of what it couldn't");
        if (!app::dialogs::available()) {
            widgets::text("This build has no file dialog yet (the GLFW one): use fenrir-cli image.");
            render::groupbox::end();
            return;
        }

        if (m.running) {
            const std::string what = phase_text(m.status.phase) + (m.status.phase == storage::ImagingPhase::finished ? "" : ", pass " + std::to_string(m.status.pass)) +
                ":  " + amounts(m.status);
            widgets::text(app::fit(what, core::available().x).c_str());
            const std::string more = app::thousands(m.status.read_errors) + " read errors so far,  " + app::duration(m.seconds) + "  ->  " + app::utf8(m.image.filename());
            widgets::text(more.c_str());
            map_bar(m.bar);
            if (widgets::button("Stop"))
                app::imaging().cancel();
            render::groupbox::end();
            return;
        }

        if (m.version != 0 && !m.image.empty()) { // how the last one went
            std::string how;
            if (m.done)
                how = m.status.bad == 0 && m.status.unknown == 0 ? "Done, all of it copied" : "Done: " + amounts(m.status);
            else if (m.error && m.error->code == storage::Errc::cancelled)
                how = "Stopped: " + amounts(m.status) + ". It goes on from its map";
            else if (m.error)
                how = std::string("It couldn't copy: ") + m.error->context + ": " + std::string(storage::to_string(m.error->code));
            widgets::text(app::fit(how, core::available().x).c_str());
            if (!m.bar.empty())
                map_bar(m.bar);
            const std::string files = app::fit(app::utf8(m.image) + "  with its map  " + app::utf8(m.mapfile.filename()), core::available().x);
            widgets::text(files.c_str());
            if (!m.done && m.error && m.error->code == storage::Errc::cancelled && widgets::button("Go on"))
                app::imaging().start(m.source, m.image);
            if ((m.done || m.status.finished != 0) && !app::session().busy() && widgets::button("Look for files in the copy"))
                app::session().scan(m.image, { .carve = app::deep_scan });
            core::layout(ImVec2(0.f, core::font_size() * 0.5f));
        }

        const std::string source = app::imaging_source ? "Copy: " + app::utf8(*app::imaging_source) : "Copy: (pick a disk image)";
        const std::string target = app::imaging_target ? "Into: " + app::utf8(*app::imaging_target) : "Into: (pick where the copy goes)";
        widgets::text(app::fit(source, core::available().x).c_str());
        widgets::text(app::fit(target, core::available().x).c_str());
        if (widgets::button("Disk image to copy..."))
            app::pick_imaging_source();
        core::same_line(style::general::inner_spacing.x);
        if (widgets::button("Where the copy goes..."))
            app::pick_imaging_target();
        if (app::imaging_source && app::imaging_target) {
            core::same_line(style::general::inner_spacing.x);
            if (widgets::button("Start copying"))
                app::imaging().start(*app::imaging_source, *app::imaging_target);
        }
        widgets::text("Nothing is written over: the copy is a new file, or a stopped one with its map next to it, which goes on.");
        drives();
        render::groupbox::end();
    }

    // the drives, any of which can be the one to copy: picking one only reads it, and never anything onto it
    void drives() {
        static bool listed = false;
        if (!listed) {
            app::drives::refresh();
            listed = true;
        }
        core::layout(ImVec2(0.f, core::font_size() * 0.5f));
        const bool elevated = app::drives::elevated();
        widgets::text(elevated ? "Or copy a drive. It's only read: the copy can't go onto the drive itself."
            : "Or copy a drive: that needs administrator rights, for reading it. Nothing is ever written to it.");
        const std::vector<storage::DiskInfo>& disks = app::drives::list();
        if (disks.empty())
            widgets::text(app::drives::listing() ? "Looking for drives..." : "No drives found.");
        for (const storage::DiskInfo& d : disks) {
            std::string label = "Drive " + std::to_string(d.number) + ":  " + (d.size ? app::human_size(d.size) : std::string("no medium")) + "  " + d.bus + "  " +
                (d.vendor.empty() ? d.model : d.vendor + " " + d.model);
            for (const std::string& v : d.volumes)
                if (v.size() <= 3) // drive letters, not volume names
                    label += "  " + v;
            if (d.system)
                label += "  (Windows is on it)";
            label += "##drive" + std::to_string(d.number);
            if (!elevated || d.size == 0) {
                widgets::text(label.substr(0, label.find("##")).c_str());
                continue;
            }
            if (widgets::button(label.c_str()))
                app::imaging_source = d.path;
        }
        if (widgets::button(app::drives::listing() ? "Looking..." : "Look again"))
            app::drives::refresh();
        if (!elevated && !app::imaging().busy()) {
            core::same_line(style::general::inner_spacing.x);
            if (widgets::button("Restart as administrator") && app::drives::restart_elevated())
                platform::close();
        }
    }
}

void fenrir::screens::images() {
    make_image();

    const storage::SessionSnapshot& s = app::now();
    const bool busy = s.phase != storage::SessionPhase::idle;
    const std::string description = s.open ? app::utf8(s.image) : "Raw images (.img, .dd, .bin), opened read-only";
    render::groupbox::begin("Disk image", description.c_str());
    if (!s.open) {
        widgets::text(busy ? "Opening it..." : s.error ? s.status.c_str() : "No image is open.");
        if (!busy && app::dialogs::available() && widgets::button("Open an image..."))
            app::open_image();
        core::layout(ImVec2(0.f, core::font_size() * 0.5f));
        widgets::text("Images are opened read-only, and on Windows nothing else can write to them while they're open.");
        widgets::text("An image with a map next to it (one made above, or by ddrescue) is read through it: what wasn't copied isn't taken for data.");
        render::groupbox::end();
        return;
    }

    if (s.map) { // the map it's read through
        static std::uint64_t drawn_version = UINT64_MAX;
        static std::vector<storage::MapCell> cells;
        if (drawn_version != s.version) {
            cells = storage::summarize(*s.map, storage::ImagingSession::bar_cells);
            drawn_version = s.version;
        }
        const std::string line = "Read through its map " + app::utf8(s.mapfile.filename()) + ": " + app::human_size(s.map->bytes(storage::BlockStatus::finished)) +
            " copied, " + app::human_size(s.map->bytes(storage::BlockStatus::bad)) + " bad, " + app::human_size(s.map->unknown_bytes()) + " not read";
        widgets::text(app::fit(line, core::available().x).c_str());
        map_bar(cells);
    }

    const storage::PartitionMap& map = s.partitions;
    std::string scheme;
    switch (map.scheme) {
    case storage::PartitionScheme::none: scheme = "none"; break;
    case storage::PartitionScheme::mbr: scheme = "MBR"; break;
    case storage::PartitionScheme::gpt: scheme = map.gpt_from_backup ? "GPT, read from the backup at the end of the disk" : "GPT"; break;
    }
    const std::string size = app::human_size(s.image_size) + "  (" + app::thousands(s.image_size) + " bytes, " + app::thousands(map.disk_sectors) + " sectors of " +
        std::to_string(map.sector_size) + ")";
    const std::string table = "Partition table: " + scheme;
    widgets::text(("Size: " + size).c_str());
    widgets::text(table.c_str());
    core::layout(ImVec2(0.f, core::font_size() * 0.5f));

    // every part of it files were looked for in
    const float x[] = { 0.f, px(130.f), px(240.f), px(330.f), px(430.f), px(520.f) };
    row({ "", "Starts at", "Size", "Filesystem", "Files found", "Type" }, x, dim());
    for (const storage::SessionVolume& v : s.volumes) {
        std::string type;
        for (const storage::Partition& p : map.partitions)
            if ("partition " + std::to_string(p.number) == v.name)
                type = map.scheme == storage::PartitionScheme::mbr ? std::string(p.type_name.empty() ? "unknown type" : p.type_name)
                    : std::string(p.type_name.empty() ? storage::to_string(p.type_guid) : std::string(p.type_name)) + (p.name.empty() ? "" : ", " + p.name);
        if (v.offset == 0 && v.name == "the whole image")
            type = s.carving ? "the deep scan looks through all of it" : "not looked through (no deep scan)";
        const std::string files = v.scanned || v.offset == 0 ? app::thousands(v.files) : v.filesystem == storage::FileSystem::unknown ? "-" : "not read yet";
        row({ v.name, "byte " + app::thousands(v.offset), app::human_size(v.size), std::string(storage::to_string(v.filesystem)), files, type }, x,
            style::themes::active.text);
    }
    for (const storage::Partition& p : map.partitions)
        if (!p.inside_disk)
            row({ "partition " + std::to_string(p.number), "byte " + app::thousands(p.offset_bytes), app::human_size(p.size_bytes), "", "",
                "runs past the end of the image, not read" }, x, dim());

    core::layout(ImVec2(0.f, core::font_size() * 0.5f));
    if (s.carving) {
        std::string deep = s.carved_to >= s.image_size ? "Deep scan: all of it was looked through"
            : "Deep scan: looked through up to byte " + app::thousands(s.carved_to) + (s.can_resume ? ", it was stopped and can go on (Recover)" : "");
        if (s.carve_rejected != 0)
            deep += ". " + app::thousands(s.carve_rejected) + " signatures didn't hold up as files";
        widgets::text(deep.c_str());
    }

    // what's damaged or odd
    core::layout(ImVec2(0.f, core::font_size() * 0.5f));
    if (s.diagnostics.empty()) {
        widgets::text("Nothing damaged or odd was noticed.");
    } else {
        const std::string heading = s.diagnostics.size() == 1 ? "1 thing noticed:" : std::to_string(s.diagnostics.size()) + " things noticed:";
        widgets::text(heading.c_str());
        for (const storage::Diagnostic& d : s.diagnostics) {
            const std::string line = std::string(storage::to_string(d.severity)) + ": " + d.message;
            widgets::text(line.c_str());
        }
    }
    render::groupbox::end();
}
