# Spike S4 plugin hosting test (Objective-C++: it owns NSWindows for plugin GUIs).
add_executable(jrb_s4_host main.mm)
target_link_libraries(jrb_s4_host PRIVATE jrb_juce "-framework AppKit")
target_compile_options(jrb_s4_host PRIVATE -fobjc-arc -Wall -Wextra)
set_target_properties(jrb_s4_host PROPERTIES OBJCXX_STANDARD 20)
add_dependencies(jrb_s4_host jrb-plugin-scanner)
