// SPDX-License-Identifier: Zlib
// SPDX-FileCopyrightText: 1997-2025 Sam Lantinga <slouken@libsdl.org>
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include "SDL_internal.h"
#include <../src/io/SDL_iostream_c.h>

#ifndef HAVE_STDIO_H
#define HAVE_STDIO_H
#endif

#include <stdio.h>
#include <errno.h>

struct SDL_IOStream
{
    SDL_IOStreamInterface iface;
    void *userdata;
    SDL_IOStatus status;
    SDL_PropertiesID props;
};

#if defined(HAVE_STDIO_H) && !defined(SDL_PLATFORM_WINDOWS)

// Functions to read/write stdio file pointers. Not used for windows.

typedef struct IOStreamStdioData
{
    FILE *fp;
    bool autoclose;
    bool regular_file;
} IOStreamStdioData;

#define fseek_off_t long

static Sint64 SDLCALL stdio_seek(void *userdata, Sint64 offset, SDL_IOWhence whence)
{
    IOStreamStdioData *iodata = (IOStreamStdioData *)userdata;
    int stdiowhence;

    switch (whence) {
    case SDL_IO_SEEK_SET:
        stdiowhence = SEEK_SET;
        break;
    case SDL_IO_SEEK_CUR:
        stdiowhence = SEEK_CUR;
        break;
    case SDL_IO_SEEK_END:
        stdiowhence = SEEK_END;
        break;
    default:
        SDL_SetError("Unknown value for 'whence'");
        return -1;
    }

    // don't make a possibly-costly API call for the noop seek from SDL_TellIO
    const bool is_noop = (whence == SDL_IO_SEEK_CUR) && (offset == 0);

    if (is_noop || fseek(iodata->fp, (fseek_off_t)offset, stdiowhence) == 0) {
        const Sint64 pos = ftell(iodata->fp);
        if (pos < 0) {
            SDL_SetError("Couldn't get stream offset: %s", strerror(errno));
            return -1;
        }
        return pos;
    }
    SDL_SetError("Error seeking in datastream: %s", strerror(errno));
    return -1;
}

static size_t SDLCALL stdio_read(void *userdata, void *ptr, size_t size, SDL_IOStatus *status)
{
    IOStreamStdioData *iodata = (IOStreamStdioData *)userdata;
    const size_t bytes = fread(ptr, 1, size, iodata->fp);
    if (bytes == 0 && ferror(iodata->fp)) {
        if (errno == EAGAIN) {
            *status = SDL_IO_STATUS_NOT_READY;
            clearerr(iodata->fp);
        } else {
            SDL_SetError("Error reading from datastream: %s", strerror(errno));
        }
    }
    return bytes;
}

static size_t SDLCALL stdio_write(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status)
{
    IOStreamStdioData *iodata = (IOStreamStdioData *)userdata;
    const size_t bytes = fwrite(ptr, 1, size, iodata->fp);
    if (bytes == 0 && ferror(iodata->fp)) {
        if (errno == EAGAIN) {
            *status = SDL_IO_STATUS_NOT_READY;
            clearerr(iodata->fp);
        } else {
            SDL_SetError("Error writing to datastream: %s", strerror(errno));
        }
    }
    return bytes;
}

static bool SDLCALL stdio_flush(void *userdata, SDL_IOStatus *status)
{
    IOStreamStdioData *iodata = (IOStreamStdioData *)userdata;

    if (fflush(iodata->fp) != 0) {
        if (errno == EAGAIN) {
            *status = SDL_IO_STATUS_NOT_READY;
            return false;
        } else {
            return SDL_SetError("Error flushing datastream: %s", strerror(errno));
        }
    }
    return true;
}

static bool SDLCALL stdio_close(void *userdata)
{
    IOStreamStdioData *iodata = (IOStreamStdioData *)userdata;
    bool status = true;
    if (iodata->autoclose) {
        if (fclose(iodata->fp) != 0) {
            status = SDL_SetError("Error closing datastream: %s", strerror(errno));
        }
    }
    SDL_free(iodata);
    return status;
}

SDL_IOStream *SDL_IOFromFP(FILE *fp, bool autoclose)
{
    IOStreamStdioData *iodata = (IOStreamStdioData *)SDL_calloc(1, sizeof(*iodata));
    if (!iodata) {
        if (autoclose) {
            fclose(fp);
        }
        return NULL;
    }

    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    // There's no stdio_size because SDL_GetIOSize emulates it the same way we'd do it for stdio anyhow.
    iface.seek = stdio_seek;
    iface.read = stdio_read;
    iface.write = stdio_write;
    iface.flush = stdio_flush;
    iface.close = stdio_close;

    iodata->fp = fp;
    iodata->autoclose = autoclose;

    iodata->regular_file = true;

    SDL_IOStream *iostr = SDL_OpenIO(&iface, iodata);
    if (!iostr) {
        iface.close(iodata);
    } else {
        const SDL_PropertiesID props = SDL_GetIOProperties(iostr);
        if (props) {
            SDL_SetPointerProperty(props, SDL_PROP_IOSTREAM_STDIO_FILE_POINTER, fp);
            SDL_SetNumberProperty(props, SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, (int)fp);
        }
    }

    return iostr;
}
#endif // !HAVE_STDIO_H && !defined(SDL_PLATFORM_WINDOWS)

// Functions to read/write memory pointers

typedef struct IOStreamMemData
{
    Uint8 *base;
    Uint8 *here;
    Uint8 *stop;
} IOStreamMemData;

static Sint64 SDLCALL mem_size(void *userdata)
{
    const IOStreamMemData *iodata = (IOStreamMemData *)userdata;
    return (iodata->stop - iodata->base);
}

static Sint64 SDLCALL mem_seek(void *userdata, Sint64 offset, SDL_IOWhence whence)
{
    IOStreamMemData *iodata = (IOStreamMemData *)userdata;
    Uint8 *newpos;

    switch (whence) {
    case SDL_IO_SEEK_SET:
        newpos = iodata->base + offset;
        break;
    case SDL_IO_SEEK_CUR:
        newpos = iodata->here + offset;
        break;
    case SDL_IO_SEEK_END:
        newpos = iodata->stop + offset;
        break;
    default:
        SDL_SetError("Unknown value for 'whence'");
        return -1;
    }
    if (newpos < iodata->base) {
        newpos = iodata->base;
    }
    if (newpos > iodata->stop) {
        newpos = iodata->stop;
    }
    iodata->here = newpos;
    return (Sint64)(iodata->here - iodata->base);
}

static size_t mem_io(void *userdata, void *dst, const void *src, size_t size)
{
    IOStreamMemData *iodata = (IOStreamMemData *)userdata;
    const size_t mem_available = (iodata->stop - iodata->here);
    if (size > mem_available) {
        size = mem_available;
    }
    SDL_memcpy(dst, src, size);
    iodata->here += size;
    return size;
}

static size_t SDLCALL mem_read(void *userdata, void *ptr, size_t size, SDL_IOStatus *status)
{
    IOStreamMemData *iodata = (IOStreamMemData *)userdata;
    return mem_io(userdata, ptr, iodata->here, size);
}

static size_t SDLCALL mem_write(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status)
{
    IOStreamMemData *iodata = (IOStreamMemData *)userdata;
    return mem_io(userdata, iodata->here, ptr, size);
}

static bool SDLCALL mem_close(void *userdata)
{
    SDL_free(userdata);
    return true;
}

// Functions to create SDL_IOStream structures from various data sources

#if defined(HAVE_STDIO_H) && !defined(SDL_PLATFORM_WINDOWS)
static bool IsRegularFileOrPipe(FILE *f)
{
#ifndef NXDK
#ifndef SDL_PLATFORM_EMSCRIPTEN
    struct stat st;
    if (fstat(fileno(f), &st) < 0 || !(S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode))) {
        return false;
    }
#endif // !SDL_PLATFORM_EMSCRIPTEN
#endif

    return true;
}
#endif

SDL_IOStream *SDL_IOFromFile(const char *file, const char *mode)
{
    SDL_IOStream *iostr = NULL;

    if (!file || !*file) {
        SDL_InvalidParamError("file");
        return NULL;
    }
    if (!mode || !*mode) {
        SDL_InvalidParamError("mode");
        return NULL;
    }

#ifdef SDL_PLATFORM_ANDROID
#ifdef HAVE_STDIO_H
    // Try to open the file on the filesystem first
    if (*file == '/') {
        FILE *fp = fopen(file, mode);
        if (fp) {
            if (!IsRegularFileOrPipe(fp)) {
                fclose(fp);
                SDL_SetError("%s is not a regular file or pipe", file);
                return NULL;
            }
            return SDL_IOFromFP(fp, true);
        }
    } else if (SDL_strncmp(file, "content://", 10) == 0) {
        // Try opening content:// URI
        int fd = Android_JNI_OpenFileDescriptor(file, mode);
        if (fd == -1) {
            // SDL error is already set.
            return NULL;
        }

        FILE *fp = fdopen(fd, mode);
        if (!fp) {
            close(fd);
            SDL_SetError("Unable to open file descriptor (%d) from URI %s: %s", fd, file, strerror(errno));
            return NULL;
        }

        return SDL_IOFromFP(fp, true);
    } else {
        // Try opening it from internal storage if it's a relative path
        char *path = NULL;
        SDL_asprintf(&path, "%s/%s", SDL_GetAndroidInternalStoragePath(), file);
        if (path) {
            FILE *fp = fopen(path, mode);
            SDL_free(path);
            if (fp) {
                if (!IsRegularFileOrPipe(fp)) {
                    fclose(fp);
                    SDL_SetError("%s is not a regular file or pipe", path);
                    return NULL;
                }
                return SDL_IOFromFP(fp, true);
            }
        }
    }
#endif // HAVE_STDIO_H

    // Try to open the file from the asset system

    void *iodata = NULL;
    if (!Android_JNI_FileOpen(&iodata, file, mode)) {
        return NULL;
    }

    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.size = Android_JNI_FileSize;
    iface.seek = Android_JNI_FileSeek;
    iface.read = Android_JNI_FileRead;
    iface.write = Android_JNI_FileWrite;
    iface.close = Android_JNI_FileClose;

    iostr = SDL_OpenIO(&iface, iodata);
    if (!iostr) {
        iface.close(iodata);
    } else {
        const SDL_PropertiesID props = SDL_GetIOProperties(iostr);
        if (props) {
            SDL_SetPointerProperty(props, SDL_PROP_IOSTREAM_ANDROID_AASSET_POINTER, iodata);
        }
    }

#elif defined(SDL_PLATFORM_WINDOWS)
    HANDLE handle = windows_file_open(file, mode);
    if (handle != INVALID_HANDLE_VALUE) {
        iostr = SDL_IOFromHandle(handle, mode, true);
    }

#elif defined(HAVE_STDIO_H)
    {
#if defined(SDL_PLATFORM_3DS)
        FILE *fp = N3DS_FileOpen(file, mode);
#else
        FILE *fp = fopen(file, mode);
#endif

        if (!fp) {
            SDL_SetError("Couldn't open %s: %s", file, strerror(errno));
        } else if (!IsRegularFileOrPipe(fp)) {
            fclose(fp);
            fp = NULL;
            SDL_SetError("%s is not a regular file or pipe", file);
        } else {
            iostr = SDL_IOFromFP(fp, true);
        }
    }

#else
    SDL_SetError("SDL not compiled with stdio support");
#endif // !HAVE_STDIO_H

    return iostr;
}

SDL_IOStream *SDL_IOFromMem(void *mem, size_t size)
{
    if (!mem) {
        SDL_InvalidParamError("mem");
        return NULL;
    } else if (!size) {
        SDL_InvalidParamError("size");
        return NULL;
    }

    IOStreamMemData *iodata = (IOStreamMemData *)SDL_calloc(1, sizeof(*iodata));
    if (!iodata) {
        return NULL;
    }

    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.size = mem_size;
    iface.seek = mem_seek;
    iface.read = mem_read;
    iface.write = mem_write;
    iface.close = mem_close;

    iodata->base = (Uint8 *)mem;
    iodata->here = iodata->base;
    iodata->stop = iodata->base + size;

    SDL_IOStream *iostr = SDL_OpenIO(&iface, iodata);
    if (!iostr) {
        SDL_free(iodata);
    } else {
        const SDL_PropertiesID props = SDL_GetIOProperties(iostr);
        if (props) {
            SDL_SetPointerProperty(props, SDL_PROP_IOSTREAM_MEMORY_POINTER, mem);
            SDL_SetNumberProperty(props, SDL_PROP_IOSTREAM_MEMORY_SIZE_NUMBER, size);
        }
    }
    return iostr;
}

SDL_IOStream *SDL_IOFromConstMem(const void *mem, size_t size)
{
    if (!mem) {
        SDL_InvalidParamError("mem");
        return NULL;
    } else if (!size) {
        SDL_InvalidParamError("size");
        return NULL;
    }

    IOStreamMemData *iodata = (IOStreamMemData *)SDL_calloc(1, sizeof(*iodata));
    if (!iodata) {
        return NULL;
    }

    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.size = mem_size;
    iface.seek = mem_seek;
    iface.read = mem_read;
    // leave iface.write as NULL.
    iface.close = mem_close;

    iodata->base = (Uint8 *)mem;
    iodata->here = iodata->base;
    iodata->stop = iodata->base + size;

    SDL_IOStream *iostr = SDL_OpenIO(&iface, iodata);
    if (!iostr) {
        SDL_free(iodata);
    } else {
        const SDL_PropertiesID props = SDL_GetIOProperties(iostr);
        if (props) {
            SDL_SetPointerProperty(props, SDL_PROP_IOSTREAM_MEMORY_POINTER, (void *)mem);
            SDL_SetNumberProperty(props, SDL_PROP_IOSTREAM_MEMORY_SIZE_NUMBER, size);
        }
    }
    return iostr;
}

typedef struct IOStreamDynamicMemData
{
    SDL_IOStream *stream;
    IOStreamMemData data;
    Uint8 *end;
} IOStreamDynamicMemData;

static Sint64 SDLCALL dynamic_mem_size(void *userdata)
{
    IOStreamDynamicMemData *iodata = (IOStreamDynamicMemData *)userdata;
    return mem_size(&iodata->data);
}

static Sint64 SDLCALL dynamic_mem_seek(void *userdata, Sint64 offset, SDL_IOWhence whence)
{
    IOStreamDynamicMemData *iodata = (IOStreamDynamicMemData *)userdata;
    return mem_seek(&iodata->data, offset, whence);
}

static size_t SDLCALL dynamic_mem_read(void *userdata, void *ptr, size_t size, SDL_IOStatus *status)
{
    IOStreamDynamicMemData *iodata = (IOStreamDynamicMemData *)userdata;
    return mem_io(&iodata->data, ptr, iodata->data.here, size);
}

static bool dynamic_mem_realloc(IOStreamDynamicMemData *iodata, size_t size)
{
    size_t chunksize = (size_t)SDL_GetNumberProperty(SDL_GetIOProperties(iodata->stream), SDL_PROP_IOSTREAM_DYNAMIC_CHUNKSIZE_NUMBER, 0);
    if (!chunksize) {
        chunksize = 1024;
    }

    // We're intentionally allocating more memory than needed so it can be null terminated
    size_t chunks = (((iodata->end - iodata->data.base) + size) / chunksize) + 1;
    size_t length = (chunks * chunksize);
    Uint8 *base = (Uint8 *)SDL_realloc(iodata->data.base, length);
    if (!base) {
        return false;
    }

    size_t here_offset = (iodata->data.here - iodata->data.base);
    size_t stop_offset = (iodata->data.stop - iodata->data.base);
    iodata->data.base = base;
    iodata->data.here = base + here_offset;
    iodata->data.stop = base + stop_offset;
    iodata->end = base + length;
    return SDL_SetPointerProperty(SDL_GetIOProperties(iodata->stream), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, base);
}

static size_t SDLCALL dynamic_mem_write(void *userdata, const void *ptr, size_t size, SDL_IOStatus *status)
{
    IOStreamDynamicMemData *iodata = (IOStreamDynamicMemData *)userdata;
    if (size > (size_t)(iodata->data.stop - iodata->data.here)) {
        if (size > (size_t)(iodata->end - iodata->data.here)) {
            if (!dynamic_mem_realloc(iodata, size)) {
                return 0;
            }
        }
        iodata->data.stop = iodata->data.here + size;
    }
    return mem_io(&iodata->data, iodata->data.here, ptr, size);
}

static bool SDLCALL dynamic_mem_close(void *userdata)
{
    const IOStreamDynamicMemData *iodata = (IOStreamDynamicMemData *)userdata;
    void *mem = SDL_GetPointerProperty(SDL_GetIOProperties(iodata->stream), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, NULL);
    if (mem) {
        SDL_free(mem);
    }
    SDL_free(userdata);
    return true;
}

SDL_IOStream *SDL_IOFromDynamicMem(void)
{
    IOStreamDynamicMemData *iodata = (IOStreamDynamicMemData *)SDL_calloc(1, sizeof(*iodata));
    if (!iodata) {
        return NULL;
    }

    SDL_IOStreamInterface iface;
    SDL_INIT_INTERFACE(&iface);
    iface.size = dynamic_mem_size;
    iface.seek = dynamic_mem_seek;
    iface.read = dynamic_mem_read;
    iface.write = dynamic_mem_write;
    iface.close = dynamic_mem_close;

    SDL_IOStream *iostr = SDL_OpenIO(&iface, iodata);
    if (iostr) {
        iodata->stream = iostr;
    } else {
        SDL_free(iodata);
    }
    return iostr;
}

SDL_IOStatus SDL_GetIOStatus(SDL_IOStream *context)
{
    if (!context) {
        SDL_InvalidParamError("context");
        return SDL_IO_STATUS_ERROR;
    }
    return context->status;
}

SDL_IOStream *SDL_OpenIO(const SDL_IOStreamInterface *iface, void *userdata)
{
    if (!iface) {
        SDL_InvalidParamError("iface");
        return NULL;
    }
    if (iface->version < sizeof(*iface)) {
        // Update this to handle older versions of this interface
        SDL_SetError("Invalid interface, should be initialized with SDL_INIT_INTERFACE()");
        return NULL;
    }

    SDL_IOStream *iostr = (SDL_IOStream *)SDL_calloc(1, sizeof(*iostr));
    if (iostr) {
        SDL_copyp(&iostr->iface, iface);
        iostr->userdata = userdata;
    }
    return iostr;
}

bool SDL_CloseIO(SDL_IOStream *iostr)
{
    bool result = true;
    if (iostr) {
        if (iostr->iface.close) {
            result = iostr->iface.close(iostr->userdata);
        }
        SDL_DestroyProperties(iostr->props);
        SDL_free(iostr);
    }
    return result;
}

// Load all the data from an SDL data stream
void *SDL_LoadFile_IO(SDL_IOStream *src, size_t *datasize, bool closeio)
{
    const int FILE_CHUNK_SIZE = 1024;
    Sint64 size, size_total = 0;
    size_t size_read;
    char *data = NULL, *newdata;
    bool loading_chunks = false;

    if (!src) {
        SDL_InvalidParamError("src");
        goto done;
    }

    size = SDL_GetIOSize(src);
    if (size < 0) {
        size = FILE_CHUNK_SIZE;
        loading_chunks = true;
    }
    if (size >= SDL_SIZE_MAX - 1) {
        goto done;
    }
    data = (char *)SDL_malloc((size_t)(size + 1));
    if (!data) {
        goto done;
    }

    size_total = 0;
    for (;;) {
        if (loading_chunks) {
            if ((size_total + FILE_CHUNK_SIZE) > size) {
                size = (size_total + FILE_CHUNK_SIZE);
                if (size >= SDL_SIZE_MAX - 1) {
                    newdata = NULL;
                } else {
                    newdata = (char *)SDL_realloc(data, (size_t)(size + 1));
                }
                if (!newdata) {
                    SDL_free(data);
                    data = NULL;
                    goto done;
                }
                data = newdata;
            }
        }

        size_read = SDL_ReadIO(src, data + size_total, (size_t)(size - size_total));
        if (size_read > 0) {
            size_total += size_read;
            continue;
        } else if (SDL_GetIOStatus(src) == SDL_IO_STATUS_NOT_READY) {
            // Wait for the stream to be ready
            SDL_Delay(1);
            continue;
        }

        // The stream status will remain set for the caller to check
        break;
    }

    data[size_total] = '\0';

done:
    if (datasize) {
        *datasize = (size_t)size_total;
    }
    if (closeio && src) {
        SDL_CloseIO(src);
    }
    return data;
}

void *SDL_LoadFile(const char *file, size_t *datasize)
{
    SDL_IOStream *stream = SDL_IOFromFile(file, "rb");
    if (!stream) {
        if (datasize) {
            *datasize = 0;
        }
        return NULL;
    }
    return SDL_LoadFile_IO(stream, datasize, true);
}

bool SDL_SaveFile_IO(SDL_IOStream *src, const void *data, size_t datasize, bool closeio)
{
    size_t size_written = 0;
    size_t size_total = 0;
    bool success = true;

    if (!src) {
        SDL_InvalidParamError("src");
        goto done;
    }

    if (!data && datasize > 0) {
        SDL_InvalidParamError("data");
        goto done;
    }

    if (datasize > 0) {
        while (size_total < datasize) {
            size_written = SDL_WriteIO(src, ((const char *)data) + size_written, datasize - size_written);

            if (size_written <= 0) {
                if (SDL_GetIOStatus(src) == SDL_IO_STATUS_NOT_READY) {
                    // Wait for the stream to be ready
                    SDL_Delay(1);
                    continue;
                } else {
                    success = false;
                    goto done;
                }
            }

            size_total += size_written;
        }
    }

done:
    if (closeio && src) {
        SDL_CloseIO(src);
    }

    return success;
}

bool SDL_SaveFile(const char *file, const void *data, size_t datasize)
{
    SDL_IOStream *stream = SDL_IOFromFile(file, "wb");
    if (!stream) {
        return false;
    }
    return SDL_SaveFile_IO(stream, data, datasize, true);
}

SDL_PropertiesID SDL_GetIOProperties(SDL_IOStream *context)
{
    if (!context) {
        SDL_InvalidParamError("context");
        return 0;
    }

    if (context->props == 0) {
        context->props = SDL_CreateProperties();
    }
    return context->props;
}

Sint64 SDL_GetIOSize(SDL_IOStream *context)
{
    if (!context) {
        return SDL_InvalidParamError("context");
    }
    if (!context->iface.size) {
        Sint64 pos, size;

        pos = SDL_SeekIO(context, 0, SDL_IO_SEEK_CUR);
        if (pos < 0) {
            return -1;
        }
        size = SDL_SeekIO(context, 0, SDL_IO_SEEK_END);

        SDL_SeekIO(context, pos, SDL_IO_SEEK_SET);
        return size;
    }
    return context->iface.size(context->userdata);
}

Sint64 SDL_SeekIO(SDL_IOStream *context, Sint64 offset, SDL_IOWhence whence)
{
    if (!context) {
        SDL_InvalidParamError("context");
        return -1;
    } else if (!context->iface.seek) {
        SDL_Unsupported();
        return -1;
    }
    return context->iface.seek(context->userdata, offset, whence);
}

Sint64 SDL_TellIO(SDL_IOStream *context)
{
    return SDL_SeekIO(context, 0, SDL_IO_SEEK_CUR);
}

size_t SDL_ReadIO(SDL_IOStream *context, void *ptr, size_t size)
{
    size_t bytes;

    if (!context) {
        SDL_InvalidParamError("context");
        return 0;
    } else if (!context->iface.read) {
        context->status = SDL_IO_STATUS_WRITEONLY;
        SDL_Unsupported();
        return 0;
    }

    context->status = SDL_IO_STATUS_READY;
    SDL_ClearError();

    if (size == 0) {
        return 0;
    }

    bytes = context->iface.read(context->userdata, ptr, size, &context->status);
    if (bytes == 0 && context->status == SDL_IO_STATUS_READY) {
        if (*SDL_GetError()) {
            context->status = SDL_IO_STATUS_ERROR;
        } else {
            context->status = SDL_IO_STATUS_EOF;
        }
    }
    return bytes;
}

size_t SDL_WriteIO(SDL_IOStream *context, const void *ptr, size_t size)
{
    size_t bytes;

    if (!context) {
        SDL_InvalidParamError("context");
        return 0;
    } else if (!context->iface.write) {
        context->status = SDL_IO_STATUS_READONLY;
        SDL_Unsupported();
        return 0;
    }

    context->status = SDL_IO_STATUS_READY;
    SDL_ClearError();

    if (size == 0) {
        return 0;
    }

    bytes = context->iface.write(context->userdata, ptr, size, &context->status);
    if ((bytes == 0) && (context->status == SDL_IO_STATUS_READY)) {
        context->status = SDL_IO_STATUS_ERROR;
    }
    return bytes;
}

size_t SDL_IOprintf(SDL_IOStream *context, SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
{
    va_list ap;
    int size;
    char *string;
    size_t bytes;

    va_start(ap, fmt);
    size = SDL_vasprintf(&string, fmt, ap);
    va_end(ap);
    if (size < 0) {
        return 0;
    }

    bytes = SDL_WriteIO(context, string, (size_t)size);
    SDL_free(string);
    return bytes;
}

size_t SDL_IOvprintf(SDL_IOStream *context, SDL_PRINTF_FORMAT_STRING const char *fmt, va_list ap)
{
    int size;
    char *string;
    size_t bytes;

    size = SDL_vasprintf(&string, fmt, ap);
    if (size < 0) {
        return 0;
    }

    bytes = SDL_WriteIO(context, string, (size_t)size);
    SDL_free(string);
    return bytes;
}

bool SDL_FlushIO(SDL_IOStream *context)
{
    bool result = true;

    if (!context) {
        return SDL_InvalidParamError("context");
    }

    context->status = SDL_IO_STATUS_READY;
    SDL_ClearError();

    if (context->iface.flush) {
        result = context->iface.flush(context->userdata, &context->status);
    }
    if (!result && (context->status == SDL_IO_STATUS_READY)) {
        context->status = SDL_IO_STATUS_ERROR;
    }
    return result;
}

// Functions for dynamically reading and writing endian-specific values

bool SDL_ReadU8(SDL_IOStream *src, Uint8 *value)
{
    Uint8 data = 0;
    bool result = false;

    if (SDL_ReadIO(src, &data, sizeof(data)) == sizeof(data)) {
        result = true;
    }
    if (value) {
        *value = data;
    }
    return result;
}

bool SDL_ReadS8(SDL_IOStream *src, Sint8 *value)
{
    Sint8 data = 0;
    bool result = false;

    if (SDL_ReadIO(src, &data, sizeof(data)) == sizeof(data)) {
        result = true;
    }
    if (value) {
        *value = data;
    }
    return result;
}

bool SDL_ReadU16LE(SDL_IOStream *src, Uint16 *value)
{
    Uint16 data = 0;
    bool result = false;

    if (SDL_ReadIO(src, &data, sizeof(data)) == sizeof(data)) {
        result = true;
    }
    if (value) {
        *value = SDL_Swap16LE(data);
    }
    return result;
}

bool SDL_ReadS16LE(SDL_IOStream *src, Sint16 *value)
{
    return SDL_ReadU16LE(src, (Uint16 *)value);
}

bool SDL_ReadU16BE(SDL_IOStream *src, Uint16 *value)
{
    Uint16 data = 0;
    bool result = false;

    if (SDL_ReadIO(src, &data, sizeof(data)) == sizeof(data)) {
        result = true;
    }
    if (value) {
        *value = SDL_Swap16BE(data);
    }
    return result;
}

bool SDL_ReadS16BE(SDL_IOStream *src, Sint16 *value)
{
    return SDL_ReadU16BE(src, (Uint16 *)value);
}

bool SDL_ReadU32LE(SDL_IOStream *src, Uint32 *value)
{
    Uint32 data = 0;
    bool result = false;

    if (SDL_ReadIO(src, &data, sizeof(data)) == sizeof(data)) {
        result = true;
    }
    if (value) {
        *value = SDL_Swap32LE(data);
    }
    return result;
}

bool SDL_ReadS32LE(SDL_IOStream *src, Sint32 *value)
{
    return SDL_ReadU32LE(src, (Uint32 *)value);
}

bool SDL_ReadU32BE(SDL_IOStream *src, Uint32 *value)
{
    Uint32 data = 0;
    bool result = false;

    if (SDL_ReadIO(src, &data, sizeof(data)) == sizeof(data)) {
        result = true;
    }
    if (value) {
        *value = SDL_Swap32BE(data);
    }
    return result;
}

bool SDL_ReadS32BE(SDL_IOStream *src, Sint32 *value)
{
    return SDL_ReadU32BE(src, (Uint32 *)value);
}

bool SDL_ReadU64LE(SDL_IOStream *src, Uint64 *value)
{
    Uint64 data = 0;
    bool result = false;

    if (SDL_ReadIO(src, &data, sizeof(data)) == sizeof(data)) {
        result = true;
    }
    if (value) {
        *value = SDL_Swap64LE(data);
    }
    return result;
}

bool SDL_ReadS64LE(SDL_IOStream *src, Sint64 *value)
{
    return SDL_ReadU64LE(src, (Uint64 *)value);
}

bool SDL_ReadU64BE(SDL_IOStream *src, Uint64 *value)
{
    Uint64 data = 0;
    bool result = false;

    if (SDL_ReadIO(src, &data, sizeof(data)) == sizeof(data)) {
        result = true;
    }
    if (value) {
        *value = SDL_Swap64BE(data);
    }
    return result;
}

bool SDL_ReadS64BE(SDL_IOStream *src, Sint64 *value)
{
    return SDL_ReadU64BE(src, (Uint64 *)value);
}

bool SDL_WriteU8(SDL_IOStream *dst, Uint8 value)
{
    return (SDL_WriteIO(dst, &value, sizeof(value)) == sizeof(value));
}

bool SDL_WriteS8(SDL_IOStream *dst, Sint8 value)
{
    return (SDL_WriteIO(dst, &value, sizeof(value)) == sizeof(value));
}

bool SDL_WriteU16LE(SDL_IOStream *dst, Uint16 value)
{
    const Uint16 swapped = SDL_Swap16LE(value);
    return (SDL_WriteIO(dst, &swapped, sizeof(swapped)) == sizeof(swapped));
}

bool SDL_WriteS16LE(SDL_IOStream *dst, Sint16 value)
{
    return SDL_WriteU16LE(dst, (Uint16)value);
}

bool SDL_WriteU16BE(SDL_IOStream *dst, Uint16 value)
{
    const Uint16 swapped = SDL_Swap16BE(value);
    return (SDL_WriteIO(dst, &swapped, sizeof(swapped)) == sizeof(swapped));
}

bool SDL_WriteS16BE(SDL_IOStream *dst, Sint16 value)
{
    return SDL_WriteU16BE(dst, (Uint16)value);
}

bool SDL_WriteU32LE(SDL_IOStream *dst, Uint32 value)
{
    const Uint32 swapped = SDL_Swap32LE(value);
    return (SDL_WriteIO(dst, &swapped, sizeof(swapped)) == sizeof(swapped));
}

bool SDL_WriteS32LE(SDL_IOStream *dst, Sint32 value)
{
    return SDL_WriteU32LE(dst, (Uint32)value);
}

bool SDL_WriteU32BE(SDL_IOStream *dst, Uint32 value)
{
    const Uint32 swapped = SDL_Swap32BE(value);
    return (SDL_WriteIO(dst, &swapped, sizeof(swapped)) == sizeof(swapped));
}

bool SDL_WriteS32BE(SDL_IOStream *dst, Sint32 value)
{
    return SDL_WriteU32BE(dst, (Uint32)value);
}

bool SDL_WriteU64LE(SDL_IOStream *dst, Uint64 value)
{
    const Uint64 swapped = SDL_Swap64LE(value);
    return (SDL_WriteIO(dst, &swapped, sizeof(swapped)) == sizeof(swapped));
}

bool SDL_WriteS64LE(SDL_IOStream *dst, Sint64 value)
{
    return SDL_WriteU64LE(dst, (Uint64)value);
}

bool SDL_WriteU64BE(SDL_IOStream *dst, Uint64 value)
{
    const Uint64 swapped = SDL_Swap64BE(value);
    return (SDL_WriteIO(dst, &swapped, sizeof(swapped)) == sizeof(swapped));
}

bool SDL_WriteS64BE(SDL_IOStream *dst, Sint64 value)
{
    return SDL_WriteU64BE(dst, (Uint64)value);
}
