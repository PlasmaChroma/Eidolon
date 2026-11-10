// SPDX-License-Identifier: MIT
#include "plugin.hpp"
#include "control.hpp"
#include "fm1_rack.h"
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
    std::atomic<unsigned> clockMhz{240}, leadFrames{512};
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
        unsigned clock = 240, lead = 512;
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
const NVGcolor CYAN = nvgRGB(92, 224, 207);
const NVGcolor INK = nvgRGB(181, 205, 216);

void text(NVGcontext* vg, float x, float y, float size, const std::string& value, NVGcolor color = INK, int align = NVG_ALIGN_CENTER) {
    nvgFontFaceId(vg, APP->window->uiFont->handle);
    nvgFontSize(vg, size); nvgTextAlign(vg, align | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, color); nvgText(vg, x, y, value.c_str(), nullptr);
}
void rounded(NVGcontext* vg, Rect box, float radius, NVGcolor color) {
    nvgBeginPath(vg); nvgRoundedRect(vg, box.pos.x, box.pos.y, box.size.x, box.size.y, radius);
    nvgFillColor(vg, color); nvgFill(vg);
}

struct HostPort : app::PortWidget {
    HostPort() { box.size = Vec(26, 26); }
    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg); nvgCircle(args.vg, 13, 13, 12);
        nvgFillColor(args.vg, nvgRGB(44, 64, 78)); nvgFill(args.vg);
        nvgStrokeWidth(args.vg, 1.5); nvgStrokeColor(args.vg, type == engine::Port::OUTPUT ? CYAN : INK); nvgStroke(args.vg);
        nvgBeginPath(args.vg); nvgCircle(args.vg, 13, 13, 7);
        nvgFillColor(args.vg, nvgRGB(6, 12, 18)); nvgFill(args.vg);
        app::PortWidget::draw(args);
    }
};

struct HostKnob : app::Knob {
    HostKnob() { box.size = Vec(28, 28); minAngle = -2.5f; maxAngle = 2.5f; }
    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg); nvgCircle(args.vg, 14, 14, 12);
        nvgFillColor(args.vg, nvgRGB(42, 64, 77)); nvgFill(args.vg);
        nvgStrokeColor(args.vg, CYAN); nvgStrokeWidth(args.vg, 1); nvgStroke(args.vg);
        float value = getParamQuantity() ? getParamQuantity()->getScaledValue() : .8f;
        float angle = -2.5f + 5.f * value;
        nvgBeginPath(args.vg); nvgMoveTo(args.vg, 14 + std::sin(angle) * 5, 14 - std::cos(angle) * 5);
        nvgLineTo(args.vg, 14 + std::sin(angle) * 10, 14 - std::cos(angle) * 10);
        nvgStrokeColor(args.vg, CYAN); nvgStrokeWidth(args.vg, 2); nvgStroke(args.vg);
        app::Knob::draw(args);
    }
};

// Original compact host layout, not a reproduction of the product's panel.
// All firmware controls are matrix events; right-click latches a key/button so
// firmware button combinations remain possible with a single mouse pointer.
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
    float dragAccum = 0;
    const std::array<int, 7> encoderIds{{0, 6, 1, 2, 3, 4, 5}};
    const std::array<const char*, 7> encoderLabels{{"SELECT", "PRESET", "ALGO", "K1", "K2", "K3", "K4"}};
    const std::array<Vec, 7> encoderPos{{Vec(244, 70), Vec(326, 70), Vec(408, 70), Vec(230, 128), Vec(291, 128), Vec(352, 128), Vec(413, 128)}};
    const std::array<const char*, 14> buttonLabels{{"OCT-", "OCT+", "FX", "SCL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC"}};
    HostPanel() { box.size = Vec(480, 380); status.abi_version = FM1_ABI_VERSION; }
    ~HostPanel() override { if (image >= 0 && imageContext) nvgDeleteImage(imageContext, image); }
    Rect buttonRect(int id) const { return Rect(Vec(214 + (id % 7) * 36, 175 + (id / 7) * 32), Vec(32, 23)); }
    Rect keyRect(int id) const { return Rect(Vec(18 + (id - 14) * 16.5f, 275), Vec(15, 24)); }
    int keyAt(Vec p) const {
        for (int i = 0; i < 14; ++i) if (buttonRect(i).contains(p)) return i;
        for (int i = 14; i < 41; ++i) if (keyRect(i).contains(p)) return i;
        return -1;
    }
    int encoderAt(Vec p) const {
        for (int i = 0; i < 7; ++i) if (p.minus(encoderPos[i]).norm() <= 19.f) return i;
        return -1;
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
        if (module->readStatus(status) && oldGeneration != status.generation) {
            latch.fill(false); wantedKey.fill(false); pendingKey.fill(false);
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
    void draw(const DrawArgs& args) override {
        auto* vg = args.vg;
        rounded(vg, Rect(Vec(), box.size), 0, nvgRGB(17, 29, 40));
        text(vg, 20, 20, 16, "FIRMWARE HOST", CYAN, NVG_ALIGN_LEFT);
        text(vg, 460, 20, 8, "FM-1 RUNTIME / USER-SUPPLIED IMAGES", INK, NVG_ALIGN_RIGHT);
        rounded(vg, Rect(Vec(14, 39), Vec(190, 190)), 5, nvgRGB(4, 10, 15));
        if (image >= 0 && imageContext != vg) { nvgDeleteImage(imageContext, image); image = -1; }
        if (image < 0) { image = nvgCreateImageRGBA(vg, 240, 240, NVG_IMAGE_NEAREST, rgba.data()); imageContext = vg; imageDirty = false; }
        if (image >= 0 && imageDirty) { nvgUpdateImage(vg, image, rgba.data()); imageDirty = false; }
        if (image >= 0 && status.state == FM1_STATUS_RUNNING) {
            nvgBeginPath(vg); nvgRect(vg, 17, 42, 184, 184);
            nvgFillPaint(vg, nvgImagePattern(vg, 17, 42, 184, 184, 0, image, 1)); nvgFill(vg);
        } else {
            text(vg, 109, 109, 13, status.state == FM1_STATUS_LOADING ? "BOOTING" : "NO FIRMWARE", CYAN);
            text(vg, 109, 133, 9, "Right-click: Import firmware");
            text(vg, 109, 150, 9, "or drop a .fwsc file here");
            text(vg, 109, 182, 8, "No firmware is included.");
        }
        for (int i = 0; i < 7; ++i) {
            nvgBeginPath(vg); nvgCircle(vg, encoderPos[i].x, encoderPos[i].y, 17);
            nvgFillColor(vg, nvgRGB(36, 59, 73)); nvgFill(vg);
            nvgStrokeColor(vg, CYAN); nvgStrokeWidth(vg, 1); nvgStroke(vg);
            text(vg, encoderPos[i].x, encoderPos[i].y, 12, "+/-", CYAN);
            text(vg, encoderPos[i].x, encoderPos[i].y + 27, 9, encoderLabels[i]);
        }
        for (int i = 0; i < 14; ++i) {
            Rect r = buttonRect(i);
            bool active = latch[i] || heldKey == i;
            float glow = std::max(active ? .7f : 0.f, status.leds[i]);
            rounded(vg, r, 4, nvgLerpRGBA(nvgRGB(35, 54, 67), CYAN, std::min(1.f, glow)));
            text(vg, r.pos.x + 16, r.pos.y + 12, 8, buttonLabels[i], active ? nvgRGB(7, 26, 32) : INK);
        }
        const char* state = status.state == FM1_STATUS_RUNNING ? "RUNNING" : status.state == FM1_STATUS_LOADING ? "LOADING" : status.state == FM1_STATUS_HALTED ? "HALTED" : status.state == FM1_STATUS_ERROR ? "ERROR" : "EMPTY";
        text(vg, 18, 242, 9, state, CYAN, NVG_ALIGN_LEFT);
        if (status.state == FM1_STATUS_RUNNING)
            text(vg, 76, 242, 9, string::f("%.1fx  |  %u MHz  |  lead %.1f ms", status.realtime_speed, status.clock_mhz, status.lead_frames / 44.1), INK, NVG_ALIGN_LEFT);
        text(vg, 18, 257, 8, std::string(status.message).substr(0, 68), INK, NVG_ALIGN_LEFT);
        text(vg, 438, 270, 8, "MASTER");
        const char* notes[12] = {"F", "F#", "G", "G#", "A", "A#", "B", "C", "C#", "D", "D#", "E"};
        for (int id = 14; id < 41; ++id) {
            Rect r = keyRect(id);
            const char* label = notes[(id - 14) % 12];
            bool black = std::strlen(label) == 2;
            bool active = latch[id] || heldKey == id;
            rounded(vg, r, 3, active ? CYAN : black ? nvgRGB(26, 45, 59) : nvgRGB(69, 87, 99));
            text(vg, r.pos.x + 7.5, r.pos.y + 12, 7, label, active ? nvgRGB(5, 25, 29) : INK);
        }
        const char* labels[9] = {"V/OCT", "GATE", "VEL", "CC", "CLK24", "RUN", "RESET", "L", "R"};
        for (int i = 0; i < 9; ++i) text(vg, 28 + i * 53, 316, 9, labels[i], i >= 7 ? CYAN : INK);
        text(vg, 18, 366, 8, "LOCAL FIRMWARE + FLASH  /  PATCHES CONTAIN REFERENCES ONLY", INK, NVG_ALIGN_LEFT);
        text(vg, 461, 366, 8, "MIT CORE", CYAN, NVG_ALIGN_RIGHT);
        Widget::draw(args);
    }
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
        if (heldKey >= 0) { key(heldKey, latch[heldKey]); heldKey = -1; }
        dragEncoder = -1; dragAccum = 0;
        Widget::onDragEnd(e);
    }
    void onDragMove(const event::DragMove& e) override {
        if (dragEncoder >= 0 && module && module->runtime) {
            dragAccum -= e.mouseDelta.y / 5.f;
            int steps = static_cast<int>(dragAccum);
            if (steps) { fm1_encoder(module->runtime, encoderIds[dragEncoder], steps); dragAccum -= steps; }
        }
        Widget::onDragMove(e);
    }
    void onHoverScroll(const event::HoverScroll& e) override {
        int enc = encoderAt(e.pos);
        if (enc >= 0 && module && module->runtime && e.scrollDelta.y != 0) {
            fm1_encoder(module->runtime, encoderIds[enc], e.scrollDelta.y > 0 ? 1 : -1);
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
        box.size = Vec(32 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);
        HostPanel* panel = new HostPanel;
        panel->module = module; addChild(panel);
        for (int i = 0; i < FM1Host::NUM_INPUTS; ++i)
            addInput(createInputCentered<HostPort>(Vec(28 + i * 53, 340), module, i));
        addOutput(createOutputCentered<HostPort>(Vec(399, 340), module, FM1Host::LEFT));
        addOutput(createOutputCentered<HostPort>(Vec(452, 340), module, FM1Host::RIGHT));
        addParam(createParamCentered<HostKnob>(Vec(438, 247), module, FM1Host::MASTER));
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
