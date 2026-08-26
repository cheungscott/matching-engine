// tests/check.hpp — a ~50-line test harness.
//
// Why not Catch2: Catch2 arrives via CMake FetchContent, and CMake is not
// installed on the Windows side yet. This builds with g++ alone, no network.
// Retire it the day `cmake` works — the test BODIES port over unchanged.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace check {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

inline const char*& current() {
    static const char* c = "";
    return c;
}

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* expr, const char* file, int line, const std::string& extra) {
    ++failures();
    std::printf("  FAIL  %s\n        %s:%d\n        %s\n", current(), file, line, expr);
    if (!extra.empty()) std::printf("        %s\n", extra.c_str());
}

} // namespace check

// TEST(name) { ... }  — registers a case and defines its body.
#define TEST(name)                                                        \
    static void name();                                                   \
    static ::check::Register reg_##name(#name, &name);                    \
    static void name()

#define CHECK(expr)                                                       \
    do {                                                                  \
        if (!(expr)) ::check::fail(#expr, __FILE__, __LINE__, {});        \
    } while (0)

// CHECK_EQ prints both sides, which is most of the value over a bare CHECK.
#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        auto va_ = (a);                                                   \
        auto vb_ = (b);                                                   \
        if (!(va_ == vb_))                                                \
            ::check::fail(#a " == " #b, __FILE__, __LINE__,               \
                          "got " + std::to_string(va_) +                  \
                          ", expected " + std::to_string(vb_));           \
    } while (0)

inline int main_impl() {
    std::printf("running %zu cases\n\n", ::check::registry().size());
    for (const auto& c : ::check::registry()) {
        ::check::current() = c.name;
        c.fn();
    }
    const int f = ::check::failures();
    std::printf("\n%s  %d failure(s)\n", f == 0 ? "PASS" : "FAILED", f);
    return f == 0 ? 0 : 1;
}

int main() { return main_impl(); }
