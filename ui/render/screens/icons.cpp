#include "../../ui.h"
#include "icons.h"

// each icon is drawn in a box of size around center, on unit coordinates from -0.5 to 0.5 scaled to it

namespace {
    struct t_pen {
        ImDrawList* drawlist;
        ImVec2 center;
        float size;
        ImU32 color;
        float thickness;

        ImVec2 at(float x, float y) const { return ImVec2(center.x + x * size, center.y + y * size); }
        void line(float x1, float y1, float x2, float y2) const { drawlist->AddLine(at(x1, y1), at(x2, y2), color, thickness); }
        void lines(std::initializer_list<ImVec2> points, bool closed = false) const {
            for (const ImVec2& p : points)
                drawlist->PathLineTo(at(p.x, p.y));
            drawlist->PathStroke(color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
        }
        void circle(float x, float y, float r) const { drawlist->AddCircle(at(x, y), r * size, color, 0, thickness); }
        void dot(float x, float y, float r) const { drawlist->AddCircleFilled(at(x, y), r * size, color); }
        void arc(float x, float y, float r, float from, float to) const {
            drawlist->PathArcTo(at(x, y), r * size, from, to);
            drawlist->PathStroke(color, ImDrawFlags_None, thickness);
        }
    };
}

void fenrir::icons::draw(ImDrawList* drawlist, e_icon icon, ImVec2 center, float size, ImU32 color, float thickness) {
    const t_pen pen{ drawlist, center, size, color, thickness };
    switch (icon) {
    case e_icon::home: // a roof over a house with a door
        pen.lines({ { -0.42f, -0.02f }, { 0.f, -0.42f }, { 0.42f, -0.02f } });
        pen.lines({ { -0.30f, -0.12f }, { -0.30f, 0.40f }, { 0.30f, 0.40f }, { 0.30f, -0.12f } });
        pen.lines({ { -0.08f, 0.40f }, { -0.08f, 0.14f }, { 0.08f, 0.14f }, { 0.08f, 0.40f } });
        break;
    case e_icon::recover: { // an arrow coming back around, with a file's worth of dot in the middle
        const float pi = IM_PI;
        pen.arc(0.f, 0.f, 0.38f, -pi * 0.30f, pi * 1.25f);
        const ImVec2 tip(0.38f * cosf(-pi * 0.30f), 0.38f * sinf(-pi * 0.30f));
        drawlist->AddTriangleFilled(pen.at(tip.x + 0.17f, tip.y - 0.02f), pen.at(tip.x - 0.10f, tip.y - 0.13f), pen.at(tip.x - 0.02f, tip.y + 0.15f), color);
        pen.dot(0.f, 0.f, 0.09f);
        break;
    }
    case e_icon::images: // a platter with its spindle and the arm reading it
        pen.circle(0.f, 0.f, 0.40f);
        pen.circle(0.f, 0.f, 0.10f);
        pen.line(0.40f, 0.40f, 0.12f, 0.12f);
        pen.dot(0.40f, 0.40f, 0.07f);
        break;
    case e_icon::erase: // a shield, crossed out until erasure exists
        pen.lines({ { 0.f, -0.44f }, { 0.36f, -0.30f }, { 0.32f, 0.08f }, { 0.f, 0.44f }, { -0.32f, 0.08f }, { -0.36f, -0.30f } }, true);
        pen.line(-0.14f, -0.12f, 0.14f, 0.16f);
        pen.line(0.14f, -0.12f, -0.14f, 0.16f);
        break;
    case e_icon::reports: // a page with a folded corner and three lines on it
        pen.lines({ { 0.12f, -0.44f }, { -0.32f, -0.44f }, { -0.32f, 0.44f }, { 0.32f, 0.44f }, { 0.32f, -0.24f }, { 0.12f, -0.44f }, { 0.12f, -0.24f }, { 0.32f, -0.24f } });
        pen.line(-0.18f, -0.02f, 0.18f, -0.02f);
        pen.line(-0.18f, 0.13f, 0.18f, 0.13f);
        pen.line(-0.18f, 0.28f, 0.06f, 0.28f);
        break;
    case e_icon::settings: // three sliders, their knobs at different places
        for (int i = 0; i < 3; i++) {
            const float y = -0.30f + 0.30f * i, knob = i == 0 ? 0.14f : i == 1 ? -0.18f : 0.04f;
            pen.line(-0.42f, y, 0.42f, y);
            pen.dot(knob, y, 0.11f);
        }
        break;
    case e_icon::lock: // a body and its shackle
        pen.arc(0.f, -0.12f, 0.20f, IM_PI, IM_PI * 2.f);
        pen.line(-0.20f, -0.12f, -0.20f, 0.02f);
        pen.line(0.20f, -0.12f, 0.20f, 0.02f);
        drawlist->AddRectFilled(pen.at(-0.32f, 0.02f), pen.at(0.32f, 0.44f), color, 0.08f * size);
        break;
    case e_icon::minimize:
        pen.line(-0.30f, 0.f, 0.30f, 0.f);
        break;
    case e_icon::close:
        pen.line(-0.28f, -0.28f, 0.28f, 0.28f);
        pen.line(0.28f, -0.28f, -0.28f, 0.28f);
        break;
    }
}

void fenrir::icons::fehu(ImDrawList* drawlist, ImVec2 center, float size, ImU32 color, float thickness) {
    // a stave with two branches rising to the right, the upper one from the top, the lower one from the middle
    const t_pen pen{ drawlist, center, size, color, thickness };
    pen.line(-0.18f, -0.50f, -0.18f, 0.50f);
    pen.line(-0.18f, -0.18f, 0.26f, -0.50f);
    pen.line(-0.18f, 0.12f, 0.26f, -0.20f);
}
