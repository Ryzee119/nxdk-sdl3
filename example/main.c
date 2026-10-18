// SPDX-License-Identifier: MIT
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(NXDK) || defined(__XBOX__) || defined(_WIN32)
#define PATH_SEP "\\"
#else
#define PATH_SEP "/"
#endif

#define SCREEN_W 640
#define SCREEN_H 480

// Deterministic frame where all builds automatically pause for pixel-perfect
// comparison
#define AUTO_PAUSE_FRAME 60

#define MAX_SPRITE_BATCH 1500
#define LARGE_PRIM_COUNT 360
#define ZONE_COUNT 9
#define FORMAT_COUNT 9

typedef struct Sprite {
  float x, y;
  float vx, vy;
  SDL_FColor color;
} Sprite;

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Gamepad *gamepad = NULL;
static SDL_Texture *image_texture = NULL;
static SDL_Texture *text_texture = NULL;
static MIX_Mixer *mixer = NULL;
static MIX_Track *track = NULL;
static MIX_Audio *audio = NULL;

// Stress-test textures
static SDL_Texture *tex_pot_swizzled = NULL;
static SDL_Texture *tex_npot_static = NULL;
static SDL_Texture *tex_tiny_1x1 = NULL;
static SDL_Texture *tex_streaming = NULL;
static SDL_Texture *tex_render_target = NULL;
static SDL_Texture *tex_pip_backbuffer = NULL;
static SDL_Texture *tex_pip_rendertarget = NULL;
static SDL_Texture *tex_formats[FORMAT_COUNT] = {NULL};

static const SDL_PixelFormat test_format_list[FORMAT_COUNT] = {
    SDL_PIXELFORMAT_ARGB8888, SDL_PIXELFORMAT_XRGB8888,
    SDL_PIXELFORMAT_RGBA8888, SDL_PIXELFORMAT_ABGR8888,
    SDL_PIXELFORMAT_BGRA8888, SDL_PIXELFORMAT_RGB565,
    SDL_PIXELFORMAT_ARGB4444, SDL_PIXELFORMAT_ARGB1555,
    SDL_PIXELFORMAT_XRGB1555};

static const char *zone_names[ZONE_COUNT] = {
    "0:BG SPRITES",   "1:9 FORMATS/NPOT", "2:STREAM/WRAP",
    "3:TARGET/FLIPS", "4:7 BLEND MODES",  "5:MESH >255/IDX",
    "6:LINES/SCALE",  "7:VIEWPORT/CLIP",  "8:READPIXELS PIP"};

static const SDL_FRect zone_bounds[ZONE_COUNT] = {
    {2.0f, 2.0f, 636.0f, 476.0f},     // 0: Background Sprites
    {6.0f, 6.0f, 196.0f, 120.0f},     // 1: 9 Formats / NPOT / 1x1
    {216.0f, 6.0f, 210.0f, 120.0f},   // 2: Streaming & UV Wrap
    {446.0f, 6.0f, 184.0f, 132.0f},   // 3: Render Target & Flips
    {6.0f, 140.0f, 196.0f, 92.0f},    // 4: All 7 Blend Modes
    {210.0f, 140.0f, 216.0f, 132.0f}, // 5: Mesh >255 & Idx8/32
    {446.0f, 140.0f, 184.0f, 132.0f}, // 6: 360 Lines/Points & Scale
    {6.0f, 280.0f, 208.0f, 175.0f},   // 7: Viewport & Clip
    {446.0f, 280.0f, 184.0f,
     102.0f} // 8: Split ReadPixels PIP (Target + Backbuffer)
};

static bool zone_enabled[ZONE_COUNT] = {true, true, true, true, true,
                                        true, true, true, true};

static int selected_zone = 0;
static const int sprite_presets[4] = {0, 200, 600, 1500};
static int sprite_preset_idx = 2; // Default: 600 sprites
static bool paused = false;
static bool step_one_frame = false;
static bool trigger_overflow_test = false;
static bool trigger_destroy_test = false;
static bool force_readpixels = false;
static char status_line[64] = "RUNNING TO FRAME 60 FOR PIXEL COMPARE...";
static int overflow_recovery_frames = 0;

static Sprite sprites[MAX_SPRITE_BATCH];

static bool resolve_asset_path(char *out_path, size_t out_size,
                               const char *filename) {
  const char *base = SDL_GetBasePath();
  if (base) {
    SDL_snprintf(out_path, out_size, "%sassets" PATH_SEP "%s", base, filename);
    SDL_IOStream *io = SDL_IOFromFile(out_path, "rb");
    if (io) {
      SDL_CloseIO(io);
      return true;
    }
  }
  SDL_snprintf(out_path, out_size, "assets" PATH_SEP "%s", filename);
  return true;
}

static SDL_Texture *create_checker_texture(SDL_Renderer *r, int w, int h,
                                           SDL_PixelFormat fmt,
                                           SDL_TextureAccess access,
                                           uint32_t c1_argb, uint32_t c2_argb) {
  SDL_Texture *tex = SDL_CreateTexture(r, fmt, access, w, h);
  if (!tex) {
    SDL_Log("Failed to create texture (%s): %s", SDL_GetPixelFormatName(fmt),
            SDL_GetError());
    return NULL;
  }

  uint32_t *src_argb = (uint32_t *)SDL_malloc((size_t)w * h * sizeof(uint32_t));
  void *dst_pixels = SDL_malloc((size_t)w * h * SDL_BYTESPERPIXEL(fmt));
  if (src_argb && dst_pixels) {
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        bool parity = (((x / 8) + (y / 8)) & 1) != 0;
        src_argb[y * w + x] = parity ? c1_argb : c2_argb;
      }
    }
    int dst_pitch = w * SDL_BYTESPERPIXEL(fmt);
    SDL_ConvertPixels(w, h, SDL_PIXELFORMAT_ARGB8888, src_argb, w * 4, fmt,
                      dst_pixels, dst_pitch);
    SDL_UpdateTexture(tex, NULL, dst_pixels, dst_pitch);
  }
  SDL_free(src_argb);
  SDL_free(dst_pixels);
  return tex;
}

static void update_streaming_and_swizzled(uint32_t sim_frame) {
  if (zone_enabled[2] && tex_streaming) {
    const SDL_Rect lock_rect = {8, 8, 48, 48};
    void *locked_pixels = NULL;
    int locked_pitch = 0;
    if (SDL_LockTexture(tex_streaming, &lock_rect, &locked_pixels,
                        &locked_pitch)) {
      for (int y = 0; y < lock_rect.h; ++y) {
        uint32_t *row =
            (uint32_t *)((uint8_t *)locked_pixels + y * locked_pitch);
        for (int x = 0; x < lock_rect.w; ++x) {
          uint8_t r = (uint8_t)((x * 5 + sim_frame * 3) & 0xFF);
          uint8_t g = (uint8_t)((y * 5 + sim_frame * 2) & 0xFF);
          uint8_t b = (uint8_t)(((x ^ y) * 4 - sim_frame * 4) & 0xFF);
          row[x] = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
      }
      SDL_UnlockTexture(tex_streaming);
    }
  }

  if (zone_enabled[2] && tex_pot_swizzled && (sim_frame % 10 == 0)) {
    uint32_t patch[16 * 16];
    uint32_t step = (sim_frame / 10) % 6;
    uint32_t patch_color = (step & 1) ? 0xFFFFFF00u : 0xFF00FF88u;
    for (int i = 0; i < 16 * 16; ++i) {
      patch[i] = patch_color;
    }
    const SDL_Rect sub_rect = {(int)(step * 8), 24, 16, 16};
    SDL_UpdateTexture(tex_pot_swizzled, &sub_rect, patch,
                      16 * sizeof(uint32_t));
  }
}

static void init_stress_resources(void) {
  tex_pot_swizzled =
      create_checker_texture(renderer, 64, 64, SDL_PIXELFORMAT_ARGB8888,
                             SDL_TEXTUREACCESS_STATIC, 0xFFFF3366, 0xFF33CCFF);
  tex_npot_static =
      create_checker_texture(renderer, 100, 60, SDL_PIXELFORMAT_ARGB8888,
                             SDL_TEXTUREACCESS_STATIC, 0xFF66FF33, 0xFF224422);

  uint32_t white_pixel = 0xFFFFFFFF;
  tex_tiny_1x1 = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STATIC, 1, 1);
  if (tex_tiny_1x1) {
    SDL_UpdateTexture(tex_tiny_1x1, NULL, &white_pixel, sizeof(uint32_t));
  }

  tex_streaming = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, 64, 64);
  if (tex_streaming) {
    uint32_t zero_buf[64 * 64] = {0};
    SDL_UpdateTexture(tex_streaming, NULL, zero_buf, 64 * sizeof(uint32_t));
  }

  tex_render_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_TARGET, 160, 120);
  tex_pip_backbuffer = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, 160, 120);
  tex_pip_rendertarget = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STATIC, 160, 120);

  const uint32_t colors[FORMAT_COUNT] = {0xFFFF2222, 0xFF22FF22, 0xFF2288FF,
                                         0xFFFFFF22, 0xFFFF22FF, 0xFF22FFFF,
                                         0xFFFF8822, 0xFFAA44FF, 0xFF44FFAA};
  for (int i = 0; i < FORMAT_COUNT; ++i) {
    tex_formats[i] =
        create_checker_texture(renderer, 32, 32, test_format_list[i],
                               SDL_TEXTUREACCESS_STATIC, colors[i], 0xFF202020);
  }

  // Deterministic PRNG seed so Linux and xemu generate identical initial sprite
  // states
  SDL_srand(1337);
  for (int i = 0; i < MAX_SPRITE_BATCH; ++i) {
    sprites[i].x = SDL_randf() * (SCREEN_W - 16.0f);
    sprites[i].y = SDL_randf() * (SCREEN_H - 16.0f);
    sprites[i].vx = (SDL_randf() - 0.5f) * 3.5f;
    sprites[i].vy = (SDL_randf() - 0.5f) * 3.5f;
    sprites[i].color =
        (SDL_FColor){0.3f + SDL_randf() * 0.7f, 0.3f + SDL_randf() * 0.7f,
                     0.3f + SDL_randf() * 0.7f, 0.65f};
  }

  // Initialize frame 0 texture state
  update_streaming_and_swizzled(0);
}

static void run_vertex_overflow_test(void) {
  enum { BATCH_VERTS = 2400 };
  static SDL_Vertex burst[BATCH_VERTS];

  for (int i = 0; i < BATCH_VERTS; i += 3) {
    float x = (float)((i * 7) % (SCREEN_W - 8));
    float y = (float)((i * 3) % (SCREEN_H - 8));
    burst[i + 0] =
        (SDL_Vertex){{x, y}, {1.0f, 0.2f, 0.2f, 0.25f}, {0.0f, 0.0f}};
    burst[i + 1] =
        (SDL_Vertex){{x + 6.0f, y}, {0.2f, 1.0f, 0.2f, 0.25f}, {1.0f, 0.0f}};
    burst[i + 2] =
        (SDL_Vertex){{x, y + 6.0f}, {0.2f, 0.2f, 1.0f, 0.25f}, {0.0f, 1.0f}};
  }

  int succeeded_batches = 0;
  bool hit_overflow = false;

  for (int b = 0; b < 15; ++b) {
    if (!SDL_RenderGeometry(renderer, tex_pot_swizzled, burst, BATCH_VERTS,
                            NULL, 0)) {
      hit_overflow = true;
      break;
    }
    succeeded_batches++;
  }

  if (hit_overflow) {
    SDL_snprintf(status_line, sizeof(status_line),
                 "OVERFLOW CAUGHT AT %d KB! RECOVERING...",
                 (succeeded_batches * BATCH_VERTS * 24) / 1024);
    overflow_recovery_frames = 3;
  } else {
    SDL_snprintf(status_line, sizeof(status_line),
                 "OVERFLOW TEST: 864 KB QUEUED OK (Desktop RAM)");
  }
}

static void run_transient_destroy_test(void) {
  SDL_Texture *temp =
      create_checker_texture(renderer, 32, 32, SDL_PIXELFORMAT_ARGB8888,
                             SDL_TEXTUREACCESS_STATIC, 0xFFFFFFFF, 0xFFFF0000);
  if (temp) {
    const SDL_FRect dst = {300.0f, 300.0f, 40.0f, 40.0f};
    SDL_RenderTexture(renderer, temp, NULL, &dst);
    SDL_FlushRenderer(renderer);
    SDL_DestroyTexture(temp);
    SDL_snprintf(status_line, sizeof(status_line),
                 "TRANSIENT TEXTURE CREATED, DRAWN & DESTROYED OK!");
  }
}

static void handle_action_select_prev(void) {
  selected_zone = (selected_zone + ZONE_COUNT - 1) % ZONE_COUNT;
}

static void handle_action_select_next(void) {
  selected_zone = (selected_zone + 1) % ZONE_COUNT;
}

static void handle_action_toggle_selected(void) {
  zone_enabled[selected_zone] = !zone_enabled[selected_zone];
}

static void handle_action_toggle_all(void) {
  bool any_enabled = false;
  for (int i = 0; i < ZONE_COUNT; ++i) {
    if (zone_enabled[i]) {
      any_enabled = true;
      break;
    }
  }
  for (int i = 0; i < ZONE_COUNT; ++i) {
    zone_enabled[i] = !any_enabled;
  }
}

static void handle_action_cycle_sprites(void) {
  sprite_preset_idx = (sprite_preset_idx + 1) % 4;
  zone_enabled[0] = (sprite_presets[sprite_preset_idx] > 0);
}

static void handle_action_toggle_pause(uint32_t sim_frame) {
  paused = !paused;
  if (paused) {
    SDL_snprintf(status_line, sizeof(status_line),
                 "PAUSED @ FRAME %u (Start/P:Resume R2/.:Step)",
                 (unsigned)sim_frame);
  } else {
    SDL_snprintf(status_line, sizeof(status_line),
                 "RESUMED (Start/P:Pause B:Overflow Back/Esc:Exit)");
  }
}

#ifdef NXDK
#include <xboxkrnl/xboxkrnl.h>

static void SDLCALL NXDK_LogOutput(void *userdata, int category,
                                   SDL_LogPriority priority,
                                   const char *message) {
  (void)userdata;
  (void)category;
  (void)priority;

  /* DbgPrint accepts a format string followed by arguments.
     Always pass message as an argument rather than the format string directly
     to prevent format-string injection if the message contains '%' */
  DbgPrint("%s\n", message);
}
#endif

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  char path[512];

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    SDL_Log("SDL_Init Error: %s", SDL_GetError());
    return 1;
  }

#if defined(NXDK) || defined(__XBOX__)
  SDL_SetLogOutputFunction(NXDK_LogOutput, NULL);
  const SDL_WindowFlags win_flags = SDL_WINDOW_FULLSCREEN;
#else
  const SDL_WindowFlags win_flags = 0;
#endif

  if (!SDL_CreateWindowAndRenderer("SDL3 Cross-Platform Stress Test", SCREEN_W,
                                   SCREEN_H, win_flags, &window, &renderer)) {
    SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
    SDL_Quit();
    return SDL_APP_FAILURE;
  }

  SDL_SetRenderVSync(renderer, 1);

  int num_gamepads = 0;
  SDL_JoystickID *joy_ids = SDL_GetGamepads(&num_gamepads);
  if (joy_ids && num_gamepads > 0) {
    gamepad = SDL_OpenGamepad(joy_ids[0]);
  }
  SDL_free(joy_ids);

  // Load optional assets using platform-specific PATH_SEP ("\\" on nxdk/Win32,
  // "/" on Linux)
  if (resolve_asset_path(path, sizeof(path), "sample.jpg")) {
    SDL_Surface *image_surface = IMG_Load(path);
    if (image_surface) {
      image_texture = SDL_CreateTextureFromSurface(renderer, image_surface);
      SDL_DestroySurface(image_surface);
    }
  }

  if (TTF_Init()) {
    if (resolve_asset_path(path, sizeof(path), "Roboto-Regular.ttf")) {
      TTF_Font *font = TTF_OpenFont(path, 24);
      if (font) {
        SDL_Surface *ttf_surface = TTF_RenderText_Blended(
            font, "SDL3 - nxdk!", 0, (SDL_Color){255, 255, 255, 255});
        if (ttf_surface) {
          text_texture = SDL_CreateTextureFromSurface(renderer, ttf_surface);
          SDL_DestroySurface(ttf_surface);
        }
        TTF_CloseFont(font);
      }
    }
    TTF_Quit();
  }

  if (MIX_Init()) {
    if (resolve_asset_path(path, sizeof(path), "music.mp3")) {
      mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
      if (mixer) {
        audio = MIX_LoadAudio(mixer, path, false);
        if (audio) {
          track = MIX_CreateTrack(mixer);
          if (track) {
            MIX_SetTrackAudio(track, audio);
            MIX_PlayTrack(track, 0);
          }
        }
      }
    }
  }

  init_stress_resources();

  uint32_t sim_frame = 0;
  uint32_t fps_frames = 0;
  uint64_t last_fps_tick = SDL_GetTicksNS();
  float measured_fps = 60.0f;
  bool auto_pause_triggered = false;
  bool r2_was_pressed = false;
  bool running = true;

  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        running = false;
        break;

      case SDL_EVENT_GAMEPAD_ADDED:
        if (!gamepad) {
          gamepad = SDL_OpenGamepad(ev.gdevice.which);
        }
        break;

      case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad && SDL_GetGamepadID(gamepad) == ev.gdevice.which) {
          SDL_CloseGamepad(gamepad);
          gamepad = NULL;
        }
        break;

      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        switch (ev.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_BACK:
          running = false;
          break;
        case SDL_GAMEPAD_BUTTON_START:
          handle_action_toggle_pause(sim_frame);
          break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
          handle_action_select_prev();
          break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
          handle_action_select_next();
          break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
          handle_action_toggle_selected();
          break;
        case SDL_GAMEPAD_BUTTON_WEST:
          handle_action_toggle_all();
          break;
        case SDL_GAMEPAD_BUTTON_NORTH:
          handle_action_cycle_sprites();
          break;
        case SDL_GAMEPAD_BUTTON_EAST:
          trigger_overflow_test = true;
          break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
          trigger_destroy_test = true;
          break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
          force_readpixels = true;
          break;
        default:
          break;
        }
        break;

      case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
          bool pressed = (ev.gaxis.value > 16000);
          if (pressed && !r2_was_pressed && paused) {
            step_one_frame = true;
          }
          r2_was_pressed = pressed;
        }
        break;

      case SDL_EVENT_KEY_DOWN:
        switch (ev.key.key) {
        case SDLK_ESCAPE:
        case SDLK_Q:
          running = false;
          break;
        case SDLK_P:
          handle_action_toggle_pause(sim_frame);
          break;
        case SDLK_PERIOD:
          if (paused)
            step_one_frame = true;
          break;
        case SDLK_LEFT:
          handle_action_select_prev();
          break;
        case SDLK_RIGHT:
          handle_action_select_next();
          break;
        case SDLK_SPACE:
        case SDLK_RETURN:
          handle_action_toggle_selected();
          break;
        case SDLK_A:
          handle_action_toggle_all();
          break;
        case SDLK_S:
          handle_action_cycle_sprites();
          break;
        case SDLK_O:
          trigger_overflow_test = true;
          break;
        case SDLK_D:
          trigger_destroy_test = true;
          break;
        case SDLK_R:
          force_readpixels = true;
          break;
        default:
          break;
        }
        break;
      }
    }

    if (!running) {
      break;
    }

    const int active_sprites =
        zone_enabled[0] ? sprite_presets[sprite_preset_idx] : 0;
    const bool advance_sim = (!paused || step_one_frame);

    if (advance_sim) {
      sim_frame++;
      update_streaming_and_swizzled(sim_frame);

      for (int i = 0; i < active_sprites; ++i) {
        sprites[i].x += sprites[i].vx;
        sprites[i].y += sprites[i].vy;
        if (sprites[i].x < 0.0f || sprites[i].x > SCREEN_W - 12.0f)
          sprites[i].vx = -sprites[i].vx;
        if (sprites[i].y < 0.0f || sprites[i].y > SCREEN_H - 12.0f)
          sprites[i].vy = -sprites[i].vy;
      }

      if (step_one_frame) {
        SDL_snprintf(status_line, sizeof(status_line),
                     "STEPPED TO FRAME %u (Start/P:Resume R2/.:Step)",
                     (unsigned)sim_frame);
        step_one_frame = false;
      }

      // Auto-pause at AUTO_PAUSE_FRAME on all platforms for deterministic pixel
      // comparison
      if (!auto_pause_triggered && sim_frame == AUTO_PAUSE_FRAME) {
        paused = true;
        auto_pause_triggered = true;
        force_readpixels = true;
        SDL_snprintf(status_line, sizeof(status_line),
                     "AUTO-PAUSED @ FRAME %d (Start/P:Resume R2/.:Step)",
                     AUTO_PAUSE_FRAME);
      }
    }

    // All animations use deterministic sim_time instead of wall-clock
    // SDL_GetTicks()
    const float sim_time = (float)sim_frame * (1.0f / 60.0f);
    const bool do_readpixels =
        (advance_sim && sim_frame == 5) || force_readpixels;

    // =========================================================================
    // PASS 1: Offscreen Render-To-Texture, Flip Modes & Target ReadPixels (Zone
    // 3)
    // =========================================================================
    if (zone_enabled[3] && tex_render_target) {
      SDL_SetRenderTarget(renderer, tex_render_target);

      const SDL_Rect tiny_clip = {40, 40, 20, 20};
      SDL_SetRenderClipRect(renderer, &tiny_clip);
      SDL_SetRenderDrawColor(renderer, 20, 45, 80, 255);
      SDL_RenderClear(renderer);
      SDL_SetRenderClipRect(renderer, NULL);

      const SDL_FRect rt_quad = {52.0f, 32.0f, 56.0f, 56.0f};
      const SDL_FPoint pivot = {28.0f, 28.0f};
      SDL_RenderTextureRotated(renderer, tex_pot_swizzled, NULL, &rt_quad,
                               (double)(sim_time * 90.0f), &pivot,
                               SDL_FLIP_NONE);

      const SDL_FRect corner_h = {6.0f, 88.0f, 24.0f, 24.0f};
      const SDL_FRect corner_v = {130.0f, 88.0f, 24.0f, 24.0f};
      SDL_RenderTextureRotated(renderer, tex_npot_static, NULL, &corner_h, 0.0,
                               NULL, SDL_FLIP_HORIZONTAL);
      SDL_RenderTextureRotated(
          renderer, tex_npot_static, NULL, &corner_v, 0.0, NULL,
          (SDL_FlipMode)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));

      SDL_SetRenderDrawColor(renderer, 0, 255, 180, 255);
      const SDL_FRect rt_border = {1.0f, 1.0f, 158.0f, 118.0f};
      SDL_RenderRect(renderer, &rt_border);
      SDL_RenderLine(renderer, 0.0f, 0.0f, 160.0f, 120.0f);
      SDL_RenderDebugText(renderer, 8.0f, 8.0f, "RENDER TARGET");

      if (do_readpixels && tex_pip_rendertarget) {
        SDL_Surface *rt_snap = SDL_RenderReadPixels(renderer, NULL);
        if (rt_snap) {
          SDL_UpdateTexture(tex_pip_rendertarget, NULL, rt_snap->pixels,
                            rt_snap->pitch);
          SDL_DestroySurface(rt_snap);
        }
      }

      SDL_SetRenderTarget(renderer, NULL);
    }

    // =========================================================================
    // PASS 2: Main Backbuffer Rendering
    // =========================================================================
    SDL_SetRenderDrawColor(renderer, 22, 22, 28, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);

    // --- Zone 0: Background Bouncing Sprite Batch ---
    if (active_sprites > 0 && tex_pot_swizzled) {
      SDL_SetTextureBlendMode(tex_pot_swizzled, SDL_BLENDMODE_BLEND);
      for (int i = 0; i < active_sprites; ++i) {
        const SDL_FRect dst = {sprites[i].x, sprites[i].y, 12.0f, 12.0f};
        SDL_SetTextureColorModFloat(tex_pot_swizzled, sprites[i].color.r,
                                    sprites[i].color.g, sprites[i].color.b);
        SDL_SetTextureAlphaModFloat(tex_pot_swizzled, 0.35f);
        SDL_RenderTexture(renderer, tex_pot_swizzled, NULL, &dst);
      }
      SDL_SetTextureColorModFloat(tex_pot_swizzled, 1.0f, 1.0f, 1.0f);
      SDL_SetTextureAlphaModFloat(tex_pot_swizzled, 1.0f);
    }

    // --- Zone 1 (Top-Left): All 9 Pixel Formats, NPOT Linear/Nearest, 1x1
    // Texture ---
    if (zone_enabled[1]) {
      for (int i = 0; i < FORMAT_COUNT; ++i) {
        if (tex_formats[i]) {
          const SDL_FRect fmt_dst = {10.0f + i * 21.0f, 24.0f, 19.0f, 24.0f};
          SDL_RenderTexture(renderer, tex_formats[i], NULL, &fmt_dst);
        }
      }
      if (tex_npot_static) {
        SDL_SetTextureScaleMode(tex_npot_static, SDL_SCALEMODE_NEAREST);
        const SDL_FRect npot_near = {10.0f, 54.0f, 90.0f, 50.0f};
        SDL_RenderTexture(renderer, tex_npot_static, NULL, &npot_near);

        SDL_SetTextureScaleMode(tex_npot_static, SDL_SCALEMODE_LINEAR);
        const SDL_FRect npot_lin = {106.0f, 54.0f, 90.0f, 50.0f};
        SDL_RenderTexture(renderer, tex_npot_static, NULL, &npot_lin);
      }
      if (tex_tiny_1x1) {
        SDL_SetTextureColorMod(tex_tiny_1x1, 255, 128, 0);
        const SDL_FRect tiny_dst = {10.0f, 110.0f, 186.0f, 10.0f};
        SDL_RenderTexture(renderer, tex_tiny_1x1, NULL, &tiny_dst);
      }
      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderDebugText(renderer, 10.0f, 10.0f, "1:9 FORMATS/NPOT/1x1");
    }

    // --- Zone 2 (Top-Center): Streaming Texture & Tiled Wrapping (XGU_WRAP)
    // ---
    if (zone_enabled[2]) {
      if (tex_streaming) {
        const SDL_FRect stream_dst = {220.0f, 24.0f, 96.0f, 96.0f};
        SDL_RenderTexture(renderer, tex_streaming, NULL, &stream_dst);
      }
      if (tex_pot_swizzled) {
        const SDL_FRect wrap_dst = {324.0f, 24.0f, 96.0f, 96.0f};
        SDL_RenderTextureTiled(renderer, tex_pot_swizzled, NULL, 0.5f,
                               &wrap_dst);
      }
      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderDebugText(renderer, 220.0f, 10.0f, "2:STREAM & UV WRAP");
    }

    // --- Zone 3 (Top-Right): Offscreen Render Target Output ---
    if (zone_enabled[3] && tex_render_target) {
      const SDL_FRect rt_dst = {450.0f, 24.0f, 175.0f, 110.0f};
      SDL_RenderTexture(renderer, tex_render_target, NULL, &rt_dst);
      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderDebugText(renderer, 450.0f, 10.0f, "3:TARGET & FLIPS");
    }

    // --- Zone 4 (Mid-Left): All 7 Blend Modes & Batched FillRects ---
    if (zone_enabled[4]) {
      const SDL_BlendMode blend_modes[7] = {SDL_BLENDMODE_NONE,
                                            SDL_BLENDMODE_BLEND,
                                            SDL_BLENDMODE_BLEND_PREMULTIPLIED,
                                            SDL_BLENDMODE_ADD,
                                            SDL_BLENDMODE_ADD_PREMULTIPLIED,
                                            SDL_BLENDMODE_MOD,
                                            SDL_BLENDMODE_MUL};
      SDL_FRect bg_rects[7];
      for (int i = 0; i < 7; ++i) {
        bg_rects[i] = (SDL_FRect){10.0f + i * 27.0f, 160.0f, 24.0f, 60.0f};
      }
      SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
      SDL_SetRenderDrawColor(renderer, 100, 140, 200, 255);
      SDL_RenderFillRects(renderer, bg_rects, 7);

      for (int i = 0; i < 7; ++i) {
        SDL_SetRenderDrawBlendMode(renderer, blend_modes[i]);
        SDL_SetRenderDrawColor(renderer, 255, 80, 80, 160);
        const SDL_FRect over_rect = {14.0f + i * 27.0f, 175.0f, 20.0f, 45.0f};
        SDL_RenderFillRect(renderer, &over_rect);
      }
      SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderDebugText(renderer, 10.0f, 145.0f, "4:BLEND MODES (7)");
    }

    // --- Zone 5 (Mid-Center): Custom Indexed Geometry & >255 Vertex Triangle
    // Fan ---
    if (zone_enabled[5]) {
      SDL_Vertex fan_verts[LARGE_PRIM_COUNT];
      const float cx = 310.0f, cy = 215.0f, radius = 48.0f;
      for (int i = 0; i < LARGE_PRIM_COUNT; i += 3) {
        float a0 =
            (float)i * (2.0f * SDL_PI_F / (float)LARGE_PRIM_COUNT) + sim_time;
        float a1 =
            (float)(i + 3) * (2.0f * SDL_PI_F / (float)LARGE_PRIM_COUNT) +
            sim_time;
        fan_verts[i + 0].position = (SDL_FPoint){cx, cy};
        fan_verts[i + 0].color = (SDL_FColor){1.0f, 1.0f, 1.0f, 1.0f};
        fan_verts[i + 1].position =
            (SDL_FPoint){cx + cosf(a0) * radius, cy + sinf(a0) * radius};
        fan_verts[i + 1].color = (SDL_FColor){
            0.5f + 0.5f * cosf(a0), 0.5f + 0.5f * sinf(a0), 0.2f, 1.0f};
        fan_verts[i + 2].position =
            (SDL_FPoint){cx + cosf(a1) * radius, cy + sinf(a1) * radius};
        fan_verts[i + 2].color = (SDL_FColor){
            0.5f + 0.5f * cosf(a1), 0.5f + 0.5f * sinf(a1), 0.8f, 1.0f};
      }
      SDL_RenderGeometry(renderer, NULL, fan_verts, LARGE_PRIM_COUNT, NULL, 0);

      const float raw_xy[8] = {215.0f, 165.0f, 255.0f, 165.0f,
                               255.0f, 205.0f, 215.0f, 205.0f};
      const SDL_FColor raw_cols[4] = {{1.0f, 0.0f, 0.0f, 1.0f},
                                      {0.0f, 1.0f, 0.0f, 1.0f},
                                      {0.0f, 0.0f, 1.0f, 1.0f},
                                      {1.0f, 1.0f, 0.0f, 1.0f}};
      const uint8_t idx_u8[3] = {0, 1, 2};
      const uint32_t idx_u32[3] = {0, 2, 3};
      SDL_RenderGeometryRaw(renderer, NULL, raw_xy, 8, raw_cols,
                            sizeof(SDL_FColor), NULL, 0, 4, idx_u8, 3, 1);
      SDL_RenderGeometryRaw(renderer, NULL, raw_xy, 8, raw_cols,
                            sizeof(SDL_FColor), NULL, 0, 4, idx_u32, 3, 4);

      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderDebugText(renderer, 215.0f, 145.0f, "5:MESH >255 & IDX8/32");
    }

    // --- Zone 6 (Mid-Right): Combiner Reset, >256 Lines/Points, RenderScale &
    // ColorScale ---
    if (zone_enabled[6]) {
      SDL_SetRenderDrawColor(renderer, 50, 255, 100, 255);

      SDL_SetRenderColorScale(renderer, 2.0f);
      SDL_SetRenderScale(renderer, 1.5f, 1.5f);
      const SDL_FRect scaled_tex_rect = {450.0f / 1.5f, 158.0f / 1.5f, 18.0f,
                                         18.0f};
      SDL_RenderTexture(renderer, tex_pot_swizzled, NULL, &scaled_tex_rect);
      SDL_SetRenderScale(renderer, 1.0f, 1.0f);
      SDL_SetRenderColorScale(renderer, 1.0f);

      SDL_FPoint wave_lines[LARGE_PRIM_COUNT];
      SDL_FPoint star_points[LARGE_PRIM_COUNT];
      for (int i = 0; i < LARGE_PRIM_COUNT; ++i) {
        float t = (float)i / (float)(LARGE_PRIM_COUNT - 1);
        wave_lines[i].x = 450.0f + t * 175.0f;
        wave_lines[i].y = 200.0f + sinf(t * 18.0f + sim_time * 4.0f) * 20.0f;

        star_points[i].x = 450.0f + (float)((i * 37 + sim_frame) % 175);
        star_points[i].y = 230.0f + (float)((i * 19 + sim_frame / 2) % 35);
      }
      SDL_RenderLines(renderer, wave_lines, LARGE_PRIM_COUNT);
      SDL_RenderPoints(renderer, star_points, LARGE_PRIM_COUNT);

      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderDebugText(renderer, 450.0f, 145.0f, "6:360 LINES & SCALE");
    }

    // --- Zone 7 (Bottom-Left): Viewport + Out-of-Bounds Negative ClipRect ---
    if (zone_enabled[7]) {
      const SDL_Rect sub_vp = {10, 300, 200, 150};
      SDL_SetRenderViewport(renderer, &sub_vp);

      const SDL_Rect oob_clip = {-20, -20, 160, 130};
      SDL_SetRenderClipRect(renderer, &oob_clip);

      SDL_SetRenderDrawColor(renderer, 180, 60, 220, 255);
      const SDL_FRect huge_rect = {-50.0f, -50.0f, 300.0f, 300.0f};
      SDL_RenderFillRect(renderer, &huge_rect);

      if (image_texture) {
        const SDL_FRect img_dst = {10.0f, 20.0f, 180.0f, 120.0f};
        SDL_RenderTexture(renderer, image_texture, NULL, &img_dst);
      }
      if (text_texture) {
        const SDL_FRect txt_dst = {10.0f, 80.0f, 140.0f, 35.0f};
        SDL_RenderTexture(renderer, text_texture, NULL, &txt_dst);
      }

      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderDebugText(renderer, 6.0f, 6.0f, "7:VIEWPORT+CLIP");

      SDL_SetRenderClipRect(renderer, NULL);
      SDL_SetRenderViewport(renderer, NULL);

      SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255);
      const SDL_FRect vp_outline = {10.0f, 300.0f, 200.0f, 150.0f};
      SDL_RenderRect(renderer, &vp_outline);
    }

    // --- Zone 8 (Bottom-Right): Split ReadPixels PIP (Target Left, Backbuffer
    // Right) ---
    if (zone_enabled[8] || do_readpixels) {
      if (tex_pip_backbuffer && do_readpixels) {
        const SDL_Rect capture_rect = {215, 145, 160, 120};
        SDL_Surface *snap = SDL_RenderReadPixels(renderer, &capture_rect);
        if (snap) {
          SDL_UpdateTexture(tex_pip_backbuffer, NULL, snap->pixels,
                            snap->pitch);
          SDL_DestroySurface(snap);
          if (!paused) {
            SDL_snprintf(status_line, sizeof(status_line),
                         "READPIXELS CAPTURED @ FRAME %u!",
                         (unsigned)sim_frame);
          }
        }
        force_readpixels = false;
      }

      if (tex_pip_rendertarget) {
        const SDL_FRect pip_rt_dst = {450.0f, 300.0f, 85.0f, 75.0f};
        SDL_RenderTexture(renderer, tex_pip_rendertarget, NULL, &pip_rt_dst);
        SDL_SetRenderDrawColor(renderer, 0, 255, 180, 255);
        SDL_RenderRect(renderer, &pip_rt_dst);
      }
      if (tex_pip_backbuffer) {
        const SDL_FRect pip_bb_dst = {540.0f, 300.0f, 85.0f, 75.0f};
        SDL_RenderTexture(renderer, tex_pip_backbuffer, NULL, &pip_bb_dst);
        SDL_SetRenderDrawColor(renderer, 0, 255, 255, 255);
        SDL_RenderRect(renderer, &pip_bb_dst);
      }
      SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
      SDL_RenderDebugText(renderer, 450.0f, 285.0f, "8:READPIX(RT|BB)");
    }

    // --- Highlight Selected Zone Outline (Deterministic via sim_time) ---
    {
      const SDL_FRect sel_box = zone_bounds[selected_zone];
      uint8_t pulse =
          paused ? 255 : (uint8_t)(155.0f + 100.0f * sinf(sim_time * 8.0f));
      if (zone_enabled[selected_zone]) {
        SDL_SetRenderDrawColor(renderer, 0, pulse, 255, 255);
      } else {
        SDL_SetRenderDrawColor(renderer, 255, 60, 60, 255);
      }
      SDL_RenderRect(renderer, &sel_box);
    }

    // --- HUD Panel (Bottom-Center) ---
    fps_frames++;
    uint64_t now_ns = SDL_GetTicksNS();
    if (now_ns - last_fps_tick >= 500000000ULL) {
      measured_fps = (float)fps_frames * 1e9f / (float)(now_ns - last_fps_tick);
      fps_frames = 0;
      last_fps_tick = now_ns;
    }

    if (advance_sim && overflow_recovery_frames > 0) {
      overflow_recovery_frames--;
      if (overflow_recovery_frames == 0) {
        SDL_snprintf(status_line, sizeof(status_line),
                     "OVERFLOW RECOVERED OK! RING BUFFER CLEAN");
      }
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 215);
    const SDL_FRect hud_bg = {215.0f, 385.0f, 415.0f, 88.0f};
    SDL_RenderFillRect(renderer, &hud_bg);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    char line1[64], line2[64];
    if (paused) {
      // Fixed deterministic string while paused so screenshots match
      // pixel-for-pixel
      SDL_snprintf(line1, sizeof(line1), "FRAME: %u [PAUSED] | Sprites(Y): %d",
                   (unsigned)sim_frame, active_sprites);
    } else {
      SDL_snprintf(line1, sizeof(line1),
                   "FRAME: %u | FPS: %.1f | Sprites(Y): %d",
                   (unsigned)sim_frame, measured_fps, active_sprites);
    }
    SDL_snprintf(line2, sizeof(line2), "SEL(D-Pad): %s [%s] (A:Toggle X:All)",
                 zone_names[selected_zone],
                 zone_enabled[selected_zone] ? "ON" : "OFF");

    SDL_SetRenderDrawColor(renderer, 100, 255, 100, 255);
    SDL_RenderDebugText(renderer, 222.0f, 393.0f, line1);
    SDL_SetRenderDrawColor(renderer, 255, 255, 120, 255);
    SDL_RenderDebugText(renderer, 222.0f, 415.0f, line2);
    SDL_SetRenderDrawColor(renderer, 255, 150, 255, 255);
    SDL_RenderDebugText(renderer, 222.0f, 437.0f, status_line);
    SDL_SetRenderDrawColor(renderer, 180, 180, 180, 255);
    SDL_RenderDebugText(renderer, 222.0f, 457.0f,
                        "Start/P:Pause R2/.:Step B:Overflow Back/Esc:Exit");

    if (trigger_destroy_test) {
      run_transient_destroy_test();
      trigger_destroy_test = false;
    }

    if (trigger_overflow_test) {
      run_vertex_overflow_test();
      trigger_overflow_test = false;
    }

    SDL_RenderPresent(renderer);
  }

  // =========================================================================
  // Clean, Hang-Free Teardown Sequence
  // =========================================================================
  SDL_SetRenderTarget(renderer, NULL);
  SDL_FlushRenderer(renderer);

  if (gamepad) {
    SDL_CloseGamepad(gamepad);
    gamepad = NULL;
  }

  if (mixer) {
    MIX_StopAllTracks(mixer, 0);
    if (track) {
      MIX_SetTrackAudio(track, NULL);
      MIX_DestroyTrack(track);
      track = NULL;
    }
    if (audio) {
      MIX_DestroyAudio(audio);
      audio = NULL;
    }
    MIX_DestroyMixer(mixer);
    mixer = NULL;
    MIX_Quit();
  }

  for (int i = 0; i < FORMAT_COUNT; ++i) {
    SDL_DestroyTexture(tex_formats[i]);
  }
  SDL_DestroyTexture(tex_pot_swizzled);
  SDL_DestroyTexture(tex_npot_static);
  SDL_DestroyTexture(tex_tiny_1x1);
  SDL_DestroyTexture(tex_streaming);
  SDL_DestroyTexture(tex_render_target);
  SDL_DestroyTexture(tex_pip_backbuffer);
  SDL_DestroyTexture(tex_pip_rendertarget);
  SDL_DestroyTexture(image_texture);
  SDL_DestroyTexture(text_texture);

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}