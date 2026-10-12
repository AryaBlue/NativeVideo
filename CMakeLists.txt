cmake_minimum_required(VERSION 3.16)
project(nativevideo C CXX)

# Builds one shared library, using whichever OS video API this platform has.
# Output: nativevideo.dll / libnativevideo.dylib / libnativevideo.so

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if(WIN32)
    add_library(nativevideo SHARED src/nv_common.c src/nv_mediafoundation.cpp)
    target_link_libraries(nativevideo PRIVATE mfplat mfreadwrite mfuuid ole32 uuid)
    set_target_properties(nativevideo PROPERTIES PREFIX "")
    if(MINGW)
        # Don't make users ship libstdc++/libgcc DLLs alongside the game.
        target_link_options(nativevideo PRIVATE -static -static-libgcc -static-libstdc++ -s)
    endif()
elseif(APPLE)
    enable_language(OBJC)
    add_library(nativevideo SHARED src/nv_common.c src/nv_avfoundation.m)
    target_compile_options(nativevideo PRIVATE -fobjc-arc)
    target_link_libraries(nativevideo PRIVATE
        "-framework Foundation" "-framework AVFoundation"
        "-framework CoreMedia" "-framework CoreVideo")
    # Universal binary: configure with -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GST REQUIRED IMPORTED_TARGET
        gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
    add_library(nativevideo SHARED src/nv_common.c src/nv_gstreamer.c)
    target_link_libraries(nativevideo PRIVATE PkgConfig::GST)
endif()

target_include_directories(nativevideo PRIVATE src)

# Copy the built library next to main.lua so `love .` finds it.
add_custom_command(TARGET nativevideo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:nativevideo> ${CMAKE_SOURCE_DIR}/)
