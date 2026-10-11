#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace StartupVideo
{
struct Rect
{
    int x = 0, y = 0, width = 0, height = 0;
};

// Preserve HL25's fit-down / fill-up policy, including centered cropping.
Rect DestinationRect(int videoWidth, int videoHeight, int windowWidth, int windowHeight);

struct Frame
{
    int                  width = 0, height = 0;
    int                  shiftX = 0, shiftY = 0;
    const unsigned char* planes[3]  = {};
    int                  strides[3] = {};
    double               seconds    = 0;
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

    // The DLL path must be absolute. No process-wide DLL search-path changes.
    bool               Open(const wchar_t* dllPath, const char* filename, bool (*interrupt)(void*) = nullptr, void* context = nullptr);
    ReadResult         Next(Frame& frame);
    const std::string& Error() const;
    int                Width() const;
    int                Height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace StartupVideo
