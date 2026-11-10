// SPDX-License-Identifier: MIT
#include "../../src/control.hpp"
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

int main() {
    using namespace firmware_host;
    assert(midiNote(0.f) == 60 && midiNote(1.f) == 72 && midiNote(-1.f) == 48);
    assert(midiNote(100.f) == 127 && midiNote(-100.f) == 0);
    assert(midiNote(std::numeric_limits<float>::infinity()) == 60);
    assert(midiVelocity(0.f) == 1 && midiVelocity(10.f) == 127);
    assert(midiCc(0.f) == 0 && midiCc(5.f) == 64 && midiCc(10.f) == 127);
    assert(isDigest(std::string(64, 'a')) && !isDigest(std::string(64, 'g')));
    assert(!isDigest("../../firmware") && !isStateId("../secret"));
    assert(isStateId(std::string(64, '0')) && !isStateId("") && !isStateId("a/b"));
    Edge e;
    assert(e.update(1.f)); assert(!e.update(.5f)); assert(e.high);
    assert(!e.update(.1f)); assert(!e.high); assert(e.update(3.f));
    std::vector<std::array<int, 3>> messages;
    auto send = [&](int s, int n, int v) { messages.push_back({{s, n, v}}); };
    CvMidi m;
    m.update(0, 0, 5, 10, 1, send);
    assert(messages.size() == 1 && messages[0][0] == 0x91 && messages[0][1] == 60);
    m.update(1, 0, 5, 10, 1, send);
    assert(messages.size() == 1); // shared same-note gate
    m.update(0, 0, 0, 10, 1, send);
    assert(messages.size() == 1);
    m.update(1, 1, 5, 5, 1, send);
    assert(messages.size() == 3 && messages[1][0] == 0x81 && messages[2][1] == 72);
    m.trim(0, send);
    assert(messages.size() == 4 && messages.back()[0] == 0x81);
    m.clear(); assert(m.voices[1].note == -1);
    MidiRecovery recovery;
    recovery.request();
    assert(!recovery.service([](int, int, int) { return false; }));
    assert(recovery.nextMessage == 0);
    int recovered = 0;
    auto accepted = [&](int status, int cc, int value) {
        assert(status == (0xb0 | (recovered / 2)));
        assert(cc == (recovered % 2 ? 120 : 123) && value == 0);
        ++recovered; return true;
    };
    for (int i = 0; i < 7; ++i) assert(!recovery.service(accepted));
    assert(recovered == 28 && recovery.service(accepted) && recovered == 32);
    for (double rate : {44100., 48000., 96000., 192000.}) {
        CcRateLimiter limiter;
        int ticks = 0;
        for (int i = 0; i < static_cast<int>(rate) * 4; ++i)
            if (limiter.update(1. / rate)) ++ticks;
        assert(ticks >= 999 && ticks <= 1000);
    }
    StereoSrc src;
    int reads = 0;
    auto pop = [&](float* b) { ++reads; b[0] = 1.f; b[1] = -.5f; };
    std::array<float, 2> frame{};
    for (int i = 0; i < 48000; ++i) frame = src.next(48000, pop);
    assert(reads >= 44099 && reads <= 44101);
    assert(std::abs(frame[0] - 1.f) < 1e-6 && std::abs(frame[1] + .5f) < 1e-6);
    src.clear(); reads = 0;
    for (int i = 0; i < 96000; ++i) src.next(96000, pop);
    assert(reads >= 44099 && reads <= 44101);
    std::cout << "controls: pitch/velocity/CC, hysteresis, note ownership, state tokens, SRC passed\n";
}
