// SPDX-License-Identifier: MIT
// Exercise the real Rust/C ABI without downloading or bundling any firmware.
#include "fm1_rack.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <filesystem>
#include <random>
#include <system_error>

namespace fs = std::filesystem;
static_assert(sizeof(Fm1Status) == 616, "C/Rust ABI status size changed");
static_assert(alignof(Fm1Status) == 8, "C/Rust ABI status alignment changed");
static_assert(offsetof(Fm1Status, firmware_sha256) == 224, "C/Rust ABI field offset changed");

namespace {
// Portable mkdtemp: a fresh, absolute, uniquely named directory in the OS temp dir.
std::string makeTempDirectory() {
    std::error_code error;
    const fs::path base = fs::temp_directory_path(error);
    if (error) return {};
    std::random_device device;
    std::mt19937_64 rng((static_cast<uint64_t>(device()) << 32) ^ device());
    for (int attempt = 0; attempt < 100; ++attempt) {
        char suffix[17];
        std::snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(rng()));
        const fs::path candidate = fs::absolute(base / ("fm1-rack-qa-" + std::string(suffix)), error);
        if (error) return {};
        if (fs::create_directory(candidate, error) && !error) return candidate.generic_string();
    }
    return {};
}

bool waitState(Fm1Handle* handle, uint32_t wanted, Fm1Status& status) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        if (fm1_try_get_status(handle, &status) && status.state == wanted)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void expectSilence(Fm1Handle* handle) {
    std::array<float, 2 * 128> audio;
    audio.fill(123.0f);
    assert(fm1_audio_read(handle, audio.data(), 128) == 0);
    for (float sample : audio) assert(sample == 0.0f);
}

void expectSafeStatus(const Fm1Status& status) {
    assert(status.abi_version == FM1_ABI_VERSION);
    assert(std::memchr(status.firmware_sha256, '\0', sizeof(status.firmware_sha256)));
    assert(std::memchr(status.state_id, '\0', sizeof(status.state_id)));
    assert(std::memchr(status.message, '\0', sizeof(status.message)));
    assert(std::isfinite(status.realtime_speed));
    for (float led : status.leds) assert(std::isfinite(led));
}
}

int main() {
    assert(fm1_abi_version() == FM1_ABI_VERSION);
    assert(fm1_create(nullptr) == nullptr);
    assert(fm1_create("") == nullptr);
    assert(fm1_create("relative-cache-is-not-allowed") == nullptr);
    assert(fm1_generation(nullptr) == 0);
    fm1_destroy(nullptr);

    const std::string root = makeTempDirectory();
    assert(!root.empty());
    assert(fs::path(root).is_absolute());
    auto* handle = fm1_create(root.c_str());
    assert(handle);
    Fm1Status status{};
    assert(waitState(handle, FM1_STATUS_EMPTY, status));
    expectSafeStatus(status);
    assert(status.firmware_sha256[0] == '\0');
    assert(status.state_id[0] == '\0');
    assert(fm1_generation(handle) == status.generation);
    expectSilence(handle);

    std::array<uint16_t, FM1_LCD_WIDTH * FM1_LCD_HEIGHT> lcd{};
    assert(fm1_try_copy_lcd(handle, lcd.data(), 1) == 0);
    assert(fm1_try_copy_lcd(handle, nullptr, lcd.size()) == 0);
    assert(fm1_try_get_status(handle, nullptr) == 0);
    assert(fm1_set_options(handle, 13, 512) == 0);
    assert(fm1_set_options(handle, 240, 0) == 0);
    assert(fm1_set_options(handle, 240, 4097) == 0);
    assert(fm1_key(handle, 999, 1) == 0);
    assert(fm1_encoder(handle, 999, 1) == 0);
    assert(fm1_load_cached(handle, "../../outside", "") == 0);
    const std::string validLookingHash(64, 'a');
    assert(fm1_load_cached(handle, validLookingHash.c_str(), "../../outside") == 0);

    const uint8_t note[] = {0x90, 60, 100};
    assert(fm1_midi_push(handle, nullptr, 3, FM1_EVENT_NOW) == 0);
    assert(fm1_midi_push(handle, note, 0, FM1_EVENT_NOW) == 0);
    assert(fm1_midi_push(handle, note, 4, FM1_EVENT_NOW) == 0);
    // SPSC producer only: flood a bounded queue; this call must never wait.
    for (int i = 0; i < 100000; ++i) {
        int accepted = fm1_midi_push(handle, note, 3, FM1_EVENT_NOW);
        assert(accepted == 0 || accepted == 1);
    }
    fm1_set_master(handle, std::numeric_limits<float>::quiet_NaN());
    fm1_set_master(handle, std::numeric_limits<float>::infinity());
    fm1_set_master(handle, -1.0f);
    fm1_set_master(handle, 1.0f);
    expectSilence(handle);

    assert(fm1_load_path(handle, (root + "/does-not-exist.fwsc").c_str()) == 1);
    assert(waitState(handle, FM1_STATUS_ERROR, status));
    expectSafeStatus(status);
    expectSilence(handle);

    // An invalid package is created only in this temporary directory at run time.
    // It is not firmware and is never part of the source or plugin distribution.
    const std::string invalidPath = root + "/invalid-input.fwsc";
    {
        std::ofstream stream(invalidPath, std::ios::binary);
        const char bytes[] = "not a firmware package";
        stream.write(bytes, sizeof(bytes));
        assert(stream.good());
    }
    assert(fm1_unload(handle) == 1);
    assert(waitState(handle, FM1_STATUS_EMPTY, status));
    assert(fm1_load_path(handle, invalidPath.c_str()) == 1);
    assert(waitState(handle, FM1_STATUS_ERROR, status));
    expectSafeStatus(status);
    assert(status.firmware_sha256[0] == '\0');
    expectSilence(handle);
    assert(fm1_unload(handle) == 1);
    assert(waitState(handle, FM1_STATUS_EMPTY, status));
    expectSilence(handle);
    fm1_destroy(handle);

    // Scoped cleanup: no user data exists in a directory made by this test.
    assert(std::remove(invalidPath.c_str()) == 0);
    std::error_code ignored;
    fs::remove(fs::path(root) / "firmware", ignored);  // Removes only if empty.
    fs::remove(fs::path(root) / "state", ignored);
    std::error_code rootError;
    if (!fs::remove(fs::path(root), rootError) || rootError) {
        std::cerr << "Temporary test directory has unexpected contents: " << root << '\n';
        return 1;
    }
    std::cout << "C ABI smoke: PASS (no firmware loaded or bundled)\n";
}
