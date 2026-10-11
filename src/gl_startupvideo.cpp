#include "gl_local.h"
#include "startup_video.h"
#include "startup_video_gl.h"
#include <SDL2/SDL_events.h>
#include <chrono>
#include <exception>

void InitializeGraphicEngine(void* window);

namespace
{
//Resolved from HL25 engine gamedata; optional, playback is skipped if any is missing.
void* (*StartupPrecacheSound)(const char*)                                 = nullptr;
void (*StartupStartSound)(int, int, void*, float*, float, float, int, int) = nullptr;
void (*StartupUpdateSound)(float*, float*, float*, float*)                 = nullptr;

struct PlaybackEvents
{
    decltype(&SDL_PollEvent) poll    = nullptr;
    bool                     skipped = false;

    static bool IsSkipKey(const SDL_Event& event)
    {
        if (event.type != SDL_KEYDOWN)
            return false;

        const auto sym = event.key.keysym.sym;

        return sym == SDLK_RETURN || sym == SDLK_ESCAPE || sym == SDLK_SPACE;
    }

    static bool Interrupted(void* context)
    {
        auto& self = *static_cast<PlaybackEvents*>(context);

        SDL_Event event;

        while (self.poll(&event))
        {
            if (event.type == SDL_QUIT || IsSkipKey(event))
                self.skipped = true;
        }

        return self.skipped;
    }
};

void PlayStartupSound(const char* name)
{
    auto sfx = StartupPrecacheSound(name);

    if (sfx)
    {
        float origin[3] = {};
        StartupStartSound(0, CHAN_ITEM, sfx, origin, 1.0f, 1.0f, 0, PITCH_NORM);
    }
}

std::wstring WidePath(const char* path)
{
    const int count = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);

    if (!count)
        return {};

    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_ACP, 0, path, -1, result.data(), count);

    //Strip the null terminator
    result.resize(count - 1);

    return result;
}

std::string Utf8Path(const char* path)
{
    const auto wide = WidePath(path);

    const int count = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);

    if (!count)
        return {};

    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), count, nullptr, nullptr);

    //Strip the null terminator
    result.resize(count - 1);

    return result;
}

void PlayVideo(const char* filename, void* window)
{
    if (!filename || !window)
        return;

    if (!StartupPrecacheSound || !StartupStartSound || !StartupUpdateSound)
    {
        gEngfuncs.Con_Printf("Startup video: missing HL25 sound gamedata; skipping.\n");
        return;
    }

    const auto sdl = GetModuleHandleA("SDL2.dll");

    PlaybackEvents events;
    events.poll = reinterpret_cast<decltype(events.poll)>(GetProcAddress(sdl, "SDL_PollEvent"));

    auto drawableSize = reinterpret_cast<void (*)(void*, int*, int*)>(GetProcAddress(sdl, "SDL_GL_GetDrawableSize"));

    if (!events.poll || !drawableSize || !gPrivateFuncs.SDL_GL_SwapWindow)
    {
        gEngfuncs.Con_Printf("Startup video: SDL playback API unavailable; skipping.\n");
        return;
    }

    StartupVideo::Decoder decoder;

    //MetaHook adds metahook/dlls and its subdirectories to PATH before loading plugins.
    if (!decoder.Open(L"Mile.FFmpeg.dll", Utf8Path(filename).c_str(), PlaybackEvents::Interrupted, &events))
    {
        gEngfuncs.Con_Printf("Startup video: %s\n", decoder.Error().c_str());
        return;
    }

    InitializeGraphicEngine(window);

    StartupVideo::Painter painter;
    StartupVideo::Frame   frame;

    auto status = decoder.Next(frame);

    if (status != StartupVideo::ReadResult::FrameReady)
    {
        gEngfuncs.Con_Printf("Startup video: no decoded frame (%s).\n", decoder.Error().c_str());
        return;
    }

    //Start the original WAV only once a displayable frame is available.
    PlayStartupSound("UI/valve_sound.wav");

    //Stop the WAV on any early exit (error / skipped); keep it on normal completion.
    struct SoundCleanup
    {
        bool stop = true;

        ~SoundCleanup()
        {
            if (stop)
                PlayStartupSound("common/null.wav");
        }
    } sound;

    const auto start = std::chrono::steady_clock::now();

    int presented = 0;

    gEngfuncs.Con_Printf("Startup video: playing %s (%dx%d) with Mile.FFmpeg.\n", filename, decoder.Width(), decoder.Height());

    while (status == StartupVideo::ReadResult::FrameReady)
    {
        //Wait until the frame is due, keep pumping sound and events meanwhile.
        while (!PlaybackEvents::Interrupted(&events))
        {
            float origin[3] = {};
            StartupUpdateSound(origin, origin, origin, origin);

            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

            if (StartupVideo::IsFrameDue(elapsed, frame.seconds))
                break;

            Sleep(1);
        }

        if (events.skipped)
            break;

        int width  = 0;
        int height = 0;
        drawableSize(window, &width, &height);

        if (width <= 0 || height <= 0)
        {
            //A minimized window has no drawable; continue pumping events.
            Sleep(10);
            continue;
        }

        const auto rect = StartupVideo::DestinationRect(decoder.Width(), decoder.Height(), width, height);

        if (!painter.Draw(frame, rect, width, height))
        {
            gEngfuncs.Con_Printf("Startup video: %s\n", painter.Error().c_str());
            return;
        }

        gPrivateFuncs.SDL_GL_SwapWindow(window);

        ++presented;

        status = decoder.Next(frame);
    }

    if (events.skipped)
    {
        gEngfuncs.Con_Printf("VIDEO PLAYBACK: user skipped playback.\n");
    }
    else if (status == StartupVideo::ReadResult::Error)
    {
        gEngfuncs.Con_Printf("Startup video: %s\n", decoder.Error().c_str());
    }
    else
    {
        sound.stop = false;
        gEngfuncs.Con_Printf("Startup video: completed, %d frames presented.\n", presented);
    }
}
} // namespace

void R_ResolveStartupVideoSound(void* engineBase)
{
    if (g_iEngineType != ENGINE_GOLDSRC_HL25)
        return;

    //Missing optional playback symbols must not prevent entering the game.
#define RESOLVE_STARTUP_SOUND(member, name)                                                                                 \
    {                                                                                                                       \
        void* address = nullptr;                                                                                            \
        if (g_pMetaHookAPI->ResolveGameSymbol(engineBase, name, MH_GAMESYMBOL_KIND_FUNCTION, &address) == MH_GAMESYMBOL_OK) \
            member = reinterpret_cast<decltype(member)>(address);                                                           \
    }

    RESOLVE_STARTUP_SOUND(StartupPrecacheSound, "S_PrecacheSound");
    RESOLVE_STARTUP_SOUND(StartupStartSound, "S_StartDynamicSound");
    RESOLVE_STARTUP_SOUND(StartupUpdateSound, "S_Update");

#undef RESOLVE_STARTUP_SOUND
}

void __fastcall CGame_DrawStartupVideo(void* pgame, int dummy, const char* filename, void* window)
{
    if (g_iEngineType != ENGINE_GOLDSRC_HL25)
        return;

    try
    {
        PlayVideo(filename, window);
    }
    catch (const std::exception& exception)
    {
        gEngfuncs.Con_Printf("Startup video: %s\n", exception.what());
    }
}
