# Spike S2 soak test: real Core Audio output through the engine (not part of CTest:
# it needs an audio device and runs for a while).
add_executable(jrb_s2_soak main.cpp)
target_link_libraries(jrb_s2_soak PRIVATE jrb_engine jrb_juce)
target_compile_options(jrb_s2_soak PRIVATE -Wall -Wextra -Werror)
