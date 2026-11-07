add_executable(jrb-plugin-scanner main.cpp)
target_link_libraries(jrb-plugin-scanner PRIVATE jrb_juce_headless)
target_compile_options(jrb-plugin-scanner PRIVATE -Wall -Wextra -Werror)
