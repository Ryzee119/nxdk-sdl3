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
#define SDL_XBOXAUDIO_BUFFER_COUNT 3
#endif

#if (SDL_XBOXAUDIO_BUFFER_COUNT < 2)
#error "SDL_XBOXAUDIO_BUFFER_COUNT must be at least 2"
#endif

typedef struct SDL_PrivateAudioData
{
    void *buffers[SDL_XBOXAUDIO_BUFFER_COUNT];
    int buffer_size;
    int next_buffer;
    KSEMAPHORE playsem; // Use KSEMAPHORE because we need to post from DPC
} SDL_PrivateAudioData;

static void xbox_audio_callback(void *pac97device, void *data)
{
    (void)pac97device;
    struct SDL_PrivateAudioData *audio_data = (struct SDL_PrivateAudioData *)data;
    KeReleaseSemaphore(&audio_data->playsem, IO_SOUND_INCREMENT, 1, FALSE);
    return;
}

static bool XBOXAUDIO_WaitDevice(SDL_AudioDevice *device)
{
    LARGE_INTEGER timeout;
    timeout.QuadPart = -5000 * 100;

    struct SDL_PrivateAudioData *audio_data = (struct SDL_PrivateAudioData *)device->hidden;
    if (KeWaitForSingleObject(&audio_data->playsem, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
        DbgPrint("XBOXAUDIO_WaitDevice: Timeout waiting for audio buffer\n");
        //assert(0);
        SDL_memset(audio_data->buffers[audio_data->next_buffer], 0, audio_data->buffer_size);
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

    KeInitializeSemaphore(&audio_data->playsem, 1, SDL_XBOXAUDIO_BUFFER_COUNT);
    XAudioInit(16, 2, xbox_audio_callback, (void *)audio_data);
    XAudioProvideSamples(audio_data->buffers[audio_data->next_buffer++], audio_data->buffer_size, 0);
    XAudioPlay();
    return true;
}

static void XBOXAUDIO_CloseDevice(SDL_AudioDevice *device)
{
    SDL_PrivateAudioData *audio_data = (SDL_PrivateAudioData *)device->hidden;
    XAudioPause();
    XAudioInit(16, 2, NULL, NULL);

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
