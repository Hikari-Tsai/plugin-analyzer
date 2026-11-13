cmake_minimum_required(VERSION 3.22)

# Release builds pass the tag's version (v1.2.3 -> -DPLUGIN_ANALYZER_VERSION=1.2.3).
set(PLUGIN_ANALYZER_VERSION "0.1.0" CACHE STRING "Version of the app and plugins (X.Y.Z)")
project(PluginAnalyzer VERSION ${PLUGIN_ANALYZER_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if JUCE_DIR is given (or ~/JUCE exists),
# otherwise download the pinned release.
# ---------------------------------------------------------------------------
set(JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")

if(NOT JUCE_DIR AND EXISTS "$ENV{HOME}/JUCE/CMakeLists.txt")
    set(JUCE_DIR "$ENV{HOME}/JUCE")
endif()

if(JUCE_DIR)
    message(STATUS "Using JUCE from ${JUCE_DIR}")
    add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.6
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# Sources shared by the app and the test runner
# ---------------------------------------------------------------------------
set(CORE_SOURCES
    Source/analysis/Dsp.cpp
    Source/analysis/Measurements.cpp
    Source/host/PluginChain.cpp
    Source/host/BuiltinProcessors.cpp
)

set(UI_SOURCES
    Source/core/AnalyzerCore.cpp
    Source/core/PlaybackRouter.cpp
    Source/live/LiveServer.cpp
    Source/api/AnalyzerApi.cpp
    Source/MainComponent.cpp
    Source/engine/AnalysisEngine.cpp
    Source/ui/PlotComponents.cpp
    Source/ui/ChainPanel.cpp
    Source/ui/AnalysisTabs.cpp
    Source/ui/SettingsComponent.cpp
)

set(COMMON_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    PLUGIN_ANALYZER_VERSION_STRING="${PROJECT_VERSION}"
)

set(COMPANY "HMusic")
set(COPYRIGHT "Copyright (c) 2026 HMusic")

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
option(PLUGIN_ANALYZER_BUILD_APP "Build the GUI application" ON)

if(PLUGIN_ANALYZER_BUILD_APP)
juce_add_gui_app(PluginAnalyzer
    PRODUCT_NAME "Plugin Analyzer"
    COMPANY_NAME "${COMPANY}"
    COMPANY_COPYRIGHT "${COPYRIGHT}"
    BUNDLE_ID "com.hmusic.pluginanalyzer"
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"   # .icns / .ico are generated from this
    PLUGINHOST_AU TRUE
    MICROPHONE_PERMISSION_ENABLED FALSE)

target_sources(PluginAnalyzer PRIVATE ${CORE_SOURCES} ${UI_SOURCES} Source/Main.cpp)
target_include_directories(PluginAnalyzer PRIVATE Source)

target_compile_definitions(PluginAnalyzer PRIVATE
    ${COMMON_DEFINITIONS}
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PluginAnalyzer,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PluginAnalyzer,JUCE_VERSION>")

target_link_libraries(PluginAnalyzer
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
endif()

# ---------------------------------------------------------------------------
# Plugin version: AAX + VST3 on both platforms, AU on macOS.
# AAX uses the AAX SDK bundled with JUCE 8; the result is unsigned (sign it with PACE wraptool).
# ---------------------------------------------------------------------------
option(PLUGIN_ANALYZER_BUILD_PLUGIN "Build the AAX/VST3/AU plugin" ON)

if(PLUGIN_ANALYZER_BUILD_PLUGIN)
    set(PLUGIN_FORMATS VST3 AAX)
    if(APPLE)
        list(APPEND PLUGIN_FORMATS AU)
    endif()

    juce_add_plugin(PluginAnalyzerPlugin
        PRODUCT_NAME "Plugin Analyzer"
        COMPANY_NAME "${COMPANY}"
        COMPANY_COPYRIGHT "${COPYRIGHT}"
        BUNDLE_ID "com.hmusic.pluginanalyzer.plugin"
        VERSION ${PROJECT_VERSION}
        DESCRIPTION "Measures and analyses audio plugins"
        PLUGIN_MANUFACTURER_CODE Hmsc
        PLUGIN_CODE Plan
        FORMATS ${PLUGIN_FORMATS}
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
        PLUGINHOST_AU TRUE
        VST3_CATEGORIES Fx Analyzer
        AU_MAIN_TYPE kAudioUnitType_Effect
        AAX_CATEGORY None)

    target_sources(PluginAnalyzerPlugin PRIVATE ${CORE_SOURCES} ${UI_SOURCES} Source/plugin/PluginProcessor.cpp)
    target_include_directories(PluginAnalyzerPlugin PRIVATE Source)

    target_compile_definitions(PluginAnalyzerPlugin PUBLIC
        ${COMMON_DEFINITIONS}
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(PluginAnalyzerPlugin
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()

# ---------------------------------------------------------------------------
# plugin-analyzer-cli: headless measurements with JSON output (see docs/CLI.md).
# The same JSON API (Source/api) is meant to back an MCP server.
# ---------------------------------------------------------------------------
option(PLUGIN_ANALYZER_BUILD_CLI "Build plugin-analyzer-cli" ON)

if(PLUGIN_ANALYZER_BUILD_CLI)
    juce_add_console_app(PluginAnalyzerCli
        PRODUCT_NAME "plugin-analyzer-cli"
        COMPANY_NAME "${COMPANY}"
        COMPANY_COPYRIGHT "${COPYRIGHT}"
        BUNDLE_ID "com.hmusic.pluginanalyzer.cli"
        VERSION ${PROJECT_VERSION}
        PLUGINHOST_AU TRUE)
    set_target_properties(PluginAnalyzerCli PROPERTIES OUTPUT_NAME "plugin-analyzer-cli")   # console apps ignore PRODUCT_NAME
    target_sources(PluginAnalyzerCli PRIVATE ${CORE_SOURCES} Source/api/AnalyzerApi.cpp Source/cli/CliMain.cpp Source/cli/McpServer.cpp)
    target_include_directories(PluginAnalyzerCli PRIVATE Source)
    target_compile_definitions(PluginAnalyzerCli PRIVATE ${COMMON_DEFINITIONS})
    target_link_libraries(PluginAnalyzerCli
        PRIVATE
            juce::juce_audio_processors
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()

# ---------------------------------------------------------------------------
# Headless test runner: verifies the measurement maths with built-in processors
# ---------------------------------------------------------------------------
option(PLUGIN_ANALYZER_BUILD_TESTS "Build the analysis test runner" ON)

if(PLUGIN_ANALYZER_BUILD_TESTS)
    juce_add_console_app(PluginAnalyzerTests
        PRODUCT_NAME "PluginAnalyzerTests"
        COMPANY_NAME "${COMPANY}"
        COMPANY_COPYRIGHT "${COPYRIGHT}"
        BUNDLE_ID "com.hmusic.pluginanalyzer.tests")
    target_sources(PluginAnalyzerTests PRIVATE ${CORE_SOURCES} Tests/AnalysisTests.cpp
        Source/engine/AnalysisEngine.cpp Source/core/PlaybackRouter.cpp Source/api/AnalyzerApi.cpp)
    target_include_directories(PluginAnalyzerTests PRIVATE Source)
    target_compile_definitions(PluginAnalyzerTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_AU=0
        PLUGIN_ANALYZER_VERSION_STRING="${PROJECT_VERSION}")
    target_link_libraries(PluginAnalyzerTests
        PRIVATE
            juce::juce_audio_processors
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # Loads the built VST3 like a DAW and checks the playback path end to end.
    juce_add_console_app(PluginSmokeTest
        PRODUCT_NAME "PluginSmokeTest"
        COMPANY_NAME "${COMPANY}"
        BUNDLE_ID "com.hmusic.pluginanalyzer.smoketest")
    target_sources(PluginSmokeTest PRIVATE Tests/PluginSmokeTest.cpp Source/host/BuiltinProcessors.cpp)
    target_include_directories(PluginSmokeTest PRIVATE Source)
    target_compile_definitions(PluginSmokeTest PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0)
    target_link_libraries(PluginSmokeTest
        PRIVATE
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
