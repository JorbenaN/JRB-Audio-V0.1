# Spike S1: Swift reads snapshots and performs edits through CoreAPI using
# Swift's native C++ interoperability (no C wrapper, no Objective-C++).
add_executable(jrb_s1_interop main.swift)
target_link_libraries(jrb_s1_interop PRIVATE jrb_core)
if(CMAKE_GENERATOR STREQUAL "Xcode")
    # Use Xcode's native settings so the target looks like any Xcode Swift target.
    set_target_properties(jrb_s1_interop PROPERTIES
        XCODE_ATTRIBUTE_SWIFT_OBJC_INTEROP_MODE objcxx
        XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD c++20)
else()
    target_compile_options(jrb_s1_interop PRIVATE
        "$<$<COMPILE_LANGUAGE:Swift>:-cxx-interoperability-mode=default>"
        "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xcc -std=c++20>")
endif()
add_test(NAME s1_interop COMMAND jrb_s1_interop)
