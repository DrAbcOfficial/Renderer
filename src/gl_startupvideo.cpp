#include "gl_local.h"
#include "gl_startupvideo.h"
#include "startup_video.h"
#include "startup_video_gl.h"
#include <ScopeExit/ScopeExit.h>
#include <SDL2/SDL_events.h>
#include <chrono>

class CStartupVideoPlayer : public IStartupVideoPlayer
{
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
    static bool PollEvents(void* context)
    {
        auto pthis = static_cast<CStartupVideoPlayer*>(context);

        SDL_Event event;

        while (gPrivateFuncs.SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT || IsSkipKey(event))
                pthis->m_Skipped = true;
        }

        return pthis->m_Skipped;
    }

    void PlayStartupSound(const char* name)
    {
        auto sfx = gPrivateFuncs.S_PrecacheSound(name);

        if (sfx)
        {
            float origin[3] = {};
            gPrivateFuncs.S_StartDynamicSound(0, CHAN_ITEM, sfx, origin, 1.0f, 1.0f, 0, PITCH_NORM);
        }
    }

    void UpdateSound()
    {
        float origin[3] = {};
        gPrivateFuncs.S_Update(origin, origin, origin, origin);
    }

public:
    void Play(const char* filename, void* window) override
    {
        if (!filename || !window)
            return;

        if (!gPrivateFuncs.S_PrecacheSound || !gPrivateFuncs.S_StartDynamicSound || !gPrivateFuncs.S_Update)
        {
            gEngfuncs.Con_Printf("Startup video: missing HL25 sound gamedata; skipping.\n");
            return;
        }

        m_Skipped = false;

        if (!gPrivateFuncs.SDL_PollEvent || !gPrivateFuncs.SDL_GL_GetDrawableSize || !gPrivateFuncs.SDL_GL_SwapWindow)
        {
            gEngfuncs.Con_Printf("Startup video: SDL playback API unavailable; skipping.\n");
            return;
        }

        if (!InitializeGraphicEngine(window))
        {
            gEngfuncs.Con_Printf("Startup video: graphics initialization unavailable; skipping.\n");
            return;
        }

        auto decoder = CreateStartupVideoDecoder();

        if (!decoder->Open(filename, PollEvents, this))
        {
            gEngfuncs.Con_Printf("Startup video: %s\n", decoder->GetError().c_str());
            return;
        }

        CStartupVideoPainter   painter;
        CStartupVideoFrameView frame;

        auto status = decoder->Next(frame);

        if (status != StartupVideoReadResult::FrameReady)
        {
            gEngfuncs.Con_Printf("Startup video: no decoded frame (%s).\n", decoder->GetError().c_str());
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

        gEngfuncs.Con_Printf("Startup video: playing %s (%dx%d) with Mile.FFmpeg.\n", filename, decoder->GetWidth(), decoder->GetHeight());

        while (status == StartupVideoReadResult::FrameReady)
        {
            //Wait until the frame is due, keep pumping sound and events meanwhile.
            while (!PollEvents(this))
            {
                UpdateSound();

                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

                if (StartupVideo_IsFrameDue(frame.seconds, elapsed))
                    break;

                Sleep(1);
            }

            if (m_Skipped)
                break;

            int width  = 0;
            int height = 0;
            gPrivateFuncs.SDL_GL_GetDrawableSize(window, &width, &height);

            if (width <= 0 || height <= 0)
            {
                //A minimized window has no drawable; continue pumping events.
                Sleep(10);
                continue;
            }

            const auto rect = StartupVideo_CalcDestinationRect(decoder->GetWidth(), decoder->GetHeight(), width, height);

            if (!painter.Draw(frame, rect, width, height))
            {
                gEngfuncs.Con_Printf("Startup video: %s\n", painter.GetError().c_str());
                return;
            }

            gPrivateFuncs.SDL_GL_SwapWindow(window);

            ++presented;

            status = decoder->Next(frame);
        }

        if (m_Skipped)
        {
            gEngfuncs.Con_Printf("VIDEO PLAYBACK: user skipped playback.\n");
        }
        else if (status == StartupVideoReadResult::Error)
        {
            gEngfuncs.Con_Printf("Startup video: %s\n", decoder->GetError().c_str());
        }
        else
        {
            stopSound = false;
            gEngfuncs.Con_Printf("Startup video: completed, %d frames presented.\n", presented);
        }
    }

private:
    bool m_Skipped{};
};

static CStartupVideoPlayer s_StartupVideoPlayer;

IStartupVideoPlayer* StartupVideoPlayer()
{
    return &s_StartupVideoPlayer;
}
