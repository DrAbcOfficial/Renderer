set(RENDERER_DEPENDENCY_CACHE_DIR "${PROJECT_SOURCE_DIR}/thirdparty/cache" CACHE PATH "Downloaded dependency cache")
set(VC_LTL_Root "${RENDERER_DEPENDENCY_CACHE_DIR}/VC-LTL-5.3.1" CACHE PATH "VC-LTL binary package root")
set(METAHOOK_SOURCE_PATH "$ENV{METAHOOK_SOURCE_PATH}" CACHE PATH "MetaHook source tree; empty fetches the pinned SDK")
set(VGUI2EXTENSION_SOURCE_PATH "$ENV{VGUI2EXTENSION_SOURCE_PATH}" CACHE PATH "VGUI2Extension source tree providing its public interface headers; empty fetches the pinned commit")
set(UTILTHREADTASK_SOURCE_PATH "$ENV{UTILTHREADTASK_SOURCE_PATH}" CACHE PATH "UtilThreadTask source tree providing its public interface header; empty fetches the pinned commit")
set(FREEIMAGE_SOURCE_PATH "$ENV{FREEIMAGE_SOURCE_PATH}" CACHE PATH "FreeImage source tree with CMakeLists.txt and Source/FreeImage.h; empty fetches the pinned commit")
set(GLEW_SOURCE_PATH "$ENV{GLEW_SOURCE_PATH}" CACHE PATH "glew-cmake source tree providing libglew_static; empty fetches the pinned commit")
set(SCOPEEXIT_SOURCE_PATH "$ENV{SCOPEEXIT_SOURCE_PATH}" CACHE PATH "ScopeExit source tree; empty fetches the pinned commit")
set(TINYOBJLOADER_SOURCE_PATH "$ENV{TINYOBJLOADER_SOURCE_PATH}" CACHE PATH "tinyobjloader source tree; empty fetches the pinned commit")
set(CAPSTONE_INCLUDE_DIRS "$ENV{CAPSTONE_INCLUDE_DIRS}" CACHE STRING "External Capstone include directories; empty prefers MetaHook's own capstone fork and otherwise fetches the pinned commit")
set(SDL2_INCLUDE_DIRS "$ENV{SDL2_INCLUDE_DIRS}" CACHE STRING "External include directories containing SDL2/SDL_video.h (required)")
set(SDL3_INCLUDE_DIRS "$ENV{SDL3_INCLUDE_DIRS}" CACHE STRING "External include directories containing SDL3/SDL.h (optional)")
include("${CMAKE_CURRENT_LIST_DIR}/MileFFmpeg.cmake")

# Download a pinned dependency into the build tree. Only the source is
# populated: the caller keeps its add_subdirectory() call so subprojects are
# configured at the same point and with the same options as an explicit path.
function(renderer_fetch_source name url tag out_var)
    include(FetchContent)
    FetchContent_Populate(${name}
        GIT_REPOSITORY "${url}"
        GIT_TAG "${tag}"
        GIT_SUBMODULES ""
        GIT_SUBMODULES_RECURSE FALSE
        SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/${name}-src")
    string(TOLOWER "${name}" name_lower)
    set(${out_var} "${${name_lower}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# Generic external-source file validation (used for shared ScopeExit).
function(renderer_validate_source variable source)
    foreach(required IN LISTS ARGN)
        if(NOT EXISTS "${source}/${required}" OR IS_DIRECTORY "${source}/${required}")
            message(FATAL_ERROR "${variable} is missing ${required}: ${source}")
        endif()
    endforeach()
endfunction()

function(renderer_validate_vgui2extension_source source)
    foreach(required include/Interface/IVGUI2Extension.h include/Interface/IDpiManager.h
        include/Interface/VGUI/IInput2.h include/Interface/VGUI/IScheme2.h include/Interface/VGUI/ISurface2.h)
        if(NOT EXISTS "${source}/${required}" OR IS_DIRECTORY "${source}/${required}")
            message(FATAL_ERROR "VGUI2EXTENSION_SOURCE_PATH is missing ${required}: ${source}")
        endif()
    endforeach()
endfunction()

function(renderer_validate_utilthreadtask_source source)
    if(NOT EXISTS "${source}/include/Interface/IUtilThreadTask.h" OR IS_DIRECTORY "${source}/include/Interface/IUtilThreadTask.h")
        message(FATAL_ERROR "UTILTHREADTASK_SOURCE_PATH is missing include/Interface/IUtilThreadTask.h: ${source}")
    endif()
endfunction()

function(renderer_prepare_dependencies)
    renderer_prepare_mile_ffmpeg(mile_ffmpeg_root)
    set(RENDERER_MILE_FFMPEG_ROOT "${mile_ffmpeg_root}" PARENT_SCOPE)
    set(RENDERER_MILE_FFMPEG_INCLUDE_DIRS "${mile_ffmpeg_root}/Include" PARENT_SCOPE)
    # Validate explicit paths before doing any downloads. External trees are read-only inputs.
    if(NOT SDL2_INCLUDE_DIRS)
        message(FATAL_ERROR "Set SDL2_INCLUDE_DIRS to the include directory provided by MetaHook's SDL build (containing SDL2/SDL_video.h). Renderer does not fetch or build SDL.")
    endif()
    foreach(version 2 3)
        set(sdl_includes)
        set(sdl_header_found FALSE)
        if(version EQUAL 2)
            set(sdl_header SDL2/SDL_video.h)
        else()
            set(sdl_header SDL3/SDL.h)
        endif()
        foreach(directory IN LISTS SDL${version}_INCLUDE_DIRS)
            get_filename_component(directory "${directory}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
            if(NOT IS_DIRECTORY "${directory}")
                message(FATAL_ERROR "SDL${version}_INCLUDE_DIRS directory does not exist: ${directory}")
            endif()
            if(EXISTS "${directory}/${sdl_header}")
                set(sdl_header_found TRUE)
            endif()
            list(APPEND sdl_includes "${directory}")
        endforeach()
        if(SDL${version}_INCLUDE_DIRS AND NOT sdl_header_found)
            message(FATAL_ERROR "SDL${version}_INCLUDE_DIRS must provide ${sdl_header}")
        endif()
        set(RENDERER_SDL${version}_INCLUDE_DIRS "${sdl_includes}" PARENT_SCOPE)
    endforeach()
    set(capstone_includes)
    set(capstone_header_found FALSE)
    foreach(directory IN LISTS CAPSTONE_INCLUDE_DIRS)
        get_filename_component(directory "${directory}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
        if(NOT IS_DIRECTORY "${directory}")
            message(FATAL_ERROR "CAPSTONE_INCLUDE_DIRS directory does not exist: ${directory}")
        endif()
        # Accept either include/ or include/capstone/ from a Capstone source/install tree.
        if(EXISTS "${directory}/capstone.h")
            set(capstone_header_found TRUE)
        elseif(EXISTS "${directory}/capstone/capstone.h")
            string(APPEND directory "/capstone")
            set(capstone_header_found TRUE)
        endif()
        list(APPEND capstone_includes "${directory}")
    endforeach()
    if(CAPSTONE_INCLUDE_DIRS AND NOT capstone_header_found)
        message(FATAL_ERROR "CAPSTONE_INCLUDE_DIRS must provide capstone.h or capstone/capstone.h")
    endif()
    if(DEFINED CAPSTONE_LIBRARY_DIRS)
        message(STATUS "CAPSTONE_LIBRARY_DIRS is ignored: Renderer uses Capstone headers through the MetaHook API and does not link Capstone.")
    endif()
    if(VGUI2EXTENSION_SOURCE_PATH)
        get_filename_component(vgui2extension_source "${VGUI2EXTENSION_SOURCE_PATH}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
        renderer_validate_vgui2extension_source("${vgui2extension_source}")
    endif()
    if(UTILTHREADTASK_SOURCE_PATH)
        get_filename_component(utilthreadtask_source "${UTILTHREADTASK_SOURCE_PATH}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
        renderer_validate_utilthreadtask_source("${utilthreadtask_source}")
    endif()
    if(METAHOOK_SOURCE_PATH)
        get_filename_component(metahook_source "${METAHOOK_SOURCE_PATH}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    else()
        include(FetchContent)
        FetchContent_Declare(renderer_metahook
            GIT_REPOSITORY https://github.com/MetaHookSv/MetaHook
            # MetaHook is tracked as a branch: always fetch the latest main.
            GIT_TAG origin/main
            GIT_SUBMODULES ""
            GIT_SUBMODULES_RECURSE FALSE
            # This SDK directory has no CMakeLists.txt: populate without building the launcher.
            SOURCE_SUBDIR include
        )
        FetchContent_MakeAvailable(renderer_metahook)
        set(metahook_source "${renderer_metahook_SOURCE_DIR}")
    endif()
    foreach(required include/metahook.h include/HLSDK/common/interface.cpp include/SourceSDK/filesystem.cpp include/vgui_controls/Panel.cpp)
        if(NOT EXISTS "${metahook_source}/${required}")
            message(FATAL_ERROR "METAHOOK_SOURCE_PATH is missing ${required}: ${metahook_source}")
        endif()
    endforeach()
    set(METAHOOK_SOURCE_PATH "${metahook_source}" PARENT_SCOPE)
    message(STATUS "METAHOOK_SOURCE_PATH: ${metahook_source}")

    if(NOT VGUI2EXTENSION_SOURCE_PATH)
        renderer_fetch_source(renderer_vgui2extension
            "https://github.com/MetaHookSv/VGUI2Extension"
            "cd7ef6e3b7fb51d3c98e6d7dadec02dd1fa08c4f" vgui2extension_source)
        renderer_validate_vgui2extension_source("${vgui2extension_source}")
    endif()
    set(VGUI2EXTENSION_SOURCE_PATH "${vgui2extension_source}" PARENT_SCOPE)
    message(STATUS "VGUI2EXTENSION_SOURCE_PATH: ${vgui2extension_source}")

    if(NOT UTILTHREADTASK_SOURCE_PATH)
        renderer_fetch_source(renderer_utilthreadtask
            "https://github.com/MetaHookSv/UtilThreadTask"
            "8d36bef696f95b4932cd2d58d50dd8793567dda8" utilthreadtask_source)
        renderer_validate_utilthreadtask_source("${utilthreadtask_source}")
    endif()
    set(UTILTHREADTASK_SOURCE_PATH "${utilthreadtask_source}" PARENT_SCOPE)
    message(STATUS "UTILTHREADTASK_SOURCE_PATH: ${utilthreadtask_source}")

    if(FREEIMAGE_SOURCE_PATH)
        get_filename_component(source "${FREEIMAGE_SOURCE_PATH}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    else()
        renderer_fetch_source(renderer_freeimage
            "https://github.com/hzqst/FreeImage_clone"
            "007c9e4c5d4198a1646b6c5274fd855be9cca7ef" source)
    endif()
    if(NOT EXISTS "${source}/CMakeLists.txt" OR NOT EXISTS "${source}/Source/FreeImage.h")
        message(FATAL_ERROR "FreeImage source tree (FREEIMAGE_SOURCE_PATH or FetchContent) must contain CMakeLists.txt and Source/FreeImage.h: ${source}")
    endif()
    set(FREEIMAGE_SOURCE_PATH "${source}" PARENT_SCOPE)
    message(STATUS "FREEIMAGE_SOURCE_PATH: ${source}")
    if(NOT CAPSTONE_INCLUDE_DIRS)
        # Prefer the host's own Capstone checkout: these headers describe types
        # crossing the MetaHook API boundary and must match the instance the host
        # loads at runtime. A host fetched without submodules leaves
        # thirdparty/capstone_fork empty, so fall back to the pinned commit.
        set(host_capstone "${metahook_source}/thirdparty/capstone_fork/include/capstone")
        if(EXISTS "${host_capstone}/capstone.h")
            set(capstone_includes "${host_capstone}")
        else()
            renderer_fetch_source(renderer_capstone
                "https://github.com/hzqst/capstone"
                "e81e390f621ee59d14f70e16fe065dd00f78ee71" capstone_source)
            set(capstone_includes "${capstone_source}/include/capstone")
        endif()
    endif()
    set(RENDERER_CAPSTONE_INCLUDE_DIRS "${capstone_includes}" PARENT_SCOPE)
    message(STATUS "Capstone headers: ${capstone_includes}")
    if(GLEW_SOURCE_PATH)
        get_filename_component(glew_source "${GLEW_SOURCE_PATH}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    else()
        renderer_fetch_source(renderer_glew
            "https://github.com/hzqst/glew-cmake"
            "56ed32d4a929f993f0e6b7f905af9be4d38fda04" glew_source)
    endif()
    if(NOT EXISTS "${glew_source}/CMakeLists.txt" OR NOT EXISTS "${glew_source}/include/GL/glew.h")
        message(FATAL_ERROR "GLEW source tree (GLEW_SOURCE_PATH or FetchContent) must contain CMakeLists.txt and include/GL/glew.h: ${glew_source}")
    endif()
    set(GLEW_SOURCE_PATH "${glew_source}" PARENT_SCOPE)
    message(STATUS "GLEW_SOURCE_PATH: ${glew_source}")
    # ScopeExit: shared external tree, otherwise fetch the pinned commit.
    if(SCOPEEXIT_SOURCE_PATH)
        get_filename_component(scopeexit_source "${SCOPEEXIT_SOURCE_PATH}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    else()
        renderer_fetch_source(renderer_scopeexit
            "https://github.com/SergiusTheBest/ScopeExit"
            "bd345da594a4675d04de663d93d00cb81b6678b2" scopeexit_source)
    endif()
    renderer_validate_source(SCOPEEXIT_SOURCE_PATH "${scopeexit_source}" include/ScopeExit/ScopeExit.h)
    set(RENDERER_SCOPEEXIT_INCLUDE_DIRS "${scopeexit_source}/include" PARENT_SCOPE)
    set(SCOPEEXIT_SOURCE_PATH "${scopeexit_source}" PARENT_SCOPE)
    message(STATUS "SCOPEEXIT_SOURCE_PATH: ${scopeexit_source}")
    # tinyobjloader: shared external tree, otherwise fetch the pinned commit.
    if(TINYOBJLOADER_SOURCE_PATH)
        get_filename_component(tinyobjloader_source "${TINYOBJLOADER_SOURCE_PATH}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    else()
        renderer_fetch_source(renderer_tinyobjloader
            "https://github.com/hzqst/tinyobjloader"
            "cab4ad7254cbf7eaaafdb73d272f99e92f166df8" tinyobjloader_source)
    endif()
    renderer_validate_source(TINYOBJLOADER_SOURCE_PATH "${tinyobjloader_source}" tiny_obj_loader.cc tiny_obj_loader.h)
    set(RENDERER_TINYOBJLOADER_INCLUDE_DIRS "${tinyobjloader_source}" PARENT_SCOPE)
    set(TINYOBJLOADER_SOURCE_PATH "${tinyobjloader_source}" PARENT_SCOPE)
    message(STATUS "TINYOBJLOADER_SOURCE_PATH: ${tinyobjloader_source}")
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VCLTL.cmake")
    renderer_prepare_vcltl()
endfunction()
