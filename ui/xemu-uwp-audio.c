/*
 * Audio output for the Xbox (UWP, Dev Mode) build of xemu: plays the APU's
 * SDL_AudioStream through XAudio2. See xemu-uwp-audio.h.
 *
 * Copyright (c) 2026 Skitzelles (https://github.com/skitzellez/xemu-uwp)
 * Licensed under the GNU GPL, version 2 or later, like the rest of xemu.
 */

#ifdef XEMU_UWP

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>

#include "xemu-uwp-audio.h"

void uwp_log(const char *fmt, ...);

#define SAMPLE_RATE  48000
#define CHANNELS     2
#define FRAME_BYTES  (CHANNELS * 2)
#define CHUNK_FRAMES 1024          /* ~21 ms per buffer */
#define NUM_BUFFERS  8
#define MAX_QUEUED   4             /* keep ~85 ms queued: low latency, no gaps */

typedef HRESULT (WINAPI *XAudio2Create_fn)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);

static SDL_AudioStream *stream;
static IXAudio2SourceVoice *voice;
static Uint8 buffers[NUM_BUFFERS][CHUNK_FRAMES * FRAME_BYTES];

static DWORD WINAPI feeder_thread(LPVOID unused)
{
    int next = 0;
    Uint64 next_report = SDL_GetTicks() + 2000;
    uwp_log("audio: feeder thread started");

    for (;;) {
        XAUDIO2_VOICE_STATE state;
        IXAudio2SourceVoice_GetState(voice, &state, 0);
        if (SDL_GetTicks() >= next_report) {
            uwp_log("audio: stream %d bytes, xaudio queued %u, played %llu samples",
                    SDL_GetAudioStreamAvailable(stream), state.BuffersQueued,
                    (unsigned long long)state.SamplesPlayed);
            next_report += 2000;
        }
        int avail = SDL_GetAudioStreamAvailable(stream);
        if (state.BuffersQueued >= MAX_QUEUED || avail < FRAME_BYTES) {
            SDL_Delay(2);
            continue;
        }
        /* take what is there (up to one chunk): never wait on the APU, which
         * itself waits for the stream to drain */
        int want = avail < (int)sizeof(buffers[0]) ? avail - avail % FRAME_BYTES
                                                  : (int)sizeof(buffers[0]);
        int got = SDL_GetAudioStreamData(stream, buffers[next], want);
        if (got <= 0) {
            SDL_Delay(2);
            continue;
        }
        XAUDIO2_BUFFER buf = { 0 };
        buf.AudioBytes = got;
        buf.pAudioData = buffers[next];
        IXAudio2SourceVoice_SubmitSourceBuffer(voice, &buf, NULL);
        next = (next + 1) % NUM_BUFFERS;
    }
    return 0;
}

SDL_AudioStream *xemu_uwp_audio_start(void)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_S16LE, CHANNELS, SAMPLE_RATE };
    IXAudio2 *xaudio = NULL;
    IXAudio2MasteringVoice *master = NULL;
    HRESULT hr;

    HMODULE lib = LoadLibraryExW(L"xaudio2_9.dll", NULL, 0);
    XAudio2Create_fn create = lib ? (XAudio2Create_fn)(void *)GetProcAddress(lib, "XAudio2Create") : NULL;
    if (!create) {
        uwp_log("audio: xaudio2_9.dll not available (error %lu)", GetLastError());
        return NULL;
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        uwp_log("audio: XAudio2Create failed 0x%08lx", hr);
        return NULL;
    }
    hr = IXAudio2_CreateMasteringVoice(xaudio, &master, XAUDIO2_DEFAULT_CHANNELS,
                                       XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL,
                                       AudioCategory_GameEffects);
    if (FAILED(hr)) {
        uwp_log("audio: CreateMasteringVoice failed 0x%08lx", hr);
        return NULL;
    }
    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = CHANNELS;
    wfx.nSamplesPerSec = SAMPLE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = FRAME_BYTES;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * FRAME_BYTES;
    hr = IXAudio2_CreateSourceVoice(xaudio, &voice, &wfx, 0,
                                    XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        uwp_log("audio: CreateSourceVoice failed 0x%08lx", hr);
        return NULL;
    }
    stream = SDL_CreateAudioStream(&spec, &spec);
    if (!stream) {
        uwp_log("audio: SDL_CreateAudioStream failed: %s", SDL_GetError());
        return NULL;
    }
    IXAudio2SourceVoice_Start(voice, 0, XAUDIO2_COMMIT_NOW);
    /* Win32 thread: SDL_CreateThread needs the caller's _beginthreadex, which
     * SDL3-uwp refuses for code built outside it */
    HANDLE t = CreateThread(NULL, 0, feeder_thread, NULL, 0, NULL);
    if (!t) {
        uwp_log("audio: CreateThread failed (error %lu)", GetLastError());
        return NULL;
    }
    SetThreadPriority(t, THREAD_PRIORITY_HIGHEST);
    CloseHandle(t);
    uwp_log("audio: XAudio2 output running (48 kHz stereo)");
    return stream;
}

#endif /* XEMU_UWP */
