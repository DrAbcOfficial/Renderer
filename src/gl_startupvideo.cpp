#include "gl_local.h"
#include "gl_startupvideo.h"
#include "startup_video.h"
#include "startup_video_gl.h"
#include <ScopeExit/ScopeExit.h>
#include <SDL2/SDL_events.h>
#include <chrono>
#include <exception>

void InitializeGraphicEngine(void* window);

static std::wstring StartupVideo_WidePath(const char* path)
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

static std::string StartupVideo_Utf8Path(const char* path)
{
    const auto wide = StartupVideo_WidePath(path);

    const int count = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);

    if (!count)
        return {};

    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), count, nullptr, nullptr);

    //Strip the null terminator
    result.resize(count - 1);

    return result;
}

class CStartupVideoPlayer : public IStartupVideoPlayer
{
public:
    void Play(const char* filename, void* window) override
    {
        try
        {
            PlayInternal(filename, window);
        }
        catch (const std::exception& exception)
        {
            gEngfuncs.Con_Printf("Startup video: %s\n", exception.what());
        }
    }

    void ResolveSoundSymbols(void* engineBase)
    {
        //Missing optional playback symbols must not prevent entering the game.
#define RESOLVE_STARTUP_SOUND(member, name)                                                                                 \
    {                                                                                                                       \
        void* address = nullptr;                                                                                            \
        if (g_pMetaHookAPI->ResolveGameSymbol(engineBase, name, MH_GAMESYMBOL_KIND_FUNCTION, &address) == MH_GAMESYMBOL_OK) \
            member = reinterpret_cast<decltype(member)>(address);                                                           \
    }

        RESOLVE_STARTUP_SOUND(m_pfnPrecacheSound, "S_PrecacheSound");
        RESOLVE_STARTUP_SOUND(m_pfnStartDynamicSound, "S_StartDynamicSound");
        RESOLVE_STARTUP_SOUND(m_pfnUpdateSound, "S_Update");

#undef RESOLVE_STARTUP_SOUND
    }

private:
    static bool IsSkipKey(const SDL_Event& event)
    {
        if (event.type != SDL_KEYDOWN)
            return false;

        const auto sym = event.key.keysym.sym;

        return sym == SDLK_RETURN || sym == SDLK_ESCAPE || sym == SDLK_SPACE;
    }

    /*
        Pump SDL events, returns true once the user quits or presses a skip key.
        Also used as the decoder's interrupt callback.
    */
    static bool PollSkipEvents(void* context)
    {
        auto pthis = static_cast<CStartupVideoPlayer*>(context);

        SDL_Event event;

        while (pthis->m_pfnPollEvent(&event))
        {
            if (event.type == SDL_QUIT || IsSkipKey(event))
                pthis->m_Skipped = true;
        }

        return pthis->m_Skipped;
    }

    void PlayStartupSound(const char* name)
    {
        auto sfx = m_pfnPrecacheSound(name);

        if (sfx)
        {
            float origin[3] = {};
            m_pfnStartDynamicSound(0, CHAN_ITEM, sfx, origin, 1.0f, 1.0f, 0, PITCH_NORM);
        }
    }

    void PlayInternal(const char* filename, void* window)
    {
        if (!filename || !window)
            return;

        if (!m_pfnPrecacheSound || !m_pfnStartDynamicSound || !m_pfnUpdateSound)
        {
            gEngfuncs.Con_Printf("Startup video: missing HL25 sound gamedata; skipping.\n");
            return;
        }

        const auto sdl = GetModuleHandleA("SDL2.dll");

        m_pfnPollEvent = reinterpret_cast<decltype(m_pfnPollEvent)>(GetProcAddress(sdl, "SDL_PollEvent"));
        m_Skipped      = false;

        auto pfnGetDrawableSize = reinterpret_cast<void (*)(void*, int*, int*)>(GetProcAddress(sdl, "SDL_GL_GetDrawableSize"));

        if (!m_pfnPollEvent || !pfnGetDrawableSize || !gPrivateFuncs.SDL_GL_SwapWindow)
        {
            gEngfuncs.Con_Printf("Startup video: SDL playback API unavailable; skipping.\n");
            return;
        }

        CStartupVideoDecoder decoder;

        //MetaHook adds metahook/dlls and its subdirectories to PATH before loading plugins.
        if (!decoder.Open("Mile.FFmpeg.dll", StartupVideo_Utf8Path(filename).c_str(), PollSkipEvents, this))
        {
            gEngfuncs.Con_Printf("Startup video: %s\n", decoder.GetError().c_str());
            return;
        }

        InitializeGraphicEngine(window);

        CStartupVideoPainter painter;
        CStartupVideoFrame   frame;

        auto status = decoder.Next(frame);

        if (status != StartupVideoReadResult_FrameReady)
        {
            gEngfuncs.Con_Printf("Startup video: no decoded frame (%s).\n", decoder.GetError().c_str());
            return;
        }

        //Start the original WAV only once a displayable frame is available.
        PlayStartupSound("UI/valve_sound.wav");

        //Stop the WAV on any early exit (error / skipped); keep it on normal completion.
        bool stopSound = true;

        SCOPE_EXIT
        {
            if (stopSound)
                PlayStartupSound("common/null.wav");
        };

        const auto start = std::chrono::steady_clock::now();

        int presented = 0;

        gEngfuncs.Con_Printf("Startup video: playing %s (%dx%d) with Mile.FFmpeg.\n", filename, decoder.GetWidth(), decoder.GetHeight());

        while (status == StartupVideoReadResult_FrameReady)
        {
            //Wait until the frame is due, keep pumping sound and events meanwhile.
            while (!PollSkipEvents(this))
            {
                float origin[3] = {};
                m_pfnUpdateSound(origin, origin, origin, origin);

                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

                if (frame.IsDue(elapsed))
                    break;

                Sleep(1);
            }

            if (m_Skipped)
                break;

            int width  = 0;
            int height = 0;
            pfnGetDrawableSize(window, &width, &height);

            if (width <= 0 || height <= 0)
            {
                //A minimized window has no drawable; continue pumping events.
                Sleep(10);
                continue;
            }

            const auto rect = CStartupVideoRect::CalcDestination(decoder.GetWidth(), decoder.GetHeight(), width, height);

            if (!painter.Draw(frame, rect, width, height))
            {
                gEngfuncs.Con_Printf("Startup video: %s\n", painter.GetError().c_str());
                return;
            }

            gPrivateFuncs.SDL_GL_SwapWindow(window);

            ++presented;

            status = decoder.Next(frame);
        }

        if (m_Skipped)
        {
            gEngfuncs.Con_Printf("VIDEO PLAYBACK: user skipped playback.\n");
        }
        else if (status == StartupVideoReadResult_Error)
        {
            gEngfuncs.Con_Printf("Startup video: %s\n", decoder.GetError().c_str());
        }
        else
        {
            stopSound = false;
            gEngfuncs.Con_Printf("Startup video: completed, %d frames presented.\n", presented);
        }
    }

    //Resolved from HL25 engine gamedata; optional, playback is skipped if any is missing.
    void* (*m_pfnPrecacheSound)(const char*){};
    void (*m_pfnStartDynamicSound)(int, int, void*, float*, float, float, int, int){};
    void (*m_pfnUpdateSound)(float*, float*, float*, float*){};

    decltype(&SDL_PollEvent) m_pfnPollEvent{};
    bool                     m_Skipped{};
};

static CStartupVideoPlayer s_StartupVideoPlayer;

IStartupVideoPlayer* StartupVideoPlayer()
{
    return &s_StartupVideoPlayer;
}

void R_ResolveStartupVideoSound(void* engineBase)
{
    if (g_iEngineType != ENGINE_GOLDSRC_HL25)
        return;

    s_StartupVideoPlayer.ResolveSoundSymbols(engineBase);
}
