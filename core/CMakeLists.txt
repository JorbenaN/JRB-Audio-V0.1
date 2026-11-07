add_library(jrb_core STATIC
    src/model/Project.cpp
    src/model/NoteSequence.cpp
    src/edit/Draft.cpp
    src/edit/Commands.cpp
    src/edit/Session.cpp
    src/api/CoreAPI.cpp
)
target_include_directories(jrb_core PUBLIC include)
target_compile_options(jrb_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror)

# Real-time code: every [[clang::nonblocking]] violation is a build error.
set(JRB_RT_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Werror -Wfunction-effects -Werror=function-effects)

add_library(jrb_engine STATIC
    src/platform/Realtime.cpp
    src/engine/WorkerPool.cpp
    src/engine/Engine.cpp
    src/engine/SpikeScene.cpp
)
target_include_directories(jrb_engine PUBLIC include)
target_compile_options(jrb_engine PRIVATE ${JRB_RT_WARNINGS})

# Plugin scanning client and wire format: plain C++, no JUCE.
add_library(jrb_plugins STATIC src/plugins/ScanClient.cpp)
target_include_directories(jrb_plugins PUBLIC include)
target_compile_options(jrb_plugins PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror)

# JUCE layers. JUCE module sources compile into exactly one of these per
# executable (linking both into one binary would duplicate JUCE symbols).
set(JRB_JUCE_DEFINITIONS
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1)
set(JRB_JUCE_FLAGS juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# For the app and tools: audio devices + plugin hosting with plugin GUIs.
add_library(jrb_juce STATIC src/device/JuceAudioDevice.cpp src/plugins/JucePluginHost.cpp)
target_include_directories(jrb_juce PUBLIC include)
target_link_libraries(jrb_juce
    PUBLIC jrb_plugins
    PRIVATE juce::juce_audio_devices juce::juce_audio_processors juce::juce_gui_extra ${JRB_JUCE_FLAGS}
    INTERFACE "-framework CoreAudioKit")   # AU generic editor (AUGenericView), needed by JUCE's AU hosting
target_compile_definitions(jrb_juce PRIVATE ${JRB_JUCE_DEFINITIONS} JRB_PLUGIN_EDITORS=1)

# For the out-of-process scanner: hosting without any GUI code.
add_library(jrb_juce_headless STATIC src/plugins/JucePluginHost.cpp)
target_include_directories(jrb_juce_headless PUBLIC include)
target_link_libraries(jrb_juce_headless
    PUBLIC jrb_plugins
    PRIVATE juce::juce_audio_processors_headless ${JRB_JUCE_FLAGS})
target_compile_definitions(jrb_juce_headless PRIVATE ${JRB_JUCE_DEFINITIONS} JRB_PLUGIN_EDITORS=0)

add_executable(jrb_core_tests tests/CoreTests.cpp tests/NoteSequenceTests.cpp tests/EngineTests.cpp tests/PluginTests.cpp)
target_link_libraries(jrb_core_tests PRIVATE jrb_core jrb_engine jrb_plugins)
target_compile_options(jrb_core_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME core_tests COMMAND jrb_core_tests)

add_executable(jrb_model_benchmark tests/ModelBenchmark.cpp)
target_link_libraries(jrb_model_benchmark PRIVATE jrb_core)

# A stand-in scanner that crashes, hangs or prints garbage on request, to test
# that the scan client survives misbehaving plugins.
add_executable(jrb_fake_scanner tests/FakeScanner.cpp)
target_link_libraries(jrb_fake_scanner PRIVATE jrb_plugins)
add_dependencies(jrb_core_tests jrb_fake_scanner)
target_compile_definitions(jrb_core_tests PRIVATE JRB_FAKE_SCANNER="$<TARGET_FILE:jrb_fake_scanner>")
