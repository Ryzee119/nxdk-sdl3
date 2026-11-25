// SPDX-License-Identifier: Zlib
// SPDX-FileCopyrightText: 1997-2019 Sam Lantinga <slouken@libsdl.org>
// SPDX-FileCopyrightText: 2020 Jannik Vogel
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include "SDL_xboxaudio.h"
#include "SDL_internal.h"

#include <hal/audio.h>
#include <assert.h>
#include <xboxkrnl/xboxkrnl.h>

#ifndef SDL_XBOXAUDIO_BUFFER_COUNT
#define SDL_XBOXAUDIO_BUFFER_COUNT 4
#endif

// Buffers queued on the hardware (LVI - CIV) at which WaitDevice hands out the
// next one. Kept at BUFFER_COUNT - 3 so the buffer refilled is never the one
// the hardware finished last: the controller moves CIV on before the last
// samples of a buffer have been played (seen on 1.6 consoles), and refilling
// that buffer put the start of later sound into the end of it, heard as static.
#define SDL_XBOXAUDIO_REFILL_DEPTH (SDL_XBOXAUDIO_BUFFER_COUNT - 3)

#if (SDL_XBOXAUDIO_BUFFER_COUNT < 4)
#error "SDL_XBOXAUDIO_BUFFER_COUNT must be at least 4"
#endif

typedef struct SDL_PrivateAudioData
{
    void *buffers[SDL_XBOXAUDIO_BUFFER_COUNT];
    int buffer_size;
    int next_buffer;
    bool restart;       // engine was resynced: XAudioPlay after the next queue
    KSEMAPHORE playsem; // Use KSEMAPHORE because we need to post from DPC
} SDL_PrivateAudioData;

static void xbox_audio_callback(void *pac97device, void *data)
{
    (void)pac97device;
    struct SDL_PrivateAudioData *audio_data = (struct SDL_PrivateAudioData *)data;
    KeReleaseSemaphore(&audio_data->playsem, IO_SOUND_INCREMENT, 1, FALSE);
    return;
}

// AC97 PCM-out bus master registers: current and last valid descriptor index.
#define AC97_PO_CIV (*(volatile unsigned char *)0xFEC00114)
#define AC97_PO_LVI (*(volatile unsigned char *)0xFEC00115)
// S/PDIF out bus master: it spools the same buffers as PCM out, at its own pace.
#define AC97_SP_CIV (*(volatile unsigned char *)0xFEC00174)
#define AC97_SP_LVI (*(volatile unsigned char *)0xFEC00175)

static bool XBOXAUDIO_WaitDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *audio_data = (struct SDL_PrivateAudioData *)device->hidden;
    LARGE_INTEGER timeout;
    timeout.QuadPart = -5 * 10000; // 5 ms: re-check the hardware well within one buffer

    // Refill from the hardware queue depth, not from semaphore posts: a DPC
    // that is already queued is not queued again, so completions can merge
    // into one post, and a lost post must not stall playback. Refill once at
    // most REFILL_DEPTH buffers are queued; the buffer handed out next then
    // finished at least two buffers ago (see SDL_XBOXAUDIO_REFILL_DEPTH).
    for (;;) {
        // A buffer is only free once BOTH engines are done with it. S/PDIF
        // can run behind PCM out; refilling on PCM out alone overwrote a
        // buffer S/PDIF was still reading (static on digital/HDMI audio).
        int queued_po = (AC97_PO_LVI - AC97_PO_CIV) & 31;
        int queued_sp = (AC97_SP_LVI - AC97_SP_CIV) & 31;
        int queued = queued_po > queued_sp ? queued_po : queued_sp;
        if (queued > SDL_XBOXAUDIO_BUFFER_COUNT - 1) {
            // More in flight than we ever queue: on an underrun the MCPX does
            // not halt at LVI, it laps stale descriptors (heard as a stuck
            // loop, or silence). Reset the engine and start over, with
            // XAudioResync in nxdk builds that define XAUDIO_HAVE_RESYNC.
#ifdef XAUDIO_HAVE_RESYNC
            XAudioResync();
            audio_data->restart = true;
#endif
            // Without XAudioResync (stock nxdk), keep refilling: LVI catches up.
            break;
        }
        if (queued <= SDL_XBOXAUDIO_REFILL_DEPTH) {
            break;
        }
        KeWaitForSingleObject(&audio_data->playsem, Executive, KernelMode, FALSE, &timeout);
    }
    return true;
}

static bool XBOXAUDIO_OpenDevice(SDL_AudioDevice *device)
{
    SDL_PrivateAudioData *audio_data = (SDL_PrivateAudioData *)SDL_calloc(1, sizeof(SDL_PrivateAudioData));
    if (audio_data == NULL) {
        SDL_SetError("Failed to allocate audio private data");
        return false;
    }

    device->hidden = (struct SDL_PrivateAudioData *)audio_data;
    device->spec.freq = 48000;
    device->spec.format = SDL_AUDIO_S16LE;
    device->spec.channels = 2;

    audio_data->next_buffer = 0;
    audio_data->buffer_size = SDL_GetDefaultSampleFramesFromFreq(device->spec.freq) * SDL_AUDIO_FRAMESIZE(device->spec);

    for (int i = 0; i < SDL_XBOXAUDIO_BUFFER_COUNT; i++) {
        audio_data->buffers[i] = MmAllocateContiguousMemoryEx(audio_data->buffer_size, 0, 0xFFFFFFFF,
                                                              0, PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (audio_data->buffers[i] == NULL) {
            SDL_SetError("Failed to allocate audio buffer");
            for (int j = 0; j < i; j++) {
                if (audio_data->buffers[j]) {
                    MmFreeContiguousMemory(audio_data->buffers[j]);
                }
            }
            SDL_free(audio_data);
            return false;
        }
        SDL_memset(audio_data->buffers[i], 0, audio_data->buffer_size);
    }

    // Posts are only wake-up hints (see WaitDevice), so they can outnumber the
    // waits: give the semaphore a limit it cannot reach.
    KeInitializeSemaphore(&audio_data->playsem, 0, 0x7FFFFFFF);
    XAudioInit(16, 2, xbox_audio_callback, (void *)audio_data);
    XAudioProvideSamples(audio_data->buffers[audio_data->next_buffer++], audio_data->buffer_size, 0);
    XAudioPlay();
    return true;
}

static void XBOXAUDIO_CloseDevice(SDL_AudioDevice *device)
{
    SDL_PrivateAudioData *audio_data = (SDL_PrivateAudioData *)device->hidden;
    // Stop the engines, clear their descriptor bases and disconnect the IRQ, so
    // nothing is left running into the next title (HalQuickReboot does not
    // reset the MCPX). XAudioShutdown is in nxdk builds that define
    // XAUDIO_HAVE_SHUTDOWN; otherwise re-init without a callback, as before.
    XAudioPause();
#ifdef XAUDIO_HAVE_SHUTDOWN
    XAudioShutdown();
#else
    XAudioInit(16, 2, NULL, NULL);
#endif

    for (int i = 0; i < SDL_XBOXAUDIO_BUFFER_COUNT; i++) {
        if (audio_data->buffers[i]) {
            MmFreeContiguousMemory(audio_data->buffers[i]);
        }
    }

    SDL_free(audio_data);
}

static Uint8 *XBOXAUDIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    struct SDL_PrivateAudioData *audio_data = (struct SDL_PrivateAudioData *)device->hidden;
    *buffer_size = audio_data->buffer_size;
    return (Uint8 *)audio_data->buffers[audio_data->next_buffer];
}

static bool XBOXAUDIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buflen)
{
    struct SDL_PrivateAudioData *audio_data = (struct SDL_PrivateAudioData *)device->hidden;
    XAudioProvideSamples((unsigned char *)buffer, buflen, 0);
    audio_data->next_buffer = (audio_data->next_buffer + 1) % SDL_XBOXAUDIO_BUFFER_COUNT;
    if (audio_data->restart) {
        audio_data->restart = false;
        XAudioPlay();
    }
    return true;
}

static int XBOXAUDIO_RecordDevice(SDL_AudioDevice *device, void *buffer, int buflen)
{
    (void)device;
    (void)buffer;
    (void)buflen;
    return -1;
}

static bool XBOXAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = XBOXAUDIO_OpenDevice;
    impl->CloseDevice = XBOXAUDIO_CloseDevice;
    impl->WaitDevice = XBOXAUDIO_WaitDevice;
    impl->GetDeviceBuf = XBOXAUDIO_GetDeviceBuf;
    impl->WaitRecordingDevice = XBOXAUDIO_WaitDevice;
    impl->PlayDevice = XBOXAUDIO_PlayDevice;
    impl->RecordDevice = XBOXAUDIO_RecordDevice;

    impl->OnlyHasDefaultPlaybackDevice = true;
    impl->HasRecordingSupport = false;

    return true;
}

AudioBootStrap XBOXAUDIO_bootstrap = {
    .name = "nxdk_audio",
    .desc = "SDL nxdk audio driver",
    .init = XBOXAUDIO_Init,
    .demand_only = false,
    .is_preferred = true
};
