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
}

void fenrir::screens::images() {
    const storage::SessionSnapshot& s = app::now();
    const bool busy = s.phase != storage::SessionPhase::idle;
    const std::string description = s.open ? app::utf8(s.image) : "Raw images (.img, .dd, .bin), opened read-only";
    render::groupbox::begin("Disk image", description.c_str(),
        render::groupbox::e_group_layout::fullscreen);
    if (!s.open) {
        widgets::text(busy ? "Opening it..." : s.error ? s.status.c_str() : "No image is open.");
        if (!busy && app::dialogs::available() && widgets::button("Open an image..."))
            app::open_image();
        core::layout(ImVec2(0.f, core::font_size() * 0.5f));
        widgets::text("Images are opened read-only, and on Windows nothing else can write to them while they're open.");
        widgets::text("Drives and volumes aren't opened as images. Imaging a drive is milestone P6.");
        render::groupbox::end();
        return;
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
