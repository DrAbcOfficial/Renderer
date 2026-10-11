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

namespace StartupVideo
{
Rect DestinationRect(int videoWidth, int videoHeight, int windowWidth, int windowHeight)
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

bool PlaneSize(const Frame& frame, int plane, int& width, int& height)
{
    if (plane < 0 || plane >= 3)
        return false;

    if (frame.width <= 0 || frame.height <= 0)
        return false;

    if (frame.shiftX < 0 || frame.shiftX > 1)
        return false;

    if (frame.shiftY < 0 || frame.shiftY > frame.shiftX)
        return false;

    const int x = plane ? frame.shiftX : 0;
    const int y = plane ? frame.shiftY : 0;

    //Round up for odd dimensions
    width  = (frame.width >> x) + (x && (frame.width & 1));
    height = (frame.height >> y) + (y && (frame.height & 1));

    if (!frame.planes[plane])
        return false;

    //Negative stride is valid: planes point to the first displayed row.
    const auto stride = std::abs(std::int64_t(frame.strides[plane]));

    if (stride < width)
        return false;

    if (stride * (height - 1) > PTRDIFF_MAX)
        return false;

    return true;
}

bool IsFrameDue(double elapsed, double timestamp)
{
    if (!std::isfinite(elapsed) || !std::isfinite(timestamp))
        return false;

    return elapsed >= timestamp;
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

struct Decoder::Impl
{
    HMODULE module = nullptr;

#define DECLARE_API(name) decltype(&::name) name = nullptr;
    STARTUP_FFMPEG_APIS(DECLARE_API)
#undef DECLARE_API

    AVFormatContext* format = nullptr;
    AVCodecContext*  codec  = nullptr;
    AVFrame*         frame  = nullptr;
    AVPacket*        packet = nullptr;

    int  stream        = -1;
    bool draining      = false;
    bool packetPending = false;

    bool (*interrupt)(void*) = nullptr;
    void* context            = nullptr;

    std::int64_t firstTimestamp = AV_NOPTS_VALUE;
    double       lastSeconds    = 0;
    double       frameDuration  = 1.0 / 30.0;

    std::string error;

    ~Impl()
    {
        if (packet)
            av_packet_free(&packet);

        if (frame)
            av_frame_free(&frame);

        if (codec)
            avcodec_free_context(&codec);

        if (format)
            avformat_close_input(&format);

        if (module)
            FreeLibrary(module);
    }

    bool Fail(const char* operation, int result = 0)
    {
        error = operation;

        if (result && av_strerror)
        {
            char text[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(result, text, sizeof(text));

            error += ": ";
            error += text;
        }

        return false;
    }

    static int Interrupted(void* opaque)
    {
        const auto self = static_cast<Impl*>(opaque);

        return self->interrupt && self->interrupt(self->context);
    }
};

Decoder::Decoder() : impl(std::make_unique<Impl>())
{
}

Decoder::~Decoder() = default;

const std::string& Decoder::Error() const
{
    return impl->error;
}

int Decoder::Width() const
{
    return impl->codec ? impl->codec->width : 0;
}

int Decoder::Height() const
{
    return impl->codec ? impl->codec->height : 0;
}

bool Decoder::Open(const wchar_t* dllPath, const char* filename, bool (*interrupt)(void*), void* context)
{
    impl = std::make_unique<Impl>();

    auto& s = *impl;

    if (!dllPath || !*dllPath || !filename || !*filename)
        return s.Fail("Invalid video or DLL name");

    s.module = LoadLibraryW(dllPath);

    if (!s.module)
        return s.Fail("Cannot load metahook/dlls/Mile.FFmpeg/Mile.FFmpeg.dll");

#define LOAD_API(name)                                                            \
    s.name = reinterpret_cast<decltype(s.name)>(GetProcAddress(s.module, #name)); \
    if (!s.name)                                                                  \
        return s.Fail("Mile.FFmpeg is missing " #name);

    STARTUP_FFMPEG_APIS(LOAD_API)

#undef LOAD_API

    //Struct layouts are ABI-major specific. Do not reject compatible user builds.
    if (AV_VERSION_MAJOR(s.avcodec_version()) != LIBAVCODEC_VERSION_MAJOR ||
        AV_VERSION_MAJOR(s.avformat_version()) != LIBAVFORMAT_VERSION_MAJOR ||
        AV_VERSION_MAJOR(s.avutil_version()) != LIBAVUTIL_VERSION_MAJOR)
    {
        return s.Fail("Incompatible Mile.FFmpeg ABI (requires FFmpeg 6.x)");
    }

    s.interrupt = interrupt;
    s.context   = context;

    s.format = s.avformat_alloc_context();

    if (!s.format)
        return s.Fail("Cannot allocate WebM reader");

    s.format->interrupt_callback = {Impl::Interrupted, &s};

    int result = s.avformat_open_input(&s.format, filename, nullptr, nullptr);

    if (result < 0)
        return s.Fail("Cannot open startup video", result);

    result = s.avformat_find_stream_info(s.format, nullptr);

    if (result < 0)
        return s.Fail("Cannot read startup video tracks", result);

    s.stream = s.av_find_best_stream(s.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (s.stream < 0)
        return s.Fail("Startup video has no video track", s.stream);

    const auto stream = s.format->streams[s.stream];
    const auto id     = stream->codecpar->codec_id;

    if (id != AV_CODEC_ID_VP8 && id != AV_CODEC_ID_VP9)
        return s.Fail("Startup video must contain VP8 or VP9");

    const auto decoder = s.avcodec_find_decoder(id);

    if (!decoder)
        return s.Fail("Mile.FFmpeg has no VP8/VP9 decoder");

    s.codec = s.avcodec_alloc_context3(decoder);

    if (!s.codec)
        return s.Fail("Cannot allocate video decoder");

    result = s.avcodec_parameters_to_context(s.codec, stream->codecpar);

    if (result < 0)
        return s.Fail("Invalid video parameters", result);

    s.codec->thread_count = 8;

    result = s.avcodec_open2(s.codec, decoder, nullptr);

    if (result < 0)
        return s.Fail("Cannot initialize video decoder", result);

    if (s.codec->width <= 0 || s.codec->height <= 0)
        return s.Fail("Invalid video dimensions");

    s.frame  = s.av_frame_alloc();
    s.packet = s.av_packet_alloc();

    if (!s.frame || !s.packet)
        return s.Fail("Cannot allocate video frame");

    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
    {
        s.frameDuration = double(stream->avg_frame_rate.den) / stream->avg_frame_rate.num;
    }

    s.lastSeconds = -s.frameDuration;

    return true;
}

ReadResult Decoder::Next(Frame& output)
{
    auto& s = *impl;

    output = {};

    if (!s.codec || !s.frame || !s.packet)
    {
        s.Fail("Video decoder is not open");
        return ReadResult::Error;
    }

    s.av_frame_unref(s.frame);

    for (;;)
    {
        if (Impl::Interrupted(&s))
        {
            s.Fail("Startup video interrupted");
            return ReadResult::Error;
        }

        int result = s.avcodec_receive_frame(s.codec, s.frame);

        if (result == 0)
            break;

        if (result == AVERROR_EOF)
            return ReadResult::End;

        if (result != AVERROR(EAGAIN))
        {
            s.Fail("Video decode failed", result);
            return ReadResult::Error;
        }

        if (!s.packetPending && !s.draining)
        {
            //Skip packets from non-video streams
            do
            {
                s.av_packet_unref(s.packet);
                result = s.av_read_frame(s.format, s.packet);
            } while (result >= 0 && s.packet->stream_index != s.stream && !Impl::Interrupted(&s));

            if (result == AVERROR_EOF)
            {
                s.draining = true;
            }
            else if (result < 0)
            {
                s.Fail("Cannot read video packet", result);
                return ReadResult::Error;
            }
            else
            {
                s.packetPending = true;
            }
        }

        result = s.avcodec_send_packet(s.codec, s.draining ? nullptr : s.packet);

        if (result < 0)
        {
            //receive just returned EAGAIN; send must accept input or fail.
            s.Fail("Cannot submit video packet", result);
            return ReadResult::Error;
        }

        s.packetPending = false;
        s.av_packet_unref(s.packet);
    }

    switch (s.frame->format)
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
            s.Fail("Unsupported startup video pixel format (requires planar 8-bit YUV)");
            return ReadResult::Error;
        }
    }

    output.width  = s.frame->width;
    output.height = s.frame->height;

    for (int i = 0; i < 3; ++i)
    {
        output.planes[i]  = s.frame->data[i];
        output.strides[i] = s.frame->linesize[i];

        int w, h;

        if (!PlaneSize(output, i, w, h))
        {
            s.Fail("Invalid decoded video plane");
            return ReadResult::Error;
        }
    }

    const auto timestamp = s.frame->best_effort_timestamp;

    if (timestamp != AV_NOPTS_VALUE)
    {
        if (s.firstTimestamp == AV_NOPTS_VALUE)
            s.firstTimestamp = timestamp;

        const auto timeBase = s.format->streams[s.stream]->time_base;

        output.seconds = (double(timestamp) - double(s.firstTimestamp)) * timeBase.num / timeBase.den;
    }
    else
    {
        output.seconds = s.lastSeconds + s.frameDuration;
    }

    if (!std::isfinite(output.seconds))
    {
        s.Fail("Invalid video timestamp");
        return ReadResult::Error;
    }

    //Timestamps must never go backwards
    output.seconds = std::max(0.0, std::max(s.lastSeconds, output.seconds));

    s.lastSeconds = output.seconds;

    return ReadResult::FrameReady;
}

#undef STARTUP_FFMPEG_APIS
} // namespace StartupVideo
