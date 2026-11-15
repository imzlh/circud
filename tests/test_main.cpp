#include "test.hpp"

#include "tjs.h"

#include <iostream>

namespace circud::test {
std::vector<Test>& tests() {
    static std::vector<Test> value;
    return value;
}
} // namespace circud::test

int main(int argc, char** argv) {
    TJS_Initialize(argc, argv);
    unsigned failed = 0;
    for (const auto& [name, function] : circud::test::tests()) {
        try {
            function();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failed ? 1 : 0;
}
