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

set(WAVIS_VISUAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web/dist")
set(WAVIS_VISUAL_JS "${WAVIS_VISUAL_DIR}/app.js")
set(WAVIS_VISUAL_HTML "${WAVIS_VISUAL_DIR}/index.html")
find_program(NPM_EXECUTABLE npm)
if(NPM_EXECUTABLE)
    add_custom_command(
        OUTPUT "${WAVIS_VISUAL_JS}" "${WAVIS_VISUAL_HTML}"
        COMMAND "${NPM_EXECUTABLE}" install --prefix "${CMAKE_CURRENT_SOURCE_DIR}/web"
        COMMAND "${NPM_EXECUTABLE}" run build --prefix "${CMAKE_CURRENT_SOURCE_DIR}/web"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/web/src/main.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/web/src/visual.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/web/package.json"
        COMMENT "Bundling the Three.js visual"
        VERBATIM)
else()
    message(STATUS "npm was not found; the build uses the Three.js bundle already in web/dist")
endif()

juce_add_gui_app(Wavis
    PRODUCT_NAME "Wavis"
    COMPANY_NAME "Wavis"
    BUNDLE_ID "com.wavis.app"
    NEEDS_WEB_BROWSER TRUE
    NEEDS_CURL FALSE
)

target_sources(Wavis PRIVATE
    src/Main.cpp
    src/ui/MainComponent.cpp
    src/ui/VisualView.cpp
    src/ui/VisualHost.cpp
    src/ui/WebStage.cpp
    src/ui/EscapeGrab.cpp
    src/engine/AudioEngine.cpp
    src/engine/SystemInputs.cpp
    src/core/OutputFanout.cpp
)

juce_add_binary_data(WavisVisual SOURCES "${WAVIS_VISUAL_HTML}" "${WAVIS_VISUAL_JS}")

target_include_directories(Wavis PRIVATE src)
target_compile_definitions(Wavis PRIVATE
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="Wavis"
    JUCE_APPLICATION_VERSION_STRING="1.0.0"
    WAVIS_HAS_PULSE=${WAVIS_HAS_PULSE}
)

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
endif()

target_link_libraries(Wavis PRIVATE
    WavisVisual
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_opengl
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

if(UNIX AND NOT APPLE)
    target_link_libraries(Wavis PRIVATE X11::X11)
endif()

if(WIN32)
    set(WAVIS_WEBVIEW2_VERSION "1.0.3485.44")
    set(WAVIS_WEBVIEW2_ROOT "${CMAKE_BINARY_DIR}/webview2")
    if(NOT EXISTS "${WAVIS_WEBVIEW2_ROOT}/build/native/include/WebView2.h")
        file(DOWNLOAD
            "https://globalcdn.nuget.org/packages/microsoft.web.webview2.${WAVIS_WEBVIEW2_VERSION}.nupkg"
            "${CMAKE_BINARY_DIR}/webview2.nupkg"
            STATUS WAVIS_WEBVIEW2_STATUS)
        list(GET WAVIS_WEBVIEW2_STATUS 0 WAVIS_WEBVIEW2_CODE)
        if(NOT WAVIS_WEBVIEW2_CODE EQUAL 0)
            message(FATAL_ERROR "Could not download the WebView2 SDK (${WAVIS_WEBVIEW2_STATUS})")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/webview2.nupkg" DESTINATION "${WAVIS_WEBVIEW2_ROOT}")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(WAVIS_WEBVIEW2_ARCH arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(WAVIS_WEBVIEW2_ARCH x64)
    else()
        set(WAVIS_WEBVIEW2_ARCH x86)
    endif()
    target_compile_definitions(Wavis PRIVATE JUCE_USE_WIN_WEBVIEW2=1)
    target_include_directories(Wavis PRIVATE "${WAVIS_WEBVIEW2_ROOT}/build/native/include")
    add_custom_command(TARGET Wavis POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${WAVIS_WEBVIEW2_ROOT}/build/native/${WAVIS_WEBVIEW2_ARCH}/WebView2Loader.dll"
            "$<TARGET_FILE_DIR:Wavis>"
        VERBATIM)
endif()

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
