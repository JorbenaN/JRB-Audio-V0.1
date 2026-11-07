# Test VST3 plugins for host testing (spike S4 and later the compatibility suite).
# A separate CMake project on purpose: JUCE's plugin tooling changes global
# compiler flags, which must not leak into the main build.
#
#   cmake -S tools/test-plugin -B build/test-plugin -G Xcode && cmake --build build/test-plugin --config Release
cmake_minimum_required(VERSION 3.29)
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "")
project(JRBTestPlugins VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/JUCE ${CMAKE_BINARY_DIR}/JUCE)

function(jrb_test_plugin target name code synth source)
    juce_add_plugin(${target}
        COMPANY_NAME "JRB Audio Test"
        PLUGIN_MANUFACTURER_CODE Jrbt
        PLUGIN_CODE ${code}
        FORMATS VST3
        PRODUCT_NAME "${name}"
        IS_SYNTH ${synth}
        NEEDS_MIDI_INPUT ${synth}
        COPY_PLUGIN_AFTER_BUILD FALSE)
    target_sources(${target} PRIVATE ${source})
    target_compile_definitions(${target} PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(${target}
        PRIVATE juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endfunction()

jrb_test_plugin(JRBTestGain "JRB Test Gain" Jtg1 FALSE TestGain.cpp)
jrb_test_plugin(JRBTestSynth "JRB Test Synth" Jts1 TRUE TestSynth.cpp)
