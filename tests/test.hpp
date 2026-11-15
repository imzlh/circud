#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace circud::test {

using Test = std::pair<std::string, std::function<void()>>;
std::vector<Test>& tests();

struct Register {
    Register(std::string name, std::function<void()> function) {
        tests().emplace_back(std::move(name), std::move(function));
    }
};

inline void require(bool condition, std::string message = "requirement failed") {
    if (!condition) throw std::runtime_error(std::move(message));
}

} // namespace circud::test

#define CIRCUD_TEST(name)                                                   \
    static void name();                                                     \
    static ::circud::test::Register name##_register(#name, name);           \
    static void name()
