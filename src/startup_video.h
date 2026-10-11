#pragma once

#include <Windows.h>
#include <cstdint>
#include <memory>
#include <string>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

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
    Non-owning view of a decoded frame: planes stay valid only until the next Next() or Close() of the decoder.
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

class CStartupVideoFFmpegAPI;

class CStartupVideoDecoder
{
public:
    CStartupVideoDecoder();
    ~CStartupVideoDecoder();

    CStartupVideoDecoder(const CStartupVideoDecoder&)            = delete;
    CStartupVideoDecoder& operator=(const CStartupVideoDecoder&) = delete;

    /*
        Accepts a DLL name on the host's search path or an explicit path for standalone callers.
    */
    bool Open(const char* dllPath, const char* filename, bool (*interrupt)(void*) = nullptr, void* context = nullptr);

    StartupVideoReadResult Next(CStartupVideoFrameView& output);

    const std::string& GetError() const;

    int GetWidth() const;
    int GetHeight() const;

private:
    void Close();
    bool Fail(const char* operation, int result = 0);
    bool IsInterrupted() const;

    static int InterruptCallback(void* opaque);

    HMODULE                                 m_hModule{};
    std::unique_ptr<CStartupVideoFFmpegAPI> m_pAPI;

    AVFormatContext* m_pFormat{};
    AVCodecContext*  m_pCodec{};
    AVFrame*         m_pFrame{};
    AVPacket*        m_pPacket{};

    int  m_StreamIndex{-1};
    bool m_Draining{};
    bool m_PacketPending{};

    bool (*m_pfnInterrupt)(void*){};
    void* m_pInterruptContext{};

    //Reset by Close()
    int64_t m_FirstTimestamp{};
    double  m_LastSeconds{};
    double  m_FrameDuration{};

    std::string m_Error;
};
