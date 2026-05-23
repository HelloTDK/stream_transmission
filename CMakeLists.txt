cmake_minimum_required(VERSION 3.16)

project(weaknet_webrtc_image_transport
    VERSION 0.1.0
    DESCRIPTION "弱网高丢包远距离图传 WebRTC/GStreamer 示例工程"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GST REQUIRED
    gstreamer-1.0>=1.18
    gstreamer-webrtc-1.0>=1.18
    gstreamer-sdp-1.0>=1.18
    gstreamer-app-1.0>=1.18
    gstreamer-video-1.0>=1.18
    json-glib-1.0>=1.4
    glib-2.0>=2.56
    gobject-2.0>=2.56)

add_executable(weaknet_webrtc
    src/main.cpp
    src/adaptive/AdaptiveController.cpp
    src/app/Application.cpp
    src/config/Config.cpp
    src/media/GuardFrameChannel.cpp
    src/media/ReceiverSession.cpp
    src/media/SenderSession.cpp
    src/signaling/ConsoleSignalingClient.cpp
    src/util/Json.cpp
    src/util/Logger.cpp)

target_include_directories(weaknet_webrtc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GST_INCLUDE_DIRS})

target_link_directories(weaknet_webrtc PRIVATE ${GST_LIBRARY_DIRS})
target_link_libraries(weaknet_webrtc PRIVATE ${GST_LIBRARIES})
target_compile_options(weaknet_webrtc PRIVATE ${GST_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)

install(TARGETS weaknet_webrtc RUNTIME DESTINATION bin)
install(FILES config/default.yaml DESTINATION etc/weaknet_webrtc)
