// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025-2026 Ryan Wendland

#include "SDL_xboxaudio.h"
#include "SDL_internal.h"

#include <nxaudio.h>
#include <assert.h>
#include <xboxkrnl/xboxkrnl.h>

#ifndef SDL_XBOXAUDIO_BUFFER_COUNT
#define SDL_XBOXAUDIO_BUFFER_COUNT 2
#endif

typedef struct SDL_PrivateAudioData
{
    nxAudioVoice voice;
    nxAudioBuffer buffers[SDL_XBOXAUDIO_BUFFER_COUNT];
    void *buffer_mem[SDL_XBOXAUDIO_BUFFER_COUNT];
    int buffer_size;
    int next_buffer;
    KSEMAPHORE playsem;
} SDL_PrivateAudioData;

static void xbox_audio_callback(struct nxAudioVoice *voice, void *user_context)
{
    (void)voice;
    SDL_PrivateAudioData *audio_data = (SDL_PrivateAudioData *)user_context;
    KeReleaseSemaphore(&audio_data->playsem, IO_SOUND_INCREMENT, 1, FALSE);
}

static bool XBOXAUDIO_WaitDevice(SDL_AudioDevice *device)
{
    LARGE_INTEGER timeout;
    timeout.QuadPart = -5000 * 100; // 50ms

    SDL_PrivateAudioData *audio_data = (SDL_PrivateAudioData *)device->hidden;
    if (KeWaitForSingleObject(&audio_data->playsem, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
        SDL_memset(audio_data->buffer_mem[audio_data->next_buffer], 0, audio_data->buffer_size);
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

    // Validate / negotiate channels (1, 2, 4, 6)
    if (device->spec.channels != 1 && device->spec.channels != 2 &&
        device->spec.channels != 4 && device->spec.channels != 6) {
        device->spec.channels = 2;
    }

    // Validate / negotiate sample rate (hardware handles any rate, default 48000 if not specified)
    if (device->spec.freq <= 0) {
        device->spec.freq = 48000;
    }

    // Validate / negotiate format (U8, S16LE, S32LE)
    uint8_t bytes_per_sample = 2;
    switch (device->spec.format) {
        case SDL_AUDIO_U8:
            bytes_per_sample = 1;
            break;
        case SDL_AUDIO_S16LE:
            bytes_per_sample = 2;
            break;
        case SDL_AUDIO_S32LE:
            bytes_per_sample = 4;
            break;
        default:
            // Fall back to 16-bit signed LE (SDL3 handles automatic conversion)
            device->spec.format = SDL_AUDIO_S16LE;
            bytes_per_sample = 2;
            break;
    }

    audio_data->next_buffer = 0;
    audio_data->buffer_size = SDL_GetDefaultSampleFramesFromFreq(device->spec.freq) * SDL_AUDIO_FRAMESIZE(device->spec);

    nxAudioInitParams init_params = {0};
    if (!nxAudioInit(&init_params)) {
        SDL_SetError("nxAudioInit failed");
        SDL_free(audio_data);
        return false;
    }

    nxAudioFormat format = {
        .sample_rate = (uint32_t)device->spec.freq,
        .channels = (uint8_t)device->spec.channels,
        .bytes_per_sample = bytes_per_sample,
        .codec = NX_AUDIO_CODEC_PCM,
        .type = NX_VOICE_TYPE_2D_STREAM,
    };

    if (!nxAudioVoiceCreate(&audio_data->voice, &format)) {
        SDL_SetError("nxAudioVoiceCreate failed");
        nxAudioShutdown();
        SDL_free(audio_data);
        return false;
    }

    nxAudioVoiceSetMasterGain(&audio_data->voice, 1.0f);

    for (int i = 0; i < SDL_XBOXAUDIO_BUFFER_COUNT; i++) {
        audio_data->buffer_mem[i] = MmAllocateContiguousMemoryEx(audio_data->buffer_size, 0, 0xFFFFFFFF,
                                                                 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (audio_data->buffer_mem[i] == NULL) {
            SDL_SetError("Failed to allocate audio buffer");
            for (int j = 0; j < i; j++) {
                if (audio_data->buffer_mem[j]) {
                    MmFreeContiguousMemory(audio_data->buffer_mem[j]);
                }
            }
            nxAudioVoiceDestroy(&audio_data->voice);
            nxAudioShutdown();
            SDL_free(audio_data);
            return false;
        }
        SDL_memset(audio_data->buffer_mem[i], 0, audio_data->buffer_size);
        nxAudioBufferInitialize(&audio_data->buffers[i], audio_data->buffer_mem[i], audio_data->buffer_size);
    }

    KeInitializeSemaphore(&audio_data->playsem, 1, SDL_XBOXAUDIO_BUFFER_COUNT);
    nxAudioBufferSetCallback(&audio_data->voice, xbox_audio_callback, (void *)audio_data);

    // Prime the voice with the initial buffer
    nxAudioBufferQueue(&audio_data->voice, &audio_data->buffers[audio_data->next_buffer++]);
    audio_data->next_buffer %= SDL_XBOXAUDIO_BUFFER_COUNT;

    nxAudioVoiceStart(&audio_data->voice);
    return true;
}

static void XBOXAUDIO_CloseDevice(SDL_AudioDevice *device)
{
    SDL_PrivateAudioData *audio_data = (SDL_PrivateAudioData *)device->hidden;
    if (audio_data) {
        nxAudioVoiceStop(&audio_data->voice);
        nxAudioVoiceDestroy(&audio_data->voice);
        nxAudioShutdown();

        for (int i = 0; i < SDL_XBOXAUDIO_BUFFER_COUNT; i++) {
            if (audio_data->buffer_mem[i]) {
                MmFreeContiguousMemory(audio_data->buffer_mem[i]);
            }
        }

        SDL_free(audio_data);
    }
}

static Uint8 *XBOXAUDIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    SDL_PrivateAudioData *audio_data = (SDL_PrivateAudioData *)device->hidden;
    *buffer_size = audio_data->buffer_size;
    return (Uint8 *)audio_data->buffer_mem[audio_data->next_buffer];
}

static bool XBOXAUDIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buflen)
{
    SDL_PrivateAudioData *audio_data = (SDL_PrivateAudioData *)device->hidden;
    int idx = audio_data->next_buffer;
    nxAudioBufferInitialize(&audio_data->buffers[idx], audio_data->buffer_mem[idx], buflen);
    nxAudioBufferQueue(&audio_data->voice, &audio_data->buffers[idx]);
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
