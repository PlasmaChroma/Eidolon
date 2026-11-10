/* SPDX-License-Identifier: MIT
 * Firmware-free FM-1 emulator bridge. ABI version 1.
 * All functions except audio_read, midi_push, audio_frame, generation and set_master are
 * NON-REALTIME. The caller must stop all calls before destroying a handle.
 * Exactly one DSP thread consumes audio and produces MIDI for each handle.
 * UI/control calls may be made concurrently with that DSP thread.
 */
#ifndef FM1_RACK_H
#define FM1_RACK_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct Fm1Handle Fm1Handle;
enum { FM1_ABI_VERSION = 1, FM1_LCD_WIDTH = 240, FM1_LCD_HEIGHT = 240,
       FM1_LED_COUNT = 41, FM1_STATUS_EMPTY = 0, FM1_STATUS_LOADING = 1,
       FM1_STATUS_RUNNING = 2, FM1_STATUS_ERROR = 3, FM1_STATUS_HALTED = 4 };
#define FM1_NATIVE_RATE 44100.0
#define FM1_EVENT_NOW UINT64_MAX

typedef struct Fm1Status {
    uint32_t abi_version;
    uint32_t state;
    uint32_t lead_frames;
    uint32_t clock_mhz;
    uint64_t generation;
    uint64_t audio_frame;
    uint64_t underrun_frames;
    uint64_t dropped_events;
    uint64_t lcd_revision;
    float realtime_speed;
    float leds[FM1_LED_COUNT];
    char firmware_sha256[65];
    char state_id[65];
    char message[256];
} Fm1Status;

uint32_t fm1_abi_version(void);
/* data_root must be a UTF-8 absolute local path. No networking occurs. */
Fm1Handle* fm1_create(const char* data_root);
void fm1_destroy(Fm1Handle* handle);
/* Asynchronous requests: 1 = queued, 0 = invalid/full; inspect status later. */
int32_t fm1_load_path(Fm1Handle* handle, const char* user_fwsc_path);
/* A cached load forks state into a new local session, preventing duplicated
 * modules from overwriting the same flash file. Empty state_id = fresh boot. */
int32_t fm1_load_cached(Fm1Handle* handle, const char* sha256, const char* state_id);
int32_t fm1_unload(Fm1Handle* handle);
int32_t fm1_reset(Fm1Handle* handle);
int32_t fm1_request_save(Fm1Handle* handle);
/* lead: 256..4096 native frames; clock: 120,160,240,320,400 MHz. */
int32_t fm1_set_options(Fm1Handle* handle, uint32_t clock_mhz, uint32_t lead_frames);
/* Desired key matrix state, coalesced independently from the command queue.
 * A valid key release is never dropped because another request is pending.
 * A press and release wholly within a long import may be coalesced. */
int32_t fm1_key(Fm1Handle* handle, uint32_t id, int32_t down);
int32_t fm1_encoder(Fm1Handle* handle, uint32_t encoder, int32_t detents);
/* Master ADC 0..1; lock-free and safe from DSP or UI (last writer wins). */
void fm1_set_master(Fm1Handle* handle, float value);

/* DSP ONLY. Nonblocking, bounded, no allocation. len is 1..3 MIDI bytes.
 * Native-frame timestamps; FM1_EVENT_NOW stamps current audio_frame+lead.
 * MIDI bytes enter the emulated 31250 baud DIN UART, not an unlimited pipe. */
int32_t fm1_midi_push(Fm1Handle* handle, const uint8_t* bytes, uint32_t len, uint64_t at_frame);
uint64_t fm1_audio_frame(Fm1Handle* handle);
uint64_t fm1_generation(Fm1Handle* handle);
/* Writes all requested interleaved stereo frames (silence on underrun).
 * Returns the number of real frames copied. Native rate is 44.1 kHz nominal. */
uint32_t fm1_audio_read(Fm1Handle* handle, float* stereo, uint32_t frames);

/* UI ONLY. Nonblocking snapshots; 0 means busy/invalid, retain previous view.
 * LCD contains RGB565 pixels. No borrowed pointers cross the ABI.
 * Firmware/flash bytes and local paths never appear in these structures. */
int32_t fm1_try_get_status(Fm1Handle* handle, Fm1Status* out);
int32_t fm1_try_copy_lcd(Fm1Handle* handle, uint16_t* pixels, uint32_t pixel_capacity);

#ifdef __cplusplus
}
#endif
#endif
