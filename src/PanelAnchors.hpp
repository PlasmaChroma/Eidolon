// SPDX-License-Identifier: MIT
// Slim runtime panel-anchor reader, after Leviathan's panel_svg anchors.
// Panel geometry lives in an SVG as id'd <rect>/<circle>/<ellipse> elements
// (conventionally inside a hidden "component_anchors" group). The file is
// parsed once; lookups return Rack panel pixels. No Rack dependency so it can
// be unit-tested on its own.
#pragma once
#include <map>
#include <string>

namespace panel_anchors {

// Rack panel pixels per millimetre (RACK_GRID_WIDTH 15 px == 5.08 mm).
constexpr float PX_PER_MM = 75.f / 25.4f;

struct Anchor {
    float cx = 0, cy = 0;          // centre (always set)
    float x = 0, y = 0, w = 0, h = 0;  // bounds (rects; circles get their bounding box)
    float r = 0;                   // radius (circles / ellipses: mean radius)
    bool isRect = false;
};

class Anchors {
public:
    // Parses SVG text. Units: if the root <svg> has width="..mm" and a viewBox,
    // user units are scaled to mm and then to px; otherwise user units are px.
    bool parse(const std::string& svgText);
    bool load(const std::string& path);
    const Anchor* find(const std::string& id) const;
    bool empty() const { return anchors.empty(); }
    size_t size() const { return anchors.size(); }
    // Root document size in panel px (0 if unknown).
    float width = 0, height = 0;

private:
    std::map<std::string, Anchor> anchors;
};

} // namespace panel_anchors
