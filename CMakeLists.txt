cmake_minimum_required(VERSION 3.22)

project(TrueAir VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows: link the C++ runtime statically (/MT, /MTd) so the plugin loads without
# the Visual C++ Redistributable installed. Must be set before any target exists.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Speed up rebuilds when ccache is available
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# ---- JUCE (pinned) -----------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.10
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ---- Plugin ------------------------------------------------------------------
set(TRUEAIR_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND TRUEAIR_FORMATS AU)
endif()

# NOTE: COMPANY_NAME, BUNDLE_ID and the two 4-char codes are placeholders.
# Pick final values before the first public build: changing PLUGIN_CODE or
# PLUGIN_MANUFACTURER_CODE later makes DAWs treat it as a different plugin.
juce_add_plugin(TrueAir
    COMPANY_NAME             "All-Meyn"
    BUNDLE_ID                "com.allmeyn.trueair"
    PLUGIN_MANUFACTURER_CODE Almy
    PLUGIN_CODE              Tair
    PRODUCT_NAME             "TrueAir"
    FORMATS                  ${TRUEAIR_FORMATS}
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    COPY_PLUGIN_AFTER_BUILD  FALSE)

target_sources(TrueAir PRIVATE
    Source/AirRestorer.cpp
    Source/CutoffDetector.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_include_directories(TrueAir PUBLIC Source)

target_compile_definitions(TrueAir PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(TrueAir
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Tests -------------------------------------------------------------------
option(TRUEAIR_BUILD_TESTS "Build TrueAir unit tests" ON)
if(TRUEAIR_BUILD_TESTS)
    enable_testing()
    # Plain executable (not juce_add_console_app) so it compiles with exactly the
    # same JUCE/JucePlugin_* definitions that TrueAir's shared code exports.
    add_executable(TrueAirTests Tests/TestMain.cpp)
    target_link_libraries(TrueAirTests PRIVATE
        TrueAir
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    add_test(NAME TrueAirTests COMMAND TrueAirTests)
endif()
