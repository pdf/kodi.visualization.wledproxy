#pragma once

// Minimal test helpers: each CHECK prints the failure and counts it; main()
// returns the count.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test
{

inline int& Failures()
{
  static int n = 0;
  return n;
}

struct Case
{
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& Cases()
{
  static std::vector<Case> c;
  return c;
}

struct Register
{
  Register(const char* name, std::function<void()> fn) { Cases().push_back({name, fn}); }
};

inline int RunAll()
{
  for (const Case& c : Cases())
  {
    const int before = Failures();
    c.fn();
    std::printf("%s %s\n", Failures() == before ? "ok  " : "FAIL", c.name);
  }
  std::printf("%d failure(s)\n", Failures());
  return Failures() == 0 ? 0 : 1;
}

} // namespace test

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name)                                                                \
  static void name();                                                             \
  static test::Register TEST_CAT(reg_, name)(#name, name);                        \
  static void name()

#define CHECK(cond)                                                               \
  do                                                                              \
  {                                                                               \
    if (!(cond))                                                                  \
    {                                                                             \
      std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);      \
      ++test::Failures();                                                         \
    }                                                                             \
  } while (0)

#define CHECK_MSG(cond, ...)                                                      \
  do                                                                              \
  {                                                                               \
    if (!(cond))                                                                  \
    {                                                                             \
      std::printf("  %s:%d: CHECK(%s) failed: ", __FILE__, __LINE__, #cond);      \
      std::printf(__VA_ARGS__);                                                   \
      std::printf("\n");                                                          \
      ++test::Failures();                                                         \
    }                                                                             \
  } while (0)

#define TEST_MAIN()                                                               \
  int main() { return test::RunAll(); }
