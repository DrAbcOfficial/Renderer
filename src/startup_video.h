#pragma once

#include <cstdint>
#include <memory>
#include <string>

class CStartupVideoRect
{
public:
    int x{};
    int y{};
    int width{};
    int height{};
};

/*
    Preserve HL25's fit-down / fill-up policy, including centered cropping.
*/
CStartupVideoRect StartupVideo_CalcDestinationRect(int videoWidth, int videoHeight, int windowWidth, int windowHeight);

//Frames whose timestamp is not finite are never due.
bool StartupVideo_IsFrameDue(double frameSeconds, double elapsed);

/*
    Non-owning view: planes stay valid until the next Next(), Open(), or decoder destruction.
*/
class CStartupVideoFrameView
{
public:
    int width{};
    int height{};

    //Chroma subsampling shift: 4:2:0 = (1, 1), 4:2:2 = (1, 0), 4:4:4 = (0, 0)
    int shiftX{};
    int shiftY{};

    const unsigned char* planes[3]{};
    int                  strides[3]{};

    double seconds{};

    bool GetPlaneSize(int plane, int& planeWidth, int& planeHeight) const;
};

enum class StartupVideoReadResult
{
    FrameReady = 0,
    End,
    Error
};

class IStartupVideoDecoder
{
public:
    virtual ~IStartupVideoDecoder() = default;

    /*
        Opens a video file, loading Mile.FFmpeg.dll through the host's search path.
    */
    virtual bool Open(const char* filename, bool (*interrupt)(void*) = nullptr, void* context = nullptr) = 0;

    virtual StartupVideoReadResult Next(CStartupVideoFrameView& output) = 0;

    virtual const std::string& GetError() const = 0;

    virtual int GetWidth() const  = 0;
    virtual int GetHeight() const = 0;
};

std::unique_ptr<IStartupVideoDecoder> CreateStartupVideoDecoder();
