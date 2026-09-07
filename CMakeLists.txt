cmake_minimum_required(VERSION 3.24)

project(PianoLED
    VERSION 0.1.0
    DESCRIPTION "MIDI-driven piano LED visualizer (AU + VST3)"
    LANGUAGES C CXX OBJCXX
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

add_subdirectory(bridge EXCLUDE_FROM_ALL)

add_executable(ledbridged
    bridge/mac/tools/ledbridged.cpp
    bridge/mac/src/helper_app.mm
)
target_link_libraries(ledbridged PRIVATE piano_led_bridge "-framework Cocoa")

set(PIANOLED_HELPER_APP "$ENV{HOME}/Library/Application Support/PianoLED/PianoLEDBridge.app")
add_custom_command(TARGET ledbridged POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${PIANOLED_HELPER_APP}/Contents/MacOS"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/bridge/mac/PianoLEDBridge.plist"
        "${PIANOLED_HELPER_APP}/Contents/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:ledbridged>
        "${PIANOLED_HELPER_APP}/Contents/MacOS/PianoLEDBridge"
)

juce_add_plugin(PianoLED
    COMPANY_NAME "PianoLED"
    BUNDLE_ID com.pianoled.plugin
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
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
        src/MidiDevicePlayer.cpp
        src/MidiDevicePlayer.h
)

target_include_directories(PianoLED
    PRIVATE
        src
        bridge/plugin
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
        juce::juce_audio_devices
        juce::juce_gui_extra
        piano_led_bridge
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

foreach (plugin_target IN ITEMS PianoLED_AU PianoLED_VST3 PianoLED_Standalone)
    if (TARGET ${plugin_target})
        add_dependencies(${plugin_target} ledbridged)
        add_custom_command(TARGET ${plugin_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:ledbridged>
                $<TARGET_FILE_DIR:${plugin_target}>/ledbridged
        )
    endif()
endforeach()
