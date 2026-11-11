// SPDX-License-Identifier: MIT
#include "PanelAnchors.cpp"
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <string>

// Always-on check (assert may be compiled out).
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

using panel_anchors::Anchors;

static bool approx(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    {  // px user units, nested group transforms, comments and non-anchor elements.
        Anchors a;
        CHECK(a.parse(R"svg(<?xml version="1.0"?>
<svg width="100" height="50" viewBox="0 0 100 50">
  <!-- <rect id="COMMENTED" x="1" y="1" width="1" height="1"/> -->
  <rect x="0" y="0" width="100" height="50"/>
  <g transform="translate(10, 5)">
    <g transform="scale(2)"><circle id="C" cx="3" cy="4" r="1"/></g>
    <rect id="R" x="1" y="2" width="3" height="4"/>
  </g>
  <ellipse id="E" cx="50" cy="25" rx="4" ry="2"/>
</svg>)svg"));
        CHECK(a.size() == 3 && !a.find("COMMENTED"));
        CHECK(approx(a.width, 100) && approx(a.height, 50));
        const auto* c = a.find("C");
        CHECK(c && approx(c->cx, 16) && approx(c->cy, 13) && approx(c->r, 2) && !c->isRect);
        const auto* r = a.find("R");
        CHECK(r && r->isRect && approx(r->x, 11) && approx(r->y, 7) && approx(r->w, 3) && approx(r->h, 4));
        const auto* e = a.find("E");
        CHECK(e && approx(e->w, 8) && approx(e->h, 4));
    }
    {  // mm document with Leviathan-style 0.01 mm user units.
        Anchors a;
        CHECK(a.parse(R"svg(<svg width="50.8mm" height="128.5mm" viewBox="0 0 5080 12850"><circle id="P" cx="2540" cy="0" r="10"/></svg>)svg"));
        CHECK(approx(a.width, 150));
        CHECK(approx(a.find("P")->cx, 75));
    }
    {  // The shipped panel defines every anchor the widget reads.
        Anchors a;
        CHECK(a.load("res/FM1Host.svg"));
        CHECK(approx(a.width, 63 * 15) && approx(a.height, 380));
        const char* ids[] = {"BODY", "SCREEN", "WELL_OCT", "WELL_FUNC", "KEYBED", "WELL_OUT", "MASTER",
                             "ENC_SELECT", "ENC_PRESETS", "ENC_ALGORITHM", "ENC_KNOB1", "ENC_KNOB2", "ENC_KNOB3", "ENC_KNOB4",
                             "BTN_OCT_DOWN", "BTN_OCT_UP", "BTN_FX", "BTN_SEL", "BTN_ENV", "BTN_LFO", "BTN_EDIT", "BTN_GLO",
                             "BTN_HOME", "BTN_SAVE", "BTN_ARP", "BTN_SEQ", "BTN_PLAY", "BTN_REC",
                             "JACK_VOCT", "JACK_GATE", "JACK_VEL", "JACK_CC", "JACK_CLK", "JACK_RUN", "JACK_RESET", "JACK_L", "JACK_R"};
        for (const char* id : ids)
            if (!a.find(id)) { std::fprintf(stderr, "missing %s\n", id); return 1; }
        for (int k = 14; k <= 40; ++k) CHECK(a.find("KEY_" + std::to_string(k)));
        const auto* s = a.find("SCREEN");
        CHECK(approx(s->w, 360) && approx(s->h, 360));
    }
    std::puts("panel anchors ok");
}
