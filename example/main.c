#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <SDL3_image/SDL_image.h>

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *image_texture = NULL;
static SDL_Texture *text_texture = NULL;
static MIX_Mixer *mixer = NULL;
static MIX_Track *track = NULL;
static MIX_Audio *audio = NULL;

int main(int argc, char **argv)
{
    char path[256];
    const char *base_path = SDL_GetBasePath();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    {
        SDL_Log("SDL_Init Error: %s", SDL_GetError());
        return 1;
    }

    if (!SDL_CreateWindowAndRenderer("example", 640, 480, SDL_WINDOW_FULLSCREEN, &window, &renderer))
    {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // SDL_Image test
    {
        SDL_snprintf(path, sizeof(path), "%sassets\\sample.jpg", base_path);
        SDL_Surface *image_surface = IMG_Load(path);
        if (image_surface)
        {
            image_texture = SDL_CreateTextureFromSurface(renderer, image_surface);
            SDL_DestroySurface(image_surface);
        }
    }

    // SDL_TTF test
    {
        if (!TTF_Init())
        {
            SDL_Log("Couldn't initialize SDL_ttf: %s", SDL_GetError());
            return SDL_APP_FAILURE;
        }
        SDL_snprintf(path, sizeof(path), "%sassets\\Roboto-Regular.ttf", base_path);
        TTF_Font *font = TTF_OpenFont(path, 24);
        if (font)
        {
            SDL_Color white = {255, 255, 255, 255};
            SDL_Surface *ttf_surface = TTF_RenderText_Blended(font, "SDL3 - nxdk!", 0, (SDL_Color){255, 255, 255, 255});
            if (ttf_surface)
            {
                text_texture = SDL_CreateTextureFromSurface(renderer, ttf_surface);
            }
            SDL_DestroySurface(ttf_surface);
            TTF_CloseFont(font);
        }
        TTF_Quit();
    }

    // SDL_Mixer
    {
        SDL_snprintf(path, sizeof(path), "%sassets\\music.mp3", base_path);
        MIX_Init();
        mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
        audio = MIX_LoadAudio(mixer, path, false);
        track = MIX_CreateTrack(mixer);
        MIX_SetTrackAudio(track, audio);
        MIX_PlayTrack(track, 0);
    }

    while (1)
    {
        SDL_SetRenderDrawColor(renderer, 33, 33, 33, SDL_ALPHA_OPAQUE);
        SDL_RenderClear(renderer);

        // Test primitive rendering functions
        {
            // Draw a primitive rectangle to the screen.
            const SDL_FRect primitive_rect = {
                .x = 640 / 2 - 150,
                .y = 480 / 2 - 150,
                .w = 300,
                .h = 300};
            SDL_SetRenderDrawColor(renderer, 0, 0, 255, SDL_ALPHA_OPAQUE);
            SDL_RenderFillRect(renderer, &primitive_rect);

            // Sprinkle some points
            SDL_FPoint points[128];
            for (size_t i = 0; i < SDL_arraysize(points); ++i)
            {
                points[i].x = SDL_randf() * 640;
                points[i].y = SDL_randf() * 480;
            }
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, SDL_ALPHA_OPAQUE);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, SDL_ALPHA_OPAQUE); 
            SDL_RenderPoints(renderer, points, SDL_arraysize(points));

            // Draw a line
            SDL_SetRenderDrawColor(renderer, 255, 255, 0, SDL_ALPHA_OPAQUE);
            SDL_RenderLine(renderer, 0, 0, 640, 480);
            SDL_RenderLine(renderer, 0, 480, 640, 0);
        }

        // Draw an image using the texture created by SDL_image
        if (image_texture)
        {
            const SDL_FRect image_rect = {
                .x = 640 / 2 - 100,
                .y = 480 / 2 - 75,
                .w = 200,
                .h = 150};
            SDL_RenderTexture(renderer, image_texture, NULL, &image_rect);
        }

        // Draw some text using the texture created by SDL_ttf
        if (text_texture)
        {
            const SDL_FRect text_rect = {
                .x = 640 / 2 - 100,
                .y = 480 / 2 + 100,
                .w = 200,
                .h = 50};
            SDL_RenderTexture(renderer, text_texture, NULL, &text_rect);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(1000);
    }

    MIX_StopAllTracks(mixer, 0);
    MIX_DestroyTrack(track);
    MIX_DestroyAudio(audio);
    MIX_Quit();

    SDL_DestroyTexture(image_texture);
    SDL_DestroyTexture(text_texture);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
