#pragma once

/**
 * Микро-фреймворк для тестов. Без gtest: собирается одной командой g++
 * и запускается где угодно.
 */

#include <iostream>
#include <sstream>
#include <string>

namespace test_framework {

inline int g_checks_run = 0;
inline int g_checks_failed = 0;

inline void begin_test(const std::string& name) {
    std::cout << "\n── " << name << "\n";
}

inline void report(bool passed, const std::string& what, const std::string& detail) {
    ++g_checks_run;
    if (passed) {
        std::cout << "   ok   " << what << "\n";
    } else {
        ++g_checks_failed;
        std::cout << "   FAIL " << what << "\n";
        if (!detail.empty()) std::cout << "        " << detail << "\n";
    }
}

inline void check(bool condition, const std::string& what) {
    report(condition, what, "");
}

template <typename A, typename B>
void check_eq(const A& actual, const B& expected, const std::string& what) {
    std::ostringstream detail;
    detail << "получено: [" << actual << "]  ожидалось: [" << expected << "]";
    report(actual == expected, what, detail.str());
}

inline int finish() {
    std::cout << "\n════════════════════════════════════════════════════\n";
    if (g_checks_failed == 0) {
        std::cout << "  ВСЕ ТЕСТЫ ПРОЙДЕНЫ: " << g_checks_run << " проверок\n";
    } else {
        std::cout << "  ПРОВАЛЕНО " << g_checks_failed << " из " << g_checks_run << " проверок\n";
    }
    std::cout << "════════════════════════════════════════════════════\n";
    return g_checks_failed == 0 ? 0 : 1;
}

}  // namespace test_framework
