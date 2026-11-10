// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace firmware_host {

inline bool isDigest(const std::string& value) {
    if (value.size() != 64) return false;
    for (char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline bool isStateId(const std::string& value) {
    return isDigest(value);
}

inline int midiNote(float voltage) {
    if (!std::isfinite(voltage)) return 60;
    return std::max(0, std::min(127, static_cast<int>(std::round(
        std::max(-10.f, std::min(10.f, voltage)) * 12.f + 60.f))));
}

inline int midiVelocity(float voltage) {
    if (!std::isfinite(voltage)) return 100;
    return std::max(1, std::min(127, static_cast<int>(std::round(
        std::max(0.f, std::min(10.f, voltage)) * 12.7f))));
}

inline int midiCc(float voltage) {
    if (!std::isfinite(voltage)) return 0;
    return std::max(0, std::min(127, static_cast<int>(std::round(
        std::max(0.f, std::min(10.f, voltage)) * 12.7f))));
}

struct Edge {
    bool high = false;
    bool update(float v) {
        if (!std::isfinite(v)) v = 0.f;
        bool before = high;
        if (v >= 1.f) high = true;
        else if (v <= .1f) high = false;
        return high && !before;
    }
};

// Fixed-size mapper: no heap, locks, or system calls. Duplicate poly voices on
// the same MIDI note share a reference count, so one gate cannot cut off another.
// Pitch is quantized to semitones; a change while gated retriggers that voice.
struct CvMidi {
    struct Voice { Edge gate; int note = -1; int channel = 0; };
    std::array<Voice, 16> voices{};
    std::array<std::array<unsigned, 128>, 16> held{};

    void clear() { voices = {}; held = {}; }

    template <class Send>
    void release(int index, Send&& send) {
        auto& v = voices[index];
        if (v.note >= 0) {
            auto& n = held[v.channel][v.note];
            if (n > 0 && --n == 0) send(0x80 | v.channel, v.note, 0);
            v.note = -1;
        }
    }

    template <class Send>
    void update(int index, float pitch, float gate, float velocity, int channel, Send&& send) {
        auto& v = voices[index];
        v.gate.update(gate);
        channel = std::max(0, std::min(15, channel));
        int note = midiNote(pitch);
        if (v.note >= 0 && (!v.gate.high || note != v.note || channel != v.channel))
            release(index, send);
        if (v.gate.high && v.note < 0) {
            v.note = note;
            v.channel = channel;
            if (held[channel][note]++ == 0) send(0x90 | channel, note, midiVelocity(velocity));
        }
    }

    template <class Send>
    void trim(int channels, Send&& send) {
        for (int i = channels; i < 16; ++i) {
            release(i, send);
            voices[i].gate.high = false;
        }
    }
};

struct MidiRecovery {
    int nextMessage = 32;
    void request() { nextMessage = 0; }
    void clear() { nextMessage = 32; }
    template <class Send>
    bool service(Send&& send) {
        for (int i = 0; i < 4 && nextMessage < 32; ++i) {
            if (!send(0xb0 | (nextMessage / 2), nextMessage % 2 ? 120 : 123, 0)) break;
            ++nextMessage;
        }
        return nextMessage == 32;
    }
};

struct CcRateLimiter {
    double elapsed = 0;
    bool update(double sampleTime) {
        if (!std::isfinite(sampleTime) || sampleTime <= 0) return false;
        elapsed += sampleTime;
        if (elapsed < .004) return false; // 250 Hz regardless of Rack sample rate
        elapsed = std::fmod(elapsed, .004);
        return true;
    }
};

// First-pass linear SRC, matching upstream's plugin approach. It is not a
// bandlimited mastering-quality converter; upgrade separately after parity QA.
struct StereoSrc {
    std::array<float, 2> a{}, b{};
    double phase = 0;
    void clear() { a = {}; b = {}; phase = 0; }
    template <class Pop>
    std::array<float, 2> next(double hostRate, Pop&& pop) {
        double ratio = 44100.0 / std::max(8000.0, hostRate);
        phase += ratio;
        while (phase >= 1.0) {
            a = b;
            pop(b.data());
            for (float& sample : b)
                if (!std::isfinite(sample)) sample = 0.f;
            phase -= 1.0;
        }
        float t = static_cast<float>(phase);
        return {{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t}};
    }
};
} // namespace firmware_host
