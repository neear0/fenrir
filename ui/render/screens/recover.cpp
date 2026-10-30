#include "../../ui.h"
#include "screens.h"

// recover: the candidates a scan found and the one picked. for now they're 100,000 made up ones, generated once and the
// same every run, so the list, the filter and the keyboard can be tried at a realistic size. the page says they're mock
// everywhere they show, and there's no export until the candidates are real (P2)

namespace {
    enum class e_status { intact, partial, uncertain, unrecoverable, count };
    const char* status_names[] = { "Intact", "Partial", "Uncertain", "Unrecoverable" };
    const char* status_reasons[] = {
        "Contiguous clusters, the file's structure checks out",
        "Ends early, the clusters after it were reused",
        "The cluster chain is gone, assumed contiguous",
        "Its clusters were overwritten by another file",
    };

    struct t_mock_candidate {
        std::string name;
        std::uint64_t size;
        std::uint64_t start;  // first cluster for filesystem entries, byte offset for carved ones
        bool carved;          // found by its content, without a filesystem entry, so without its name
        const char* found_by;
        e_status status;
    };

    constexpr int mock_count = 100000;

    const std::vector<t_mock_candidate>& mock_candidates() {
        static const std::vector<t_mock_candidate> list = [] {
            std::vector<t_mock_candidate> out;
            out.reserve(mock_count);
            std::uint32_t state = 0x9E3779B9u; // xorshift, the same list every run
            const auto next = [&state] { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
            const char* named[] = { "IMG_%04u.JPG", "DSC%05u.JPG", "Screenshot_%u.png", "invoice_%u.pdf", "notes_%u.txt", "backup_%u.zip" };
            const char* carved[] = { "carved_%06u.jpg", "carved_%06u.png", "carved_%06u.pdf", "carved_%06u.zip" };
            const char* carvers[] = { "Carved: JPEG", "Carved: PNG", "Carved: PDF", "Carved: ZIP" };
            char name[64];
            for (std::uint32_t i = 0; i < (std::uint32_t)mock_count; i++) {
                t_mock_candidate c;
                c.carved = next() % 3 == 0;
                if (c.carved) {
                    const std::uint32_t kind = next() % 4;
                    snprintf(name, sizeof(name), carved[kind], i);
                    c.found_by = carvers[kind];
                    c.start = (std::uint64_t)(next() % 4000000u) * 512u;
                }
                else {
                    snprintf(name, sizeof(name), named[next() % 6], next() % 10000u);
                    c.found_by = "FAT32 deleted entry";
                    c.start = 2 + next() % 900000u;
                }
                c.name = name;
                c.size = 200 + (std::uint64_t)(next() % 6000000u);
                const std::uint32_t roll = next() % 100;
                c.status = roll < 55 ? e_status::intact : roll < 75 ? e_status::partial : roll < 93 ? e_status::uncertain : e_status::unrecoverable;
                out.push_back(std::move(c));
            }
            return out;
        }();
        return list;
    }

    std::string thousands(std::uint64_t value) { // 1234567 -> 1,234,567
        std::string digits = std::to_string(value), out;
        for (std::size_t i = 0; i < digits.size(); i++) {
            if (i > 0 && (digits.size() - i) % 3 == 0)
                out += ',';
            out += digits[i];
        }
        return out;
    }

    std::string human_size(std::uint64_t bytes) {
        const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
        double value = (double)bytes;
        int unit = 0;
        while (value >= 1024.0 && unit < 4) {
            value /= 1024.0;
            unit++;
        }
        char text[32];
        snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
        return text;
    }

    ImColor status_color(e_status status) { // readable against the panels, whichever theme is on
        static const ImColor colors[] = { { 110, 200, 90, 255 }, { 255, 176, 64, 255 }, { 150, 165, 200, 255 }, { 255, 96, 96, 255 } };
        return celosia::style::themes::readable(colors[(int)status], celosia::style::themes::active.background_darker, celosia::style::limits::accent_contrast_min);
    }

    // the columns' share of a row: name, size, found by, status
    constexpr float columns[] = { 0.44f, 0.14f, 0.22f, 0.20f };
    ImRect cell(const ImRect& row, int column) {
        float x = row.Min.x;
        for (int i = 0; i < column; i++)
            x += row.GetWidth() * columns[i];
        const float pad = celosia::style::frame::size_padding.x;
        return ImRect(ImVec2(x + pad, row.Min.y), ImVec2(x + row.GetWidth() * columns[column] - pad, row.Max.y));
    }

    // a label and its value on one line, the values lined up after the labels
    void field(const char* label, const std::string& value, ImColor color = celosia::style::themes::active.text) {
        using namespace celosia;
        const float label_width = 90.f * style::scale;
        const ImVec2 pos = core::cursor();
        const ImVec2 value_size = render::text::size(value.c_str());
        const ImVec2 size(label_width + value_size.x, value_size.y);
        core::layout(size, 0.f);
        if (!core::add(ImRect(pos, pos + size), 0))
            return;
        ImVec4 dim = style::themes::active.text.Value;
        dim.w *= 0.6f;
        render::text::draw(core::drawlist(), pos, ImColor(dim), label);
        render::text::draw(core::drawlist(), pos + ImVec2(label_width, 0.f), color, value.c_str());
    }

    int filter = 0;           // 0 is all, then one per status
    int filter_built = -1;    // what shown was built for
    std::vector<int> shown;   // indices into mock_candidates() that pass the filter
    int selected = -1;        // into shown
}

void fenrir::screens::recover() {
    using namespace celosia;
    const std::vector<t_mock_candidate>& candidates = mock_candidates();

    // one groupbox for both, the list takes what the details under it leave. two groupboxes would halve the list
    render::groupbox::begin("Candidates", "Mock data: nothing has been scanned", render::groupbox::e_group_layout::fullscreen);
    const float right_gap = -style::frame::size_padding.x; // the same room on the right as the groupbox has on the left
    const float line = core::font_size() + style::general::spacing.y;
    const float details_height = line * 8.f + style::frame::size_padding.y;
    const char* filters[] = { "All", "Intact", "Partial", "Uncertain", "Unrecoverable" };
    widgets::segmented("Show", &filter, filters, IM_ARRAYSIZE(filters));
    if (filter != filter_built) { // only when it changes, not every frame
        shown.clear();
        for (int i = 0; i < (int)candidates.size(); i++)
            if (filter == 0 || (int)candidates[i].status == filter - 1)
                shown.push_back(i);
        filter_built = filter;
        selected = -1;
    }

    const std::string count = thousands(shown.size()) + " of " + thousands(candidates.size()) + " mock candidates";
    widgets::text(count.c_str());

    // the column titles, over the list's columns
    const ImVec2 header_pos = core::cursor();
    const ImRect header(header_pos, header_pos + ImVec2(core::available().x + right_gap, core::font_size()));
    core::layout(header.GetSize(), 0.f);
    if (core::add(header, 0)) {
        ImVec4 dim = style::themes::active.text.Value;
        dim.w *= 0.6f;
        const char* titles[] = { "Name", "Size", "Found by", "Status" };
        for (int c = 0; c < 4; c++) {
            const ImRect r = cell(header, c);
            render::text::draw_aligned(core::drawlist(), r.Min, r.Max, ImVec2(c == 1 ? 1.f : 0.f, 0.5f), ImColor(dim), titles[c]);
        }
    }

    const float row_height = core::frame_height();
    const ImVec4 accent = style::general::accent.Value, text = style::themes::active.text.Value;
    const float rounding = (float)style::general::rounding;
    widgets::virtual_list("##candidates", ImVec2(right_gap, -details_height), (int)shown.size(), row_height, &selected,
        [&](int index, const ImRect& row, bool is_selected, bool is_hovered) {
            const t_mock_candidate& c = candidates[shown[index]];
            ImDrawList* drawlist = core::drawlist();
            if (is_selected)
                drawlist->AddRectFilled(row.Min, row.Max, ImColor(accent.x, accent.y, accent.z, 0.22f), rounding);
            else if (is_hovered)
                drawlist->AddRectFilled(row.Min, row.Max, ImColor(text.x, text.y, text.z, 0.06f), rounding);

            const std::string size = human_size(c.size);
            ImRect r = cell(row, 0);
            render::text::draw_aligned(drawlist, r.Min, r.Max, ImVec2(0.f, 0.5f), style::themes::active.text, c.name.c_str());
            r = cell(row, 1);
            render::text::draw_aligned(drawlist, r.Min, r.Max, ImVec2(1.f, 0.5f), style::themes::active.text, size.c_str());
            r = cell(row, 2);
            render::text::draw_aligned(drawlist, r.Min, r.Max, ImVec2(0.f, 0.5f), style::themes::active.text, c.found_by);
            r = cell(row, 3);
            render::text::draw_aligned(drawlist, r.Min, r.Max, ImVec2(0.f, 0.5f), status_color(c.status), status_names[(int)c.status]);
        });

    // the details of the picked one, under the list
    core::layout(ImVec2(0.f, style::frame::size_padding.y * 0.5f));
    if (selected < 0 || selected >= (int)shown.size())
        widgets::text("Pick a candidate to see where it came from and why it has its status.");
    else {
        const t_mock_candidate& c = candidates[shown[selected]];
        field("Name", c.carved ? c.name + "  (made up, carved files have no name)" : c.name);
        field("Size", human_size(c.size) + "  (" + thousands(c.size) + " bytes)");
        field("Found by", c.found_by);
        field("Status", status_names[(int)c.status], status_color(c.status));
        field("Why", status_reasons[(int)c.status]);
        if (c.carved)
            field("Where", "byte " + thousands(c.start) + " of the image");
        else
            field("Where", "clusters " + thousands(c.start) + " to " + thousands(c.start + c.size / 4096));
        core::layout(ImVec2(0.f, core::font_size() * 0.5f)); // a gap before the note
        widgets::text("This is mock data. Exporting comes with real candidates in P2, always to a different drive.");
    }
    render::groupbox::end();
}
