#define NOMINMAX
#include <Windows.h>
#include "startup_video.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

CStartupVideoRect StartupVideo_CalcDestinationRect(int videoWidth, int videoHeight, int windowWidth, int windowHeight)
{
    if (videoWidth <= 0 || videoHeight <= 0 || windowWidth <= 0 || windowHeight <= 0)
        return {};

    const double aspect = double(videoWidth) / videoHeight;

    double width      = videoWidth;
    double height     = videoHeight;
    bool   scaledDown = false;

    //Fit down
    if (width > windowWidth)
    {
        width      = windowWidth;
        height     = std::floor(width / aspect);
        scaledDown = true;
    }

    if (height > windowHeight)
    {
        height     = windowHeight;
        width      = std::floor(height * aspect);
        scaledDown = true;
    }

    //Fill up
    if (!scaledDown)
    {
        if (width < windowWidth)
        {
            height = windowHeight;
            width  = std::floor(height * aspect);
        }
        else if (height < windowHeight)
        {
            width  = windowWidth;
            height = std::floor(width / aspect);
        }
    }

    if (width < 1 || height < 1 || width > INT_MAX || height > INT_MAX)
        return {};

    const int w = int(width);
    const int h = int(height);

    return {windowWidth / 2 - w / 2, windowHeight / 2 - h / 2, w, h};
}

bool CStartupVideoFrameView::GetPlaneSize(int plane, int& planeWidth, int& planeHeight) const
{
    if (plane < 0 || plane >= 3)
        return false;

    if (width <= 0 || height <= 0)
        return false;

    if (shiftX < 0 || shiftX > 1)
        return false;

    if (shiftY < 0 || shiftY > shiftX)
        return false;

    const int x = plane ? shiftX : 0;
    const int y = plane ? shiftY : 0;

    //Round up for odd dimensions
    planeWidth  = (width >> x) + (x && (width & 1));
    planeHeight = (height >> y) + (y && (height & 1));

    if (!planes[plane])
        return false;

    //Negative stride is valid: planes point to the first displayed row.
    const auto stride = std::abs(std::int64_t(strides[plane]));

    if (stride < planeWidth)
        return false;

    if (stride * (planeHeight - 1) > PTRDIFF_MAX)
        return false;

    return true;
}

bool StartupVideo_IsFrameDue(double frameSeconds, double elapsed)
{
    if (!std::isfinite(elapsed) || !std::isfinite(frameSeconds))
        return false;

    return elapsed >= frameSeconds;
}

// clang-format off
#define STARTUP_FFMPEG_APIS(X) \
    X(avcodec_version) \
    X(avformat_version) \
    X(avutil_version) \
    X(avformat_alloc_context) \
    X(avformat_open_input) \
    X(avformat_find_stream_info) \
    X(av_find_best_stream) \
    X(av_read_frame) \
    X(avformat_close_input) \
    X(avcodec_find_decoder) \
    X(avcodec_alloc_context3) \
    X(avcodec_parameters_to_context) \
    X(avcodec_open2) \
    X(avcodec_send_packet) \
    X(avcodec_receive_frame) \
    X(avcodec_free_context) \
    X(av_frame_alloc) \
    X(av_frame_free) \
    X(av_frame_unref) \
    X(av_packet_alloc) \
    X(av_packet_free) \
    X(av_packet_unref) \
    X(av_strerror)
// clang-format on

//Function pointers resolved from Mile.FFmpeg.dll; no import library is linked.
class CStartupVideoFFmpegAPI
{
public:
#define DECLARE_API(name) decltype(&::name) name{};
    STARTUP_FFMPEG_APIS(DECLARE_API)
#undef DECLARE_API
};

CStartupVideoDecoder::CStartupVideoDecoder()
{
    Close();
}

CStartupVideoDecoder::~CStartupVideoDecoder()
{
    Close();
}

void CStartupVideoDecoder::Close()
{
    if (m_pPacket)
        m_pAPI->av_packet_free(&m_pPacket);

    if (m_pFrame)
        m_pAPI->av_frame_free(&m_pFrame);

    if (m_pCodec)
        m_pAPI->avcodec_free_context(&m_pCodec);

    if (m_pFormat)
        m_pAPI->avformat_close_input(&m_pFormat);

    if (m_hModule)
    {
        FreeLibrary(m_hModule);
        m_hModule = nullptr;
    }

    m_pAPI = std::make_unique<CStartupVideoFFmpegAPI>();

    m_pPacket = nullptr;
    m_pFrame  = nullptr;
    m_pCodec  = nullptr;
    m_pFormat = nullptr;

    m_StreamIndex   = -1;
    m_Draining      = false;
    m_PacketPending = false;

    m_pfnInterrupt      = nullptr;
    m_pInterruptContext = nullptr;

    m_FirstTimestamp = AV_NOPTS_VALUE;
    m_LastSeconds    = 0;
    m_FrameDuration  = 1.0 / 30.0;

    m_Error.clear();
}

bool CStartupVideoDecoder::Fail(const char* operation, int result)
{
    m_Error = operation;

    if (result && m_pAPI->av_strerror)
    {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        m_pAPI->av_strerror(result, text, sizeof(text));

        m_Error += ": ";
        m_Error += text;
    }

    return false;
}

bool CStartupVideoDecoder::IsInterrupted() const
{
    return m_pfnInterrupt && m_pfnInterrupt(m_pInterruptContext);
}

int CStartupVideoDecoder::InterruptCallback(void* opaque)
{
    return static_cast<CStartupVideoDecoder*>(opaque)->IsInterrupted();
}

const std::string& CStartupVideoDecoder::GetError() const
{
    return m_Error;
}

int CStartupVideoDecoder::GetWidth() const
{
    return m_pCodec ? m_pCodec->width : 0;
}

int CStartupVideoDecoder::GetHeight() const
{
    return m_pCodec ? m_pCodec->height : 0;
}

bool CStartupVideoDecoder::Open(const char* dllPath, const char* filename, bool (*interrupt)(void*), void* context)
{
    Close();

    if (!dllPath || !*dllPath || !filename || !*filename)
        return Fail("Invalid video or DLL name");

    m_hModule = LoadLibraryA(dllPath);

    if (!m_hModule)
        return Fail("Cannot load metahook/dlls/Mile.FFmpeg/Mile.FFmpeg.dll");

    auto& api = *m_pAPI;

#define LOAD_API(name)                                                                 \
    api.name = reinterpret_cast<decltype(api.name)>(GetProcAddress(m_hModule, #name)); \
    if (!api.name)                                                                     \
        return Fail("Mile.FFmpeg is missing " #name);

    STARTUP_FFMPEG_APIS(LOAD_API)

#undef LOAD_API

    //Struct layouts are ABI-major specific. Do not reject compatible user builds.
    if (AV_VERSION_MAJOR(api.avcodec_version()) != LIBAVCODEC_VERSION_MAJOR ||
        AV_VERSION_MAJOR(api.avformat_version()) != LIBAVFORMAT_VERSION_MAJOR ||
        AV_VERSION_MAJOR(api.avutil_version()) != LIBAVUTIL_VERSION_MAJOR)
    {
        return Fail("Incompatible Mile.FFmpeg ABI (requires FFmpeg 6.x)");
    }

    m_pfnInterrupt      = interrupt;
    m_pInterruptContext = context;

    m_pFormat = api.avformat_alloc_context();

    if (!m_pFormat)
        return Fail("Cannot allocate WebM reader");

    m_pFormat->interrupt_callback = {InterruptCallback, this};

    int result = api.avformat_open_input(&m_pFormat, filename, nullptr, nullptr);

    if (result < 0)
        return Fail("Cannot open startup video", result);

    result = api.avformat_find_stream_info(m_pFormat, nullptr);

    if (result < 0)
        return Fail("Cannot read startup video tracks", result);

    m_StreamIndex = api.av_find_best_stream(m_pFormat, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (m_StreamIndex < 0)
        return Fail("Startup video has no video track", m_StreamIndex);

    const auto stream = m_pFormat->streams[m_StreamIndex];
    const auto id     = stream->codecpar->codec_id;

    if (id != AV_CODEC_ID_VP8 && id != AV_CODEC_ID_VP9)
        return Fail("Startup video must contain VP8 or VP9");

    const auto decoder = api.avcodec_find_decoder(id);

    if (!decoder)
        return Fail("Mile.FFmpeg has no VP8/VP9 decoder");

    m_pCodec = api.avcodec_alloc_context3(decoder);

    if (!m_pCodec)
        return Fail("Cannot allocate video decoder");

    result = api.avcodec_parameters_to_context(m_pCodec, stream->codecpar);

    if (result < 0)
        return Fail("Invalid video parameters", result);

    m_pCodec->thread_count = 8;

    result = api.avcodec_open2(m_pCodec, decoder, nullptr);

    if (result < 0)
        return Fail("Cannot initialize video decoder", result);

    if (m_pCodec->width <= 0 || m_pCodec->height <= 0)
        return Fail("Invalid video dimensions");

    m_pFrame  = api.av_frame_alloc();
    m_pPacket = api.av_packet_alloc();

    if (!m_pFrame || !m_pPacket)
        return Fail("Cannot allocate video frame");

    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
    {
        m_FrameDuration = double(stream->avg_frame_rate.den) / stream->avg_frame_rate.num;
    }

    m_LastSeconds = -m_FrameDuration;

    return true;
}

StartupVideoReadResult CStartupVideoDecoder::Next(CStartupVideoFrameView& output)
{
    output = {};

    if (!m_pCodec || !m_pFrame || !m_pPacket)
    {
        Fail("Video decoder is not open");
        return StartupVideoReadResult::Error;
    }

    auto& api = *m_pAPI;

    api.av_frame_unref(m_pFrame);

    for (;;)
    {
        if (IsInterrupted())
        {
            Fail("Startup video interrupted");
            return StartupVideoReadResult::Error;
        }

        int result = api.avcodec_receive_frame(m_pCodec, m_pFrame);

        if (result == 0)
            break;

        if (result == AVERROR_EOF)
            return StartupVideoReadResult::End;

        if (result != AVERROR(EAGAIN))
        {
            Fail("Video decode failed", result);
            return StartupVideoReadResult::Error;
        }

        if (!m_PacketPending && !m_Draining)
        {
            //Skip packets from non-video streams
            do
            {
                api.av_packet_unref(m_pPacket);
                result = api.av_read_frame(m_pFormat, m_pPacket);
            } while (result >= 0 && m_pPacket->stream_index != m_StreamIndex && !IsInterrupted());

            if (result == AVERROR_EOF)
            {
                m_Draining = true;
            }
            else if (result < 0)
            {
                Fail("Cannot read video packet", result);
                return StartupVideoReadResult::Error;
            }
            else
            {
                m_PacketPending = true;
            }
        }

        result = api.avcodec_send_packet(m_pCodec, m_Draining ? nullptr : m_pPacket);

        if (result < 0)
        {
            //receive just returned EAGAIN; send must accept input or fail.
            Fail("Cannot submit video packet", result);
            return StartupVideoReadResult::Error;
        }

        m_PacketPending = false;
        api.av_packet_unref(m_pPacket);
    }

    switch (m_pFrame->format)
    {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
        {
            output.shiftX = 1;
            output.shiftY = 1;
            break;
        }
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUVJ422P:
        {
            output.shiftX = 1;
            break;
        }
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ444P:
        {
            break;
        }
        default:
        {
            Fail("Unsupported startup video pixel format (requires planar 8-bit YUV)");
            return StartupVideoReadResult::Error;
        }
    }

    output.width  = m_pFrame->width;
    output.height = m_pFrame->height;

    for (int i = 0; i < 3; ++i)
    {
        output.planes[i]  = m_pFrame->data[i];
        output.strides[i] = m_pFrame->linesize[i];

        int w, h;

        if (!output.GetPlaneSize(i, w, h))
        {
            Fail("Invalid decoded video plane");
            return StartupVideoReadResult::Error;
        }
    }

    const auto timestamp = m_pFrame->best_effort_timestamp;

    if (timestamp != AV_NOPTS_VALUE)
    {
        if (m_FirstTimestamp == AV_NOPTS_VALUE)
            m_FirstTimestamp = timestamp;

        const auto timeBase = m_pFormat->streams[m_StreamIndex]->time_base;

        output.seconds = (double(timestamp) - double(m_FirstTimestamp)) * timeBase.num / timeBase.den;
    }
    else
    {
        output.seconds = m_LastSeconds + m_FrameDuration;
    }

    if (!std::isfinite(output.seconds))
    {
        Fail("Invalid video timestamp");
        return StartupVideoReadResult::Error;
    }

    //Timestamps must never go backwards
    output.seconds = std::max(0.0, std::max(m_LastSeconds, output.seconds));

    m_LastSeconds = output.seconds;

    return StartupVideoReadResult::FrameReady;
}

#undef STARTUP_FFMPEG_APIS
