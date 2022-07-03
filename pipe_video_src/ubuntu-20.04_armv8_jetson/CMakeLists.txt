cmake_minimum_required(VERSION 3.23)

# Only interpret if() arguments as variables or keywords when unquoted.
cmake_policy(SET CMP0054 NEW)
# MSVC runtime library flags are selected by an abstraction.
cmake_policy(SET CMP0091 NEW)

set(WEBRTC_INCLUDE_DIR "" CACHE PATH "WebRTC のインクルードディレクトリ")
set(WEBRTC_LIBRARY_DIR "" CACHE PATH "WebRTC のライブラリディレクトリ")
set(WEBRTC_LIBRARY_NAME "webrtc" CACHE STRING "WebRTC のライブラリ名")
set(BOOST_ROOT "" CACHE PATH "Boost のルートディレクトリ")
set(SORA_DIR "" CACHE PATH "Sora のルートディレクトリ")
set(CLI11_DIR "" CACHE PATH "CLI11 のルートディレクトリ")

project(sora-pipe-video-src C CXX)

list(APPEND CMAKE_PREFIX_PATH ${SORA_DIR})
list(APPEND CMAKE_MODULE_PATH ${SORA_DIR}/share/cmake)

set(Boost_USE_STATIC_LIBS ON)

find_package(Boost REQUIRED COMPONENTS json)
find_package(WebRTC REQUIRED)
find_package(Sora REQUIRED)
find_package(Threads REQUIRED)

add_executable(pipe_video_src)
set_target_properties(pipe_video_src PROPERTIES CXX_STANDARD 17 C_STANDARD 17)
set_target_properties(pipe_video_src PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_sources(pipe_video_src PRIVATE ../src/pipe_video_src.cpp ../src/v4l2_video_capturer.cpp ../src/pipe_video_capturer.cpp)

target_compile_options(pipe_video_src
  PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>"
    "$<$<COMPILE_LANGUAGE:CXX>:-isystem${LIBCXX_INCLUDE_DIR}>"
)
target_include_directories(pipe_video_src PRIVATE ${CLI11_DIR}/include)
target_link_libraries(pipe_video_src PRIVATE Sora::sora)
target_link_directories(pipe_video_src PRIVATE ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/tegra)
target_compile_definitions(pipe_video_src PRIVATE CLI11_HAS_FILESYSTEM=0)
