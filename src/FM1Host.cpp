// SPDX-License-Identifier: MIT
#include "plugin.hpp"
#include "control.hpp"
#include "fm1_rack.h"
#include "PanelAnchors.hpp"
#include <osdialog.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

using namespace firmware_host;

struct FM1Host : Module {
    enum Params { MASTER, MIDI_CHANNEL, CC_NUMBER, NUM_PARAMS };
    enum Inputs { PITCH, GATE, VELOCITY, CC, CLOCK, RUN, RESET, NUM_INPUTS };
    enum Outputs { LEFT, RIGHT, NUM_OUTPUTS };
    Fm1Handle* runtime = nullptr;
    CvMidi mapper;
    StereoSrc src;
    Edge clockEdge, runEdge, resetEdge;
    std::atomic<bool> resetDsp{false};
    std::atomic<unsigned> clockMhz{240}, leadFrames{1024};
    std::atomic<bool> panicRequested{false};
    CcRateLimiter ccLimiter;
    int oldCc = -1, oldCcNumber = -1, oldCcChannel = -1;
    // Only UI/serialization takes this mutex; process() never does.
    std::mutex metadataMutex;
    std::string firmwareHash, stateId;
    std::string expectedHash;
    std::string pendingPreviousId;
    bool pendingUnload = false;
    uint64_t dspGeneration = 0;
    MidiRecovery recovery;

    FM1Host() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, 0);
        configParam(MASTER, 0.f, 1.f, .8f, "Emulated master level", "%", 0.f, 100.f);
        configParam(MIDI_CHANNEL, 1.f, 16.f, 1.f, "MIDI channel")->snapEnabled = true;
        configParam(CC_NUMBER, 0.f, 127.f, 1.f, "MIDI CC number")->snapEnabled = true;
        configInput(PITCH, "Pitch (polyphonic 1 V/oct, C4 = 0 V)");
        configInput(GATE, "Gate (polyphonic)");
        configInput(VELOCITY, "Velocity (polyphonic 0-10 V; unpatched = 100)");
        configInput(CC, "Control change (0-10 V)");
        configInput(CLOCK, "MIDI clock (one pulse per MIDI clock, 24 PPQN)");
        configInput(RUN, "Run gate (MIDI Start/Stop)");
        configInput(RESET, "Restart transport (MIDI Stop, position zero, Start)");
        configOutput(LEFT, "Left audio (+/-5 V nominal)");
        configOutput(RIGHT, "Right audio (+/-5 V nominal)");
        const std::string root = asset::user("FirmwareHost");
        if (fm1_abi_version() == FM1_ABI_VERSION) runtime = fm1_create(root.c_str());
    }
    ~FM1Host() override { if (runtime) fm1_destroy(runtime); }

    void importFirmware(const std::string& path) {
        if (runtime) {
            if (fm1_load_path(runtime, path.c_str())) {
                std::lock_guard<std::mutex> guard(metadataMutex);
                expectedHash.clear(); pendingPreviousId.clear(); pendingUnload = false;
            }
        }
    }
    void chooseFirmware() {
        osdialog_filters* filters = osdialog_filters_parse("FM-1 firmware:fwsc");
        char* path = osdialog_file(OSDIALOG_OPEN, nullptr, nullptr, filters);
        osdialog_filters_free(filters);
        if (path) { importFirmware(path); std::free(path); }
    }
    void unloadFirmware() {
        if (runtime) fm1_unload(runtime);
        { std::lock_guard<std::mutex> guard(metadataMutex); firmwareHash.clear(); stateId.clear(); expectedHash.clear(); pendingPreviousId.clear(); pendingUnload = true; }
    }
    bool readStatus(Fm1Status& status) {
        if (!runtime || !fm1_try_get_status(runtime, &status)) return false;
        std::string sha(status.firmware_sha256);
        std::string id(status.state_id);
        if (isDigest(sha) && isStateId(id)) {
            std::lock_guard<std::mutex> guard(metadataMutex);
            if (!pendingUnload && (expectedHash.empty() || expectedHash == sha) &&
                (pendingPreviousId.empty() || pendingPreviousId != id)) {
                firmwareHash = sha; stateId = id; expectedHash.clear(); pendingPreviousId.clear();
            }
        }
        return true;
    }
    void setOptions(unsigned clock, unsigned lead) {
        clockMhz.store(clock); leadFrames.store(lead);
        if (runtime) fm1_set_options(runtime, clock, lead);
    }
    void onReset(const ResetEvent&) override {
        if (runtime) fm1_reset(runtime);
    }
    void onSampleRateChange(const SampleRateChangeEvent&) override {
        resetDsp.store(true, std::memory_order_release);
    }

    json_t* dataToJson() override {
        if (runtime) fm1_request_save(runtime);
        Fm1Status snapshot{};
        readStatus(snapshot);
        std::lock_guard<std::mutex> guard(metadataMutex);
        json_t* root = json_object();
        json_object_set_new(root, "schema", json_integer(1));
        // Whitelist deliberately excludes firmware paths, package bytes, flash,
        // resource data and even changed-sector deltas. Local state only.
        if (isDigest(firmwareHash)) json_object_set_new(root, "firmwareSha256", json_string(firmwareHash.c_str()));
        if (isStateId(stateId)) json_object_set_new(root, "stateId", json_string(stateId.c_str()));
        json_object_set_new(root, "clockMhz", json_integer(clockMhz.load()));
        json_object_set_new(root, "leadFrames", json_integer(leadFrames.load()));
        return root;
    }
    void dataFromJson(json_t* root) override {
        if (!json_is_object(root)) return;
        json_t* schema = json_object_get(root, "schema");
        if (schema && (!json_is_integer(schema) || json_integer_value(schema) != 1)) return;
        auto getString = [&](const char* key) {
            const char* value = json_string_value(json_object_get(root, key));
            return value ? std::string(value) : std::string();
        };
        std::string sha = getString("firmwareSha256");
        std::string id = getString("stateId");
        if (!isDigest(sha)) sha.clear();
        if (!isStateId(id)) id.clear();
        Fm1Status before{};
        if (runtime) fm1_try_get_status(runtime, &before);
        {
            std::lock_guard<std::mutex> guard(metadataMutex);
            firmwareHash = sha; stateId = id; expectedHash = sha; pendingUnload = sha.empty();
            pendingPreviousId = before.state_id;
        }
        unsigned clock = 240, lead = 1024;
        auto c = json_integer_value(json_object_get(root, "clockMhz"));
        if (c == 120 || c == 160 || c == 240 || c == 320 || c == 400) clock = static_cast<unsigned>(c);
        auto l = json_integer_value(json_object_get(root, "leadFrames"));
        if (l >= 256 && l <= 4096) lead = static_cast<unsigned>(l);
        setOptions(clock, lead);
        if (runtime) {
            if (!sha.empty()) fm1_load_cached(runtime, sha.c_str(), id.c_str());
            else fm1_unload(runtime);
        }
    }

    void process(const ProcessArgs& args) override {
        if (!runtime) {
            outputs[LEFT].setVoltage(0); outputs[RIGHT].setVoltage(0); return;
        }
        const uint64_t generation = fm1_generation(runtime);
        if (generation != dspGeneration) {
            dspGeneration = generation;
            mapper.clear(); src.clear();
            clockEdge = {}; runEdge = {}; resetEdge = {};
            oldCc = oldCcNumber = oldCcChannel = -1;
            ccLimiter = {};
            recovery.clear();
        }
        if (resetDsp.exchange(false, std::memory_order_acquire)) {
            src.clear();
            panicRequested.store(true);
        }
        auto sendRaw = [&](int status, int a, int b, unsigned length = 3) {
            const uint8_t bytes[3] = {static_cast<uint8_t>(status), static_cast<uint8_t>(a), static_cast<uint8_t>(b)};
            return fm1_midi_push(runtime, bytes, length, FM1_EVENT_NOW) != 0;
        };
        if (panicRequested.exchange(false)) {
            recovery.request();
            mapper.clear();
        }
        // Stop producing notes during queue recovery. Keep an explicit cursor:
        // a rejected note-off may only be forgotten after all-notes-off and
        // all-sound-off have been accepted for every channel. Bounded work.
        if (!recovery.service(sendRaw)) {
            fm1_set_master(runtime, params[MASTER].getValue());
            auto frame = src.next(args.sampleRate, [&](float* p) { fm1_audio_read(runtime, p, 1); });
            outputs[LEFT].setVoltage(frame[0] * 5.f);
            outputs[RIGHT].setVoltage(frame[1] * 5.f);
            return;
        }
        bool overflow = false;
        auto send = [&](int status, int a, int b, unsigned length = 3) {
            if (!sendRaw(status, a, b, length)) overflow = true;
        };
        const int channel = std::max(0, std::min(15, static_cast<int>(params[MIDI_CHANNEL].getValue()) - 1));
        const int voices = std::min(16, inputs[GATE].getChannels());
        for (int v = 0; v < voices; ++v) {
            float velocity = inputs[VELOCITY].isConnected() ? inputs[VELOCITY].getPolyVoltage(v) : 100.f / 12.7f;
            mapper.update(v, inputs[PITCH].getPolyVoltage(v), inputs[GATE].getPolyVoltage(v), velocity, channel, send);
        }
        mapper.trim(voices, send);
        if (clockEdge.update(inputs[CLOCK].getVoltage())) send(0xf8, 0, 0, 1);
        bool beforeRun = runEdge.high;
        runEdge.update(inputs[RUN].getVoltage());
        if (inputs[RUN].isConnected() && beforeRun != runEdge.high)
            send(runEdge.high ? 0xfa : 0xfc, 0, 0, 1);
        if (resetEdge.update(inputs[RESET].getVoltage())) {
            send(0xfc, 0, 0, 1); send(0xf2, 0, 0, 3);
            if (!inputs[RUN].isConnected() || runEdge.high) send(0xfa, 0, 0, 1);
        }
        // Max 250 CC messages/s at every Rack sample rate, and only on change.
        // DIN also carries notes/clocks; these remain bounded separately.
        if (ccLimiter.update(args.sampleTime)) {
            if (inputs[CC].isConnected()) {
                int value = midiCc(inputs[CC].getVoltage());
                int number = std::max(0, std::min(127, static_cast<int>(params[CC_NUMBER].getValue())));
                if (value != oldCc || number != oldCcNumber || channel != oldCcChannel) {
                    send(0xb0 | channel, number, value);
                    oldCc = value; oldCcNumber = number; oldCcChannel = channel;
                }
            } else oldCc = -1;
        }
        fm1_set_master(runtime, params[MASTER].getValue());
        if (overflow) { recovery.request(); mapper.clear(); }
        auto frame = src.next(args.sampleRate, [&](float* p) { fm1_audio_read(runtime, p, 1); });
        outputs[LEFT].setVoltage(frame[0] * 5.f);
        outputs[RIGHT].setVoltage(frame[1] * 5.f);
    }
};

namespace {
// Palette: dark graphite device body with light silkscreen.
const NVGcolor BODY = nvgRGB(52, 53, 56);
const NVGcolor BEZEL = nvgRGB(36, 37, 40);
const NVGcolor CAP = nvgRGB(30, 31, 33);
const NVGcolor INK = nvgRGB(214, 216, 220);
const NVGcolor DIM = nvgRGB(140, 143, 148);
const NVGcolor LIT = nvgRGB(255, 244, 214);

void text(NVGcontext* vg, float x, float y, float size, const std::string& value, NVGcolor color = INK, int align = NVG_ALIGN_CENTER) {
    nvgFontFaceId(vg, APP->window->uiFont->handle);
    nvgFontSize(vg, size); nvgTextAlign(vg, align | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, color); nvgText(vg, x, y, value.c_str(), nullptr);
}
void rounded(NVGcontext* vg, Rect box, float radius, NVGcolor color) {
    nvgBeginPath(vg); nvgRoundedRect(vg, box.pos.x, box.pos.y, box.size.x, box.size.y, radius);
    nvgFillColor(vg, color); nvgFill(vg);
}
// Soft raised cap: base fill, subtle top highlight, dark outline.
void cap(NVGcontext* vg, Rect r, float radius, NVGcolor color) {
    nvgBeginPath(vg); nvgRoundedRect(vg, r.pos.x, r.pos.y + 1.5f, r.size.x, r.size.y, radius);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 110)); nvgFill(vg);
    nvgBeginPath(vg); nvgRoundedRect(vg, r.pos.x, r.pos.y, r.size.x, r.size.y, radius);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, r.pos.y, 0, r.pos.y + r.size.y, nvgLerpRGBA(color, nvgRGB(255, 255, 255), .08f), color));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(0, 0, 0, 160)); nvgStrokeWidth(vg, 1); nvgStroke(vg);
}

// Stock Rack knob artwork used as a display-only endless encoder.
struct EncoderView : widget::FramebufferWidget {
    widget::TransformWidget* tw;
    widget::SvgWidget* sw;
    float angle = 0;
    EncoderView() {
        auto* bg = new widget::SvgWidget;
        bg->setSvg(Svg::load(asset::system("res/ComponentLibrary/RoundSmallBlackKnob_bg.svg")));
        addChild(bg);
        tw = new widget::TransformWidget; addChild(tw);
        sw = new widget::SvgWidget;
        sw->setSvg(Svg::load(asset::system("res/ComponentLibrary/RoundSmallBlackKnob.svg")));
        tw->addChild(sw);
        box.size = tw->box.size = bg->box.size = sw->box.size;
        rotate(0);
    }
    void rotate(float delta) {
        angle += delta;
        Vec c = sw->box.getCenter();
        tw->identity(); tw->translate(c); tw->rotate(angle); tw->translate(c.neg());
        setDirty();
    }
};

// Panel follows the hardware's control arrangement (no logos or trademarks).
// All firmware controls are matrix events; right-click latches a key/button so
// firmware button combinations remain possible with a single mouse pointer.
// Geometry (panel pixels) is read from the id'd anchors in res/FM1Host.svg.
struct HostPanel : Widget {
    FM1Host* module = nullptr;
    Fm1Status status{};
    std::array<uint16_t, FM1_LCD_WIDTH * FM1_LCD_HEIGHT> pixels{};
    std::array<uint8_t, FM1_LCD_WIDTH * FM1_LCD_HEIGHT * 4> rgba{};
    std::array<bool, FM1_LED_COUNT> latch{};
    std::array<bool, FM1_LED_COUNT> wantedKey{}, pendingKey{};
    int image = -1;
    NVGcontext* imageContext = nullptr;
    bool imageDirty = true;
    unsigned ticks = 0;
    int heldKey = -1, dragEncoder = -1;
    // Computer keyboard while the pointer is over the panel: the same key contacts as clicking.
    // Key contacts 14..40 run chromatically from F; laid out like a piano on the US layout:
    // Z row for the naturals F..A with sharps on the row above, then Q row for B..G with sharps
    // on the number row (B-C and E-F have none, so 2 and 5 are unused).
    struct PcKey { int key; const char* label; };
    static constexpr PcKey PC_KEYS[27] = {
        {GLFW_KEY_Z, "Z"}, {GLFW_KEY_S, "S"}, {GLFW_KEY_X, "X"}, {GLFW_KEY_D, "D"}, {GLFW_KEY_C, "C"},
        {GLFW_KEY_F, "F"}, {GLFW_KEY_V, "V"}, {GLFW_KEY_B, "B"}, {GLFW_KEY_H, "H"}, {GLFW_KEY_N, "N"},
        {GLFW_KEY_J, "J"}, {GLFW_KEY_M, "M"}, {GLFW_KEY_COMMA, ","}, {GLFW_KEY_L, "L"},
        {GLFW_KEY_PERIOD, "."}, {GLFW_KEY_SEMICOLON, ";"}, {GLFW_KEY_SLASH, "/"},
        {GLFW_KEY_APOSTROPHE, "'"}, {GLFW_KEY_Q, "Q"}, {GLFW_KEY_W, "W"}, {GLFW_KEY_3, "3"},
        {GLFW_KEY_E, "E"}, {GLFW_KEY_4, "4"}, {GLFW_KEY_R, "R"}, {GLFW_KEY_T, "T"},
        {GLFW_KEY_6, "6"}, {GLFW_KEY_Y, "Y"}};
    std::array<bool, 27> pcHeld{};
    bool hovered = false;
    // Underrun bursts seen since this firmware generation started (one per status poll that
    // saw the counter move), and when the last one happened.
    uint64_t underrunBase = 0;
    unsigned underrunBursts = 0, lastUnderrunTick = 0;
    float dragAccum = 0;
    // Encoder index -> firmware encoder id, label, and centre.
    const std::array<int, 7> encoderIds{{0, 6, 1, 2, 3, 4, 5}};
    const std::array<const char*, 7> encoderLabels{{"SELECT", "PRESETS", "ALGORITHM", "KNOB1", "KNOB2", "KNOB3", "KNOB4"}};
    std::array<Vec, 7> encoderPos{};
    std::array<float, 7> encoderRadius{};
    std::array<EncoderView*, 7> encoderViews{};
    const std::array<const char*, 14> buttonLabels{{"OCT-", "OCT+", "FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC"}};
    std::array<Rect, 14> buttonRects{};
    // Key contacts 14..40 are chromatic from F: sharps on the top row, naturals below.
    std::array<Rect, 27> keyRects{};
    std::array<bool, 27> keyIsSharp{};
    std::array<const char*, 27> keyLabels{};
    std::array<Vec, 9> jacks{};
    Rect body, screen, octWell, funcWell, keyBed, outWell;
    Vec masterPos;

    static Rect anchorRect(const panel_anchors::Anchors& a, const std::string& id) {
        if (const auto* an = a.find(id)) return Rect(Vec(an->x, an->y), Vec(an->w, an->h));
        WARN("Eidolon panel: missing anchor %s", id.c_str());
        return Rect();
    }
    static Vec anchorPoint(const panel_anchors::Anchors& a, const std::string& id, float* radius = nullptr) {
        if (const auto* an = a.find(id)) { if (radius) *radius = an->r; return Vec(an->cx, an->cy); }
        WARN("Eidolon panel: missing anchor %s", id.c_str());
        return Vec();
    }

    HostPanel() {
        status.abi_version = FM1_ABI_VERSION;
        panel_anchors::Anchors a;
        const std::string path = asset::plugin(pluginInstance, "res/FM1Host.svg");
        if (!a.load(path)) WARN("Eidolon panel: could not read anchors from %s", path.c_str());
        box.size = Vec(a.width > 0 ? a.width : 63 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);
        body = anchorRect(a, "BODY");
        screen = anchorRect(a, "SCREEN");
        octWell = anchorRect(a, "WELL_OCT");
        funcWell = anchorRect(a, "WELL_FUNC");
        keyBed = anchorRect(a, "KEYBED");
        outWell = anchorRect(a, "WELL_OUT");
        masterPos = anchorPoint(a, "MASTER");
        for (int i = 0; i < 7; ++i) encoderPos[i] = anchorPoint(a, std::string("ENC_") + encoderLabels[i], &encoderRadius[i]);
        static const char* buttonIds[14] = {"OCT_DOWN", "OCT_UP", "FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC"};
        for (int i = 0; i < 14; ++i) buttonRects[i] = anchorRect(a, std::string("BTN_") + buttonIds[i]);
        static const char* jackIds[9] = {"VOCT", "GATE", "VEL", "CC", "CLK", "RUN", "RESET", "L", "R"};
        for (int i = 0; i < 9; ++i) jacks[i] = anchorPoint(a, std::string("JACK_") + jackIds[i]);
        static const bool sharp[12] = {false, true, false, true, false, true, false, false, true, false, true, false};
        static const char* topLabels[11] = {"OP1", "OP2", "OP3", "OP4", "OP5", "OP6", "PIT", "GLO", "MONO", "POLY", ""};
        int black = 0;
        for (int s = 0; s < 27; ++s) {
            keyIsSharp[s] = sharp[s % 12];
            keyLabels[s] = keyIsSharp[s] ? topLabels[black++] : "";
            keyRects[s] = anchorRect(a, "KEY_" + std::to_string(s + 14));
        }
        for (int i = 0; i < 7; ++i) {
            auto* view = new EncoderView;
            view->box.pos = encoderPos[i].minus(view->box.size.div(2));
            addChild(view);
            encoderViews[i] = view;
        }
    }
    ~HostPanel() override { if (image >= 0 && imageContext) nvgDeleteImage(imageContext, image); }
    Rect buttonRect(int id) const { return buttonRects[id]; }
    Rect keyRect(int id) const { return keyRects[id - 14]; }
    int keyAt(Vec p) const {
        for (int i = 0; i < 14; ++i) if (buttonRect(i).contains(p)) return i;
        for (int i = 14; i < 41; ++i) if (keyRect(i).contains(p)) return i;
        return -1;
    }
    int encoderAt(Vec p) const {
        for (int i = 0; i < 7; ++i) if (p.minus(encoderPos[i]).norm() <= encoderRadius[i]) return i;
        return -1;
    }
    void turn(int enc, int steps) {
        fm1_encoder(module->runtime, encoderIds[enc], steps);
        encoderViews[enc]->rotate(steps * float(M_PI) / 12.f);
    }
    void key(int id, bool down) {
        wantedKey[id] = down;
        pendingKey[id] = true;
        if (module && module->runtime && fm1_key(module->runtime, id, down)) pendingKey[id] = false;
    }
    void step() override {
        Widget::step();
        if (module && module->runtime)
            for (int i = 0; i < FM1_LED_COUNT; ++i)
                if (pendingKey[i] && fm1_key(module->runtime, i, wantedKey[i])) pendingKey[i] = false;
        if (++ticks % 2 || !module) return;
        const uint64_t oldGeneration = status.generation;
        if (module->readStatus(status)) {
            if (oldGeneration != status.generation) {
                latch.fill(false); wantedKey.fill(false); pendingKey.fill(false);
                underrunBase = status.underrun_frames; underrunBursts = 0;
            } else if (status.underrun_frames > underrunBase) {
                underrunBase = status.underrun_frames; ++underrunBursts; lastUnderrunTick = ticks;
            }
        }
        if (module->runtime && fm1_try_copy_lcd(module->runtime, pixels.data(), pixels.size())) {
            for (size_t i = 0; i < pixels.size(); ++i) {
                uint16_t v = pixels[i];
                rgba[i * 4] = static_cast<uint8_t>((v >> 11) * 255 / 31);
                rgba[i * 4 + 1] = static_cast<uint8_t>(((v >> 5) & 63) * 255 / 63);
                rgba[i * 4 + 2] = static_cast<uint8_t>((v & 31) * 255 / 31);
                rgba[i * 4 + 3] = 255;
            }
            imageDirty = true;
        }
    }
    void drawScreen(NVGcontext* vg) {
        rounded(vg, screen.grow(Vec(7, 7)), 14, nvgRGB(8, 8, 9));
        rounded(vg, screen.grow(Vec(1, 1)), 2, nvgRGB(0, 0, 0));
        if (image >= 0 && imageContext != vg) { nvgDeleteImage(imageContext, image); image = -1; }
        if (image < 0) { image = nvgCreateImageRGBA(vg, 240, 240, 0, rgba.data()); imageContext = vg; imageDirty = false; }
        if (image >= 0 && imageDirty) { nvgUpdateImage(vg, image, rgba.data()); imageDirty = false; }
        const float cx = screen.getCenter().x;
        if (image >= 0 && status.state == FM1_STATUS_RUNNING) {
            nvgBeginPath(vg); nvgRect(vg, screen.pos.x, screen.pos.y, screen.size.x, screen.size.y);
            nvgFillPaint(vg, nvgImagePattern(vg, screen.pos.x, screen.pos.y, screen.size.x, screen.size.y, 0, image, 1)); nvgFill(vg);
        } else {
            const float cy = screen.getCenter().y;
            text(vg, cx, cy - 33, 14, status.state == FM1_STATUS_LOADING ? "BOOTING" : "NO FIRMWARE");
            text(vg, cx, cy - 7, 9, "Right-click: Import firmware", DIM);
            text(vg, cx, cy + 7, 9, "or drop a .fwsc file here", DIM);
            text(vg, cx, cy + 35, 8, "No firmware is included.", DIM);
        }
        const char* state = status.state == FM1_STATUS_RUNNING ? "RUNNING" : status.state == FM1_STATUS_LOADING ? "LOADING" : status.state == FM1_STATUS_HALTED ? "HALTED" : status.state == FM1_STATUS_ERROR ? "ERROR" : "EMPTY";
        std::string line = state;
        if (status.state == FM1_STATUS_RUNNING)
            line += string::f("  %.1fx  %u MHz", status.realtime_speed, status.clock_mhz);
        const float statusX = funcWell.getCenter().x, statusY = funcWell.getBottom() + 18;
        const bool recentUnderrun = underrunBursts && ticks - lastUnderrunTick < 120;  // ~2 s
        if (underrunBursts)
            line += string::f("  %u underrun%s", underrunBursts, underrunBursts == 1 ? "" : "s");
        text(vg, statusX, statusY, 7, line, recentUnderrun ? nvgRGB(240, 110, 90) : DIM);
        std::string message(status.message);
        if (message.size() > 44) message = message.substr(0, 43) + "...";
        text(vg, statusX, statusY + 12, 6.5f, message, DIM);
    }
    void draw(const DrawArgs& args) override {
        auto* vg = args.vg;
        // Module background, then the device body.
        rounded(vg, Rect(Vec(), box.size), 0, BEZEL);
        rounded(vg, body, 22, BODY);
        drawScreen(vg);

        text(vg, masterPos.x, masterPos.y - 32, 8.5f, "MASTER");
        for (int i = 0; i < 7; ++i)
            text(vg, encoderPos[i].x, encoderPos[i].y - 31, 8.5f, encoderLabels[i]);

        // Octave buttons and the 12-button function block sit in recessed wells.
        rounded(vg, octWell, 6, BEZEL);
        rounded(vg, funcWell, 9, BEZEL);
        for (int i = 0; i < 14; ++i) {
            Rect r = buttonRect(i);
            bool active = latch[i] || heldKey == i;
            float glow = std::min(1.f, std::max(active ? .8f : 0.f, status.leds[i]));
            cap(vg, r, 5, nvgLerpRGBA(CAP, nvgRGB(70, 72, 76), glow));
            NVGcolor label = nvgLerpRGBA(INK, LIT, glow);
            if (i == 12) {  // Two-line label like the hardware's PLAY/STOP.
                text(vg, r.getCenter().x, r.getCenter().y - 5, 7.5f, "PLAY", label);
                nvgBeginPath(vg); nvgMoveTo(vg, r.getCenter().x - 10, r.getCenter().y); nvgLineTo(vg, r.getCenter().x + 10, r.getCenter().y);
                nvgStrokeColor(vg, label); nvgStrokeWidth(vg, .7f); nvgStroke(vg);
                text(vg, r.getCenter().x, r.getCenter().y + 5.5f, 7.5f, "STOP", label);
            } else {
                text(vg, r.getCenter().x, r.getCenter().y, 7.5f, buttonLabels[i], label);
            }
        }

        // Key bed.
        rounded(vg, keyBed, 16, nvgRGB(24, 25, 27));
        for (int id = 14; id < 41; ++id) {
            Rect r = keyRect(id);
            bool active = latch[id] || heldKey == id || pcHeld[id - 14];
            float glow = std::min(1.f, std::max(active ? 1.f : 0.f, status.leds[id]));
            cap(vg, r, r.size.x / 2, CAP);
            const bool sharp = keyIsSharp[id - 14];
            const float barTop = r.pos.y + 8, barHeight = sharp ? 13 : 30;
            rounded(vg, Rect(Vec(r.getCenter().x - 1.6f, barTop), Vec(3.2f, barHeight)), 1.6f,
                    nvgLerpRGBA(nvgRGB(205, 207, 210), LIT, glow));
            if (glow > 0) {
                nvgBeginPath(vg); nvgRoundedRect(vg, r.getCenter().x - 4, barTop - 2, 8, barHeight + 4, 4);
                nvgFillColor(vg, nvgRGBAf(1, .9f, .7f, .25f * glow)); nvgFill(vg);
            }
            if (sharp && keyLabels[id - 14][0])
                text(vg, r.getCenter().x, r.pos.y + r.size.y - 6, 6.5f, keyLabels[id - 14]);
            // Computer-keyboard letter in the middle of the key: white, dimmed unless the pointer
            // is over the panel (where the keyboard plays it).
            const Vec c = r.getCenter();
            nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, 7.f);
            nvgFillColor(vg, hovered ? nvgRGB(236, 238, 240) : nvgRGB(120, 123, 128)); nvgFill(vg);
            text(vg, c.x, c.y + .4f, 8.f, PC_KEYS[id - 14].label, nvgRGB(12, 12, 13));
        }

        // Jack block: inputs on the left column, RUN/RESET and outputs on the right.
        static const char* labels[9] = {"V/OCT", "GATE", "VEL", "CC", "CLK", "RUN", "RESET", "L", "R"};
        rounded(vg, outWell, 5, nvgRGB(24, 25, 27));
        for (int i = 0; i < 9; ++i)
            text(vg, jackPos(i).x, jackPos(i).y - 17, 7, labels[i], i >= 7 ? INK : DIM);
        Widget::draw(args);
    }
    Vec jackPos(int i) const { return jacks[i]; }
    void onButton(const event::Button& e) override {
        int id = keyAt(e.pos);
        if (id >= 0 && e.button == GLFW_MOUSE_BUTTON_RIGHT && e.action == GLFW_PRESS) {
            latch[id] = !latch[id]; key(id, latch[id]); e.consume(this); return;
        }
        if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
            if (id >= 0) { heldKey = id; key(id, true); e.consume(this); return; }
            dragEncoder = encoderAt(e.pos);
            if (dragEncoder >= 0) { dragAccum = 0; e.consume(this); return; }
        }
        Widget::onButton(e);
    }
    void onDragEnd(const event::DragEnd& e) override {
        if (heldKey >= 0) {
            const bool byKeyboard = heldKey >= 14 && pcHeld[heldKey - 14];
            key(heldKey, latch[heldKey] || byKeyboard);
            heldKey = -1;
        }
        dragEncoder = -1; dragAccum = 0;
        Widget::onDragEnd(e);
    }
    void onDragMove(const event::DragMove& e) override {
        if (dragEncoder >= 0 && module && module->runtime) {
            dragAccum -= e.mouseDelta.y / 5.f;
            int steps = static_cast<int>(dragAccum);
            if (steps) { turn(dragEncoder, steps); dragAccum -= steps; }
        }
        Widget::onDragMove(e);
    }
    void onHoverKey(const event::HoverKey& e) override {
        if (!(e.mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER))) {
            for (int s = 0; s < 27; ++s) {
                if (PC_KEYS[s].key != e.key) continue;
                const int id = s + 14;
                if (e.action == GLFW_PRESS && !pcHeld[s]) { pcHeld[s] = true; key(id, true); }
                else if (e.action == GLFW_RELEASE && pcHeld[s]) { pcHeld[s] = false; key(id, latch[id] || heldKey == id); }
                e.consume(this);  // (repeats too: a held key must not reach Rack's shortcuts)
                return;
            }
        }
        Widget::onHoverKey(e);
    }
    // The panel takes the hover so it learns when the pointer leaves: key releases after that go
    // to whatever is under the pointer, so keys still held are released here.
    void onHover(const event::Hover& e) override {
        Widget::onHover(e);
        if (!e.isConsumed()) e.consume(this);
    }
    void onEnter(const event::Enter& e) override { hovered = true; }
    void onLeave(const event::Leave& e) override {
        hovered = false;
        for (int s = 0; s < 27; ++s)
            if (pcHeld[s]) { pcHeld[s] = false; key(s + 14, latch[s + 14] || heldKey == s + 14); }
    }
    void onHoverScroll(const event::HoverScroll& e) override {
        int enc = encoderAt(e.pos);
        if (enc >= 0 && module && module->runtime && e.scrollDelta.y != 0) {
            turn(enc, e.scrollDelta.y > 0 ? 1 : -1);
            e.consume(this); return;
        }
        Widget::onHoverScroll(e);
    }
    void onPathDrop(const event::PathDrop& e) override {
        if (module && e.paths.size() == 1) { module->importFirmware(e.paths[0]); e.consume(this); }
    }
    void onContextDestroy(const ContextDestroyEvent& e) override {
        if (image >= 0 && imageContext) nvgDeleteImage(imageContext, image);
        image = -1; imageContext = nullptr;
        Widget::onContextDestroy(e);
    }
};
} // namespace

struct FM1HostWidget : ModuleWidget {
    FM1HostWidget(FM1Host* module) {
        setModule(module);
        HostPanel* panel = new HostPanel;
        box.size = panel->box.size;
        panel->module = module; addChild(panel);
        for (int i = 0; i < FM1Host::NUM_INPUTS; ++i)
            addInput(createInputCentered<PJ301MPort>(panel->jackPos(i), module, i));
        addOutput(createOutputCentered<PJ301MPort>(panel->jackPos(7), module, FM1Host::LEFT));
        addOutput(createOutputCentered<PJ301MPort>(panel->jackPos(8), module, FM1Host::RIGHT));
        addParam(createParamCentered<RoundSmallBlackKnob>(panel->masterPos, module, FM1Host::MASTER));
    }
    void appendContextMenu(Menu* menu) override {
        ModuleWidget::appendContextMenu(menu);
        auto* m = dynamic_cast<FM1Host*>(module);
        if (!m) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("User-supplied firmware only"));
        menu->addChild(createMenuItem("Import firmware (.fwsc)...", "", [m] { m->chooseFirmware(); }));
        menu->addChild(createMenuItem("Save local flash now", "", [m] { if (m->runtime) fm1_request_save(m->runtime); }));
        menu->addChild(createMenuItem("Reboot current firmware", "", [m] { if (m->runtime) fm1_reset(m->runtime); }));
        menu->addChild(createMenuItem("Unload firmware", "", [m] { m->unloadFirmware(); }));
        menu->addChild(createMenuItem("All notes off", "", [m] { m->panicRequested.store(true); }));
        menu->addChild(createSubmenuItem("MIDI channel", string::f("%d", static_cast<int>(m->params[FM1Host::MIDI_CHANNEL].getValue())), [m](Menu* sub) {
            for (int ch = 1; ch <= 16; ++ch)
                sub->addChild(createCheckMenuItem(std::to_string(ch), "", [m, ch] { return static_cast<int>(m->params[FM1Host::MIDI_CHANNEL].getValue()) == ch; }, [m, ch] { m->params[FM1Host::MIDI_CHANNEL].setValue(ch); }));
        }));
        menu->addChild(createSubmenuItem("CC input number", std::to_string(static_cast<int>(m->params[FM1Host::CC_NUMBER].getValue())), [m](Menu* sub) {
            for (int n = 0; n < 128; ++n)
                sub->addChild(createCheckMenuItem(std::to_string(n), "", [m, n] { return static_cast<int>(m->params[FM1Host::CC_NUMBER].getValue()) == n; }, [m, n] { m->params[FM1Host::CC_NUMBER].setValue(n); }));
        }));
        menu->addChild(createSubmenuItem("Emulated CPU", string::f("%u MHz", m->clockMhz.load()), [m](Menu* sub) {
            for (unsigned mhz : {120u, 160u, 240u, 320u, 400u})
                sub->addChild(createCheckMenuItem(string::f("%u MHz", mhz), "", [m, mhz] { return m->clockMhz.load() == mhz; }, [m, mhz] { m->setOptions(mhz, m->leadFrames.load()); }));
        }));
        menu->addChild(createSubmenuItem("Audio lead", string::f("%.1f ms", m->leadFrames.load() / 44.1), [m](Menu* sub) {
            for (unsigned frames : {256u, 512u, 1024u, 2048u, 4096u})
                sub->addChild(createCheckMenuItem(string::f("%.1f ms (%u frames)", frames / 44.1, frames), "", [m, frames] { return m->leadFrames.load() == frames; }, [m, frames] { m->setOptions(m->clockMhz.load(), frames); }));
        }));
        menu->addChild(createMenuLabel("Buttons: left-click holds, right-click latches"));
        menu->addChild(createMenuLabel("Patches never embed firmware or flash data"));
    }
};

Model* modelFM1Host = createModel<FM1Host, FM1HostWidget>("FM1Host");
