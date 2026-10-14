// Xbox Dev Mode (UWP) port of xemu: Copyright (c) 2026 Skitzelles, https://github.com/skitzellez/xemu-uwp
// Free software under the GNU General Public License version 2 or later, like xemu itself;
// this notice stays in every copy.
/*
 * Audio output for the Xbox (UWP, Dev Mode) build of xemu.
 *
 * SDL3-uwp's WASAPI backend never finishes opening a device on the Xbox
 * (ActivateAudioInterfaceAsync does not complete while SDL waits for it), so
 * the APU still writes into an SDL_AudioStream, and a small thread plays that
 * stream through XAudio2 instead.
 */

#ifndef XEMU_UWP_AUDIO_H
#define XEMU_UWP_AUDIO_H

#include <stdbool.h>
#include <SDL3/SDL.h>

/* Creates the stream the APU writes to (48 kHz, signed 16-bit, stereo) and
 * starts playing it. Returns NULL if XAudio2 is unavailable. */
SDL_AudioStream *xemu_uwp_audio_start(void);

#endif
