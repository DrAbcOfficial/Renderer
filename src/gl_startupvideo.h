#pragma once

#include <interface.h>

class IStartupVideoPlayer : public IBaseInterface
{
public:
    /*
        Decode and present the HL25 startup video on the given SDL window, blocking until it ends or is skipped.
    */
    virtual void Play(const char* filename, void* window) = 0;
};

IStartupVideoPlayer* StartupVideoPlayer();
