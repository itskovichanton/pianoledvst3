cmake_minimum_required(VERSION 3.24)

project(PianoLED
    VERSION 0.1.0
    DESCRIPTION "MIDI-driven piano LED visualizer (AU + VST3)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS deployment target")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS architectures")

include(FetchContent)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(JUCE)

juce_add_plugin(PianoLED
    COMPANY_NAME "PianoLED"
    BUNDLE_ID com.pianoled.plugin
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Pled
    PLUGIN_CODE Pnld
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    AU_SANDBOX_SAFE TRUE
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "PianoLED"
)

target_sources(PianoLED
    PRIVATE
        src/PluginProcessor.cpp
        src/PluginProcessor.h
        src/PluginEditor.cpp
        src/PluginEditor.h
)

target_include_directories(PianoLED
    PRIVATE
        src
)

target_compile_definitions(PianoLED
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(PianoLED
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
