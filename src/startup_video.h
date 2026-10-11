#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace StartupVideo
{
struct Rect
{
    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
};

//Preserve HL25's fit-down / fill-up policy, including centered cropping.
Rect DestinationRect(int videoWidth, int videoHeight, int windowWidth, int windowHeight);

struct Frame
{
    int width  = 0;
    int height = 0;

    //Chroma subsampling shift: 4:2:0 = (1, 1), 4:2:2 = (1, 0), 4:4:4 = (0, 0)
    int shiftX = 0;
    int shiftY = 0;

    const unsigned char* planes[3]  = {};
    int                  strides[3] = {};

    double seconds = 0;
};

bool PlaneSize(const Frame& frame, int plane, int& width, int& height);

bool IsFrameDue(double elapsed, double timestamp);

enum class ReadResult
{
    FrameReady,
    End,
    Error
};

class Decoder
{
public:
    Decoder();
    ~Decoder();

    Decoder(const Decoder&)            = delete;
    Decoder& operator=(const Decoder&) = delete;

    //Accepts a DLL name on the host's search path or an explicit path for standalone callers.
    bool Open(const char* dllPath, const char* filename, bool (*interrupt)(void*) = nullptr, void* context = nullptr);

    ReadResult Next(Frame& frame);

    const std::string& Error() const;

    int Width() const;
    int Height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace StartupVideo
