cmake_minimum_required(VERSION 3.29)

# Must be set before project() to take effect.
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version (ADR-016)")

project(JRBAudio VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Swift targets need the Xcode or Ninja generator. With the default Makefile
# generator, configure with -DJRB_BUILD_SWIFT=OFF to build and test the C++ core only.
option(JRB_BUILD_SWIFT "Build Swift targets" ON)
if(JRB_BUILD_SWIFT)
    enable_language(Swift)
    if(CMAKE_GENERATOR STREQUAL "Xcode")
        set(CMAKE_XCODE_ATTRIBUTE_SWIFT_VERSION 6)   # Swift 6 language mode
    else()
        set(CMAKE_Swift_LANGUAGE_VERSION 6)
    endif()
endif()

enable_testing()

# JUCE 9.0.5 (third_party/JUCE/VENDORED.md): modules only, used inside the core.
set(JUCE_MODULES_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/JUCE EXCLUDE_FROM_ALL)

add_subdirectory(core)
add_subdirectory(tools/s2-soak)
add_subdirectory(tools/plugin-scanner)
add_subdirectory(tools/s4-host)

if(JRB_BUILD_SWIFT)
    add_subdirectory(spikes/s1-interop)
    add_subdirectory(apps/s3-bench)
endif()
