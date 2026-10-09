cmake_minimum_required(VERSION 3.22)

project(Wavis VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(WAVIS_JUCE_TAG "8.0.15" CACHE STRING "Pinned JUCE release tag")
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/${WAVIS_JUCE_TAG}.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(JUCE)

option(WAVIS_ASIO "Enable ASIO. Requires the Steinberg ASIO SDK and a JUCE licence that allows it." OFF)
set(WAVIS_ASIO_SDK "" CACHE PATH "ASIO SDK root, the folder that contains common/iasiodrv.h")

set(WAVIS_HAS_PULSE 0)
if(UNIX AND NOT APPLE)
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PULSE libpulse)
        pkg_check_modules(PULSE_SIMPLE libpulse-simple)
        if(PULSE_FOUND AND PULSE_SIMPLE_FOUND)
            set(WAVIS_HAS_PULSE 1)
        endif()
    endif()
endif()

juce_add_gui_app(Wavis
    PRODUCT_NAME "Wavis"
    COMPANY_NAME "Wavis"
    BUNDLE_ID "com.wavis.app"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
)

target_sources(Wavis PRIVATE
    src/Main.cpp
    src/ui/MainComponent.cpp
    src/ui/VisualView.cpp
    src/engine/AudioEngine.cpp
    src/engine/SystemInputs.cpp
    src/core/OutputFanout.cpp
)

target_include_directories(Wavis PRIVATE src)
target_compile_definitions(Wavis PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="Wavis"
    JUCE_APPLICATION_VERSION_STRING="1.0.0"
    WAVIS_HAS_PULSE=${WAVIS_HAS_PULSE}
)

target_link_libraries(Wavis PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_opengl
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

if(WAVIS_HAS_PULSE)
    target_include_directories(Wavis PRIVATE ${PULSE_INCLUDE_DIRS} ${PULSE_SIMPLE_INCLUDE_DIRS})
    target_link_libraries(Wavis PRIVATE ${PULSE_LIBRARIES} ${PULSE_SIMPLE_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(Wavis PRIVATE ole32 uuid propsys)
endif()

if(WAVIS_ASIO)
    if(NOT EXISTS "${WAVIS_ASIO_SDK}/common/iasiodrv.h")
        message(FATAL_ERROR "WAVIS_ASIO is ON but ${WAVIS_ASIO_SDK}/common/iasiodrv.h was not found")
    endif()
    target_compile_definitions(Wavis PRIVATE JUCE_ASIO=1)
    target_include_directories(Wavis PRIVATE "${WAVIS_ASIO_SDK}/common")
endif()

juce_add_console_app(WavisTests PRODUCT_NAME WavisTests)

target_sources(WavisTests PRIVATE
    tests/Tests.cpp
    src/core/OutputFanout.cpp
)

target_include_directories(WavisTests PRIVATE src)
target_compile_definitions(WavisTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

target_link_libraries(WavisTests PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
