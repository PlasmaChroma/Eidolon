// SPDX-License-Identifier: MIT
#include "PanelAnchors.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace panel_anchors {
namespace {

struct Affine {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Affine operator*(const Affine& r) const {
        return {a * r.a + c * r.b, b * r.a + d * r.b, a * r.c + c * r.d, b * r.c + d * r.d,
                a * r.e + c * r.f + e, b * r.e + d * r.f + f};
    }
    void apply(float x, float y, float* ox, float* oy) const {
        *ox = a * x + c * y + e;
        *oy = b * x + d * y + f;
    }
};

std::vector<float> numbers(const std::string& s) {
    std::vector<float> out;
    const char* p = s.c_str();
    while (*p) {
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end == p) { ++p; continue; }
        out.push_back(v);
        p = end;
    }
    return out;
}

// Supports matrix, translate, scale (the forms Inkscape writes for layout).
Affine parseTransform(const std::string& s) {
    Affine total;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t open = s.find('(', pos);
        if (open == std::string::npos) break;
        size_t close = s.find(')', open);
        if (close == std::string::npos) break;
        std::string name = s.substr(pos, open - pos);
        name.erase(std::remove_if(name.begin(), name.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)) || ch == ','; }), name.end());
        std::vector<float> v = numbers(s.substr(open + 1, close - open - 1));
        Affine t;
        if (name == "matrix" && v.size() >= 6) t = {v[0], v[1], v[2], v[3], v[4], v[5]};
        else if (name == "translate" && !v.empty()) { t.e = v[0]; t.f = v.size() > 1 ? v[1] : 0; }
        else if (name == "scale" && !v.empty()) { t.a = v[0]; t.d = v.size() > 1 ? v[1] : v[0]; }
        total = total * t;
        pos = close + 1;
    }
    return total;
}

// Tag body (between '<name' and '>') -> attribute map.
std::map<std::string, std::string> attributes(const std::string& body) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() && (std::isspace(static_cast<unsigned char>(body[i])) || body[i] == '/')) ++i;
        size_t nameStart = i;
        while (i < body.size() && body[i] != '=' && !std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        std::string name = body.substr(nameStart, i - nameStart);
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        if (i >= body.size() || body[i] != '=') continue;
        ++i;
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        if (i >= body.size() || (body[i] != '"' && body[i] != '\'')) continue;
        char quote = body[i++];
        size_t valueEnd = body.find(quote, i);
        if (valueEnd == std::string::npos) break;
        out[name] = body.substr(i, valueEnd - i);
        i = valueEnd + 1;
    }
    return out;
}

float num(const std::map<std::string, std::string>& a, const char* key, float fallback = 0) {
    auto it = a.find(key);
    return it == a.end() ? fallback : std::strtof(it->second.c_str(), nullptr);
}

} // namespace

bool Anchors::parse(const std::string& text) {
    anchors.clear();
    width = height = 0;
    std::vector<Affine> stack{Affine{}};
    float unitToPx = 1;
    size_t pos = 0;
    while ((pos = text.find('<', pos)) != std::string::npos) {
        if (text.compare(pos, 4, "<!--") == 0) {
            size_t end = text.find("-->", pos);
            if (end == std::string::npos) break;
            pos = end + 3; continue;
        }
        size_t end = text.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = text.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
        if (tag[0] == '/') {
            if (tag.compare(1, 1, "g") == 0 && (tag.size() == 2 || std::isspace(static_cast<unsigned char>(tag[2]))) && stack.size() > 1)
                stack.pop_back();
            continue;
        }
        size_t nameEnd = 0;
        while (nameEnd < tag.size() && !std::isspace(static_cast<unsigned char>(tag[nameEnd])) && tag[nameEnd] != '/') ++nameEnd;
        const std::string name = tag.substr(0, nameEnd);
        const bool selfClosing = tag.back() == '/';
        auto attr = attributes(tag.substr(nameEnd));
        Affine local = stack.back();
        if (attr.count("transform")) local = local * parseTransform(attr["transform"]);

        if (name == "svg") {
            std::vector<float> vb = attr.count("viewBox") ? numbers(attr["viewBox"]) : std::vector<float>{};
            const std::string w = attr.count("width") ? attr["width"] : "";
            const std::string h = attr.count("height") ? attr["height"] : "";
            if (vb.size() >= 4 && vb[2] > 0 && w.find("mm") != std::string::npos)
                unitToPx = std::strtof(w.c_str(), nullptr) / vb[2] * PX_PER_MM;
            else if (vb.size() >= 4 && vb[2] > 0 && !w.empty())
                unitToPx = std::strtof(w.c_str(), nullptr) / vb[2];
            if (vb.size() >= 4) { width = vb[2] * unitToPx; height = vb[3] * unitToPx; }
            else {
                width = std::strtof(w.c_str(), nullptr) * (w.find("mm") != std::string::npos ? PX_PER_MM : 1);
                height = std::strtof(h.c_str(), nullptr) * (h.find("mm") != std::string::npos ? PX_PER_MM : 1);
            }
            stack.front() = Affine{unitToPx, 0, 0, unitToPx, 0, 0};
            continue;
        }
        if (name == "g") {
            if (!selfClosing) stack.push_back(local);
            continue;
        }
        auto id = attr.find("id");
        if (id == attr.end()) continue;
        Anchor an;
        float x0, y0, x1, y1;
        if (name == "rect") {
            float x = num(attr, "x"), y = num(attr, "y"), w = num(attr, "width"), h = num(attr, "height");
            local.apply(x, y, &x0, &y0);
            local.apply(x + w, y + h, &x1, &y1);
            an.isRect = true;
        } else if (name == "circle" || name == "ellipse") {
            float cx = num(attr, "cx"), cy = num(attr, "cy");
            float rx = name == "circle" ? num(attr, "r") : num(attr, "rx");
            float ry = name == "circle" ? rx : num(attr, "ry", rx);
            local.apply(cx - rx, cy - ry, &x0, &y0);
            local.apply(cx + rx, cy + ry, &x1, &y1);
        } else {
            continue;
        }
        an.x = std::min(x0, x1); an.y = std::min(y0, y1);
        an.w = std::fabs(x1 - x0); an.h = std::fabs(y1 - y0);
        an.cx = an.x + an.w / 2; an.cy = an.y + an.h / 2;
        an.r = (an.w + an.h) / 4;
        anchors[id->second] = an;
    }
    return !anchors.empty();
}

bool Anchors::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { anchors.clear(); return false; }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str());
}

const Anchor* Anchors::find(const std::string& id) const {
    auto it = anchors.find(id);
    return it == anchors.end() ? nullptr : &it->second;
}

} // namespace panel_anchors
