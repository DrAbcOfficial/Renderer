#include "startup_video.h"
#include <Windows.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace StartupVideo;

//Decoder::Open takes an ANSI DLL path, matching the engine's char paths.
static std::string AnsiPath(const wchar_t* path)
{
    const int count = WideCharToMultiByte(CP_ACP, 0, path, -1, nullptr, 0, nullptr, nullptr);
    assert(count);

    std::string result(count, '\0');
    WideCharToMultiByte(CP_ACP, 0, path, -1, result.data(), count, nullptr, nullptr);

    //Strip the null terminator
    result.resize(count - 1);

    return result;
}

static void TestDestinationRect()
{
    //Fit down
    const auto down = DestinationRect(1920, 1080, 640, 480);
    assert(640 == down.width);
    assert(360 == down.height);
    assert(60 == down.y);

    //Fill up, cropped horizontally
    const auto up = DestinationRect(320, 180, 640, 480);
    assert(853 == up.width);
    assert(480 == up.height);
    assert(-106 == up.x);

    assert(0 == DestinationRect(0, 100, 640, 480).width);
}

static void TestIsFrameDue()
{
    assert(!IsFrameDue(0.1, 0.2));
    assert(IsFrameDue(0.2, 0.2));
    assert(IsFrameDue(1.0, 0.2));
    assert(!IsFrameDue(1.0, std::numeric_limits<double>::quiet_NaN()));
}

static void TestPlaneSize()
{
    unsigned char data[128] = {};

    Frame frame;
    frame.width  = 5;
    frame.height = 3;
    frame.shiftX = 1;
    frame.shiftY = 1;

    for (int i = 0; i < 3; ++i)
    {
        frame.planes[i]  = data;
        frame.strides[i] = 8;
    }

    int width, height;

    assert(PlaneSize(frame, 1, width, height));
    assert(3 == width);
    assert(2 == height);

    frame.strides[1] = -8;
    assert(PlaneSize(frame, 1, width, height));

    frame.strides[1] = 2;
    assert(!PlaneSize(frame, 1, width, height));

    frame.shiftY = 2;
    assert(!PlaneSize(frame, 0, width, height));
}

static void TestInvalidDllPaths()
{
    Decoder decoder;

    assert(!decoder.Open(nullptr, "missing.webm"));
    assert(!decoder.Open("", "missing.webm"));
    assert(!decoder.Open("missing-startup-video-decoder.dll", "missing.webm"));
    assert(!decoder.Open("Z:\\nonexistent\\Mile.FFmpeg.dll", "missing.webm"));

    char systemDirectory[MAX_PATH] = {};
    assert(GetSystemDirectoryA(systemDirectory, MAX_PATH));

    const std::string wrongDll = std::string(systemDirectory) + "\\kernel32.dll";
    assert(!decoder.Open(wrongDll.c_str(), "missing.webm"));
}

static void TestCorruptVideo(const char* dllPath)
{
    wchar_t tempDirectory[MAX_PATH] = {};
    wchar_t tempFile[MAX_PATH]      = {};

    assert(GetTempPathW(MAX_PATH, tempDirectory));
    assert(GetTempFileNameW(tempDirectory, L"rsv", 0, tempFile));

    {
        FILE* file = _wfopen(tempFile, L"wb");
        assert(file);

        const unsigned char truncatedWebM[] = {0x1a, 0x45, 0xdf, 0xa3, 0xff};
        assert(sizeof(truncatedWebM) == fwrite(truncatedWebM, 1, sizeof(truncatedWebM), file));

        fclose(file);

        char path[MAX_PATH * 3] = {};
        assert(WideCharToMultiByte(CP_UTF8, 0, tempFile, -1, path, sizeof(path), nullptr, nullptr));

        Decoder corrupt;
        assert(!corrupt.Open(dllPath, path));
    }

    assert(DeleteFileW(tempFile));
}

static void TestDecodeVideo(Decoder& decoder, const char* dllPath, const wchar_t* videoPath)
{
    char file[4096] = {};
    assert(WideCharToMultiByte(CP_UTF8, 0, videoPath, -1, file, sizeof(file), nullptr, nullptr));

    assert(decoder.Open(dllPath, file));

    Frame      frame;
    int        width, height;
    int        count    = 0;
    double     previous = 0;
    ReadResult result;

    while ((result = decoder.Next(frame)) == ReadResult::FrameReady)
    {
        assert(frame.width > 0);
        assert(frame.height > 0);
        assert(previous <= frame.seconds);

        for (int i = 0; i < 3; ++i)
        {
            assert(PlaneSize(frame, i, width, height));
        }

        previous = frame.seconds;
        ++count;
    }

    if (result == ReadResult::Error)
        std::fprintf(stderr, "%s\n", decoder.Error().c_str());

    assert(ReadResult::End == result);
    assert(count > 1);

    std::printf("Decoded %d frames, last timestamp %.3f seconds\n", count, previous);

    //An interrupt callback that always fires must abort Open.
    assert(!decoder.Open(dllPath, file, [](void*) { return true; }));
}

static void TestDllSearchPath(const wchar_t* dllPath, const wchar_t* videoPath)
{
    const std::wstring path      = dllPath;
    const auto         separator = path.find_last_of(L"/\\");
    assert(std::wstring::npos != separator);

    const DWORD  length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring original(length, L'\0');
    if (length)
    {
        assert(length - 1 == GetEnvironmentVariableW(L"PATH", original.data(), length));
        original.resize(length - 1);
    }

    //Reproduce the host's PATH setup before any decoder has loaded Mile.FFmpeg.
    assert(!GetModuleHandleW(L"Mile.FFmpeg.dll"));
    const auto searchPath = original + L";" + path.substr(0, separator);
    assert(SetEnvironmentVariableW(L"PATH", searchPath.c_str()));
    {
        Decoder decoder;
        assert(!decoder.Open("Mile.FFmpeg.dll", "missing-startup-video.webm"));
        const auto module = GetModuleHandleW(L"Mile.FFmpeg.dll");
        assert(module && GetProcAddress(module, "avcodec_version"));
        if (videoPath)
            TestDecodeVideo(decoder, "Mile.FFmpeg.dll", videoPath);
    }
    assert(!GetModuleHandleW(L"Mile.FFmpeg.dll"));
    assert(SetEnvironmentVariableW(L"PATH", length ? original.c_str() : nullptr));
}

//argv[1]: path to Mile.FFmpeg.dll (optional)
//argv[2]: path to a WebM video to decode (optional)
int wmain(int argc, wchar_t** argv)
{
    TestDestinationRect();
    TestIsFrameDue();
    TestPlaneSize();
    TestInvalidDllPaths();

    if (argc < 2)
        return 0;

    TestDllSearchPath(argv[1], argc >= 3 ? argv[2] : nullptr);

    const std::string dllPathAnsi = AnsiPath(argv[1]);
    const char*       dllPath     = dllPathAnsi.c_str();

    Decoder decoder;
    assert(!decoder.Open(dllPath, "missing-startup-video.webm"));

    TestCorruptVideo(dllPath);

    //Repeat failed opens to exercise partial-initialization cleanup and unload.
    for (int i = 0; i < 5; ++i)
    {
        Decoder failed;
        assert(!failed.Open(dllPath, "missing-startup-video.webm"));
    }

    std::puts("startup_video_tests passed");
}
