#pragma once

// Minimal test harness for the spike phase. To be replaced by a real
// framework (doctest or Catch2) once adding a dependency is approved.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace jrbtest {

struct Case {
    const char* name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

inline int runAll() {
    for (const auto& c : registry()) {
        const int before = failures();
        c.body();
        std::printf("%s %s\n", failures() == before ? "PASS" : "FAIL", c.name);
    }
    std::printf("%zu tests, %d failed checks\n", registry().size(), failures());
    return failures() == 0 ? 0 : 1;
}

} // namespace jrbtest

#define JRB_CAT2(a, b) a##b
#define JRB_CAT(a, b) JRB_CAT2(a, b)
#define TEST(name)                                                                      \
    static void JRB_CAT(test_, __LINE__)();                                             \
    static jrbtest::Register JRB_CAT(reg_, __LINE__)(name, &JRB_CAT(test_, __LINE__)); \
    static void JRB_CAT(test_, __LINE__)()

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ++jrbtest::failures();                                                 \
            std::printf("  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)
