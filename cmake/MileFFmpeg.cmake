include_guard(GLOBAL)
set(MILE_FFMPEG_ROOT "$ENV{MILE_FFMPEG_ROOT}" CACHE PATH "Mile.FFmpeg 6.1.0.0 binary SDK; empty downloads the verified release")

# Shared binary SDK, not an import-library dependency of Renderer. The aggregate
# calls this same helper so standalone and aggregate builds use identical bits.
function(renderer_prepare_mile_ffmpeg out_root)
    if(MILE_FFMPEG_ROOT)
        get_filename_component(root "${MILE_FFMPEG_ROOT}" ABSOLUTE)
    else()
        include(FetchContent)
        if(POLICY CMP0135)
            cmake_policy(SET CMP0135 NEW)
        endif()
        FetchContent_Declare(renderer_mile_ffmpeg
            URL https://github.com/ProjectMileArchived/Mile.FFmpeg/releases/download/6.1.0.0/Mile.FFmpeg.zip
            URL_HASH SHA256=17425bd7cd737aa956daa9d6cb3cdff9b05fe6269634b06981e78f331dfd0272
            SOURCE_SUBDIR Include)
        FetchContent_MakeAvailable(renderer_mile_ffmpeg)
        set(root "${renderer_mile_ffmpeg_SOURCE_DIR}")
    endif()
    foreach(file Include/libavcodec/avcodec.h Include/libavformat/avformat.h
                 Include/libavutil/ffversion.h Redist/x86/Mile.FFmpeg.dll License.txt)
        if(NOT EXISTS "${root}/${file}")
            message(FATAL_ERROR "MILE_FFMPEG_ROOT is missing ${file}: ${root}")
        endif()
    endforeach()
    set(${out_root} "${root}" PARENT_SCOPE)
endfunction()
