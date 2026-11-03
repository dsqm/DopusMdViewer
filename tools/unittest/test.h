// test.h - 极简单元测试框架（本插件专用，零依赖）。
//
// 与 webbuild/test/*.mjs 的 check() 风格一致：不引第三方框架，失败时打点继续，
// REQUIRE 类断言当场终止当前测试。输出只用 ASCII，避免控制台代码页问题。
//
// 用法：
//   TEST(suite, name) { CHECK(cond); CHECK_EQ_STR(got, L"want"); ... }
//   REQUIRE(cond) —— 失败时立即返回（搭建前置条件用）。
//
// 跑：tools/build_unittests.py 构建后直接运行 unittest.exe，退出码非 0 即有失败。

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace unittest {

struct TestCase {
  const char* suite;
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& Tests(void) {
  static std::vector<TestCase> v;
  return v;
}

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)()) {
    Tests().push_back({suite, name, fn});
  }
};

// ---- 断言记录 ----

int Failures(void);
void RecordFailure(const char* file, int line, const std::string& what);

void Check(bool cond, const char* file, int line, const std::string& what);

#define CHECK(cond) unittest::Check((cond), __FILE__, __LINE__, #cond)

// 带说明文字的版本：what 是一段可读描述（ASCII）。
#define CHECK_MSG(cond, what) unittest::Check((cond), __FILE__, __LINE__, (what))

// 前置条件：失败则当场结束当前测试（只能用在 TEST 函数体内）。
#define REQUIRE(cond)                          \
  do {                                         \
    if (!(cond)) {                             \
      unittest::Check(false, __FILE__, __LINE__, #cond); \
      return;                                  \
    }                                          \
  } while (0)

// ---- 值比较（失败时把两个值都打出来）----

void CheckEqInt(long long got, long long want, const char* file, int line,
                const char* expr);
void CheckEqStr(const std::wstring& got, const wchar_t* want, const char* file,
                int line, const char* expr);
void CheckContains(const std::wstring& hay, const wchar_t* needle,
                   const char* file, int line, const char* expr);

#define CHECK_EQ_INT(got, want) unittest::CheckEqInt((got), (want), __FILE__, __LINE__, #got)
#define CHECK_EQ_STR(got, want) unittest::CheckEqStr((got), (want), __FILE__, __LINE__, #got)
#define CHECK_CONTAINS(hay, needle) \
  unittest::CheckContains((hay), (needle), __FILE__, __LINE__, #hay)

// ---- TEST 注册 ----

#define TEST(suite, name)                                    \
  static void unittest_##suite##_##name(void);               \
  static unittest::Registrar unittest_reg_##suite##_##name(  \
      #suite, #name, &unittest_##suite##_##name);            \
  static void unittest_##suite##_##name(void)

// ---- 运行 ----

// 跑全部注册的测试；返回失败的测试数。失败总数（断言级）写在 *assertFailures。
int RunAll(int* assertFailures);

// ---- 测试用的文件与编码助手（实现在 main.cpp）----

// %TEMP%\mdv-unittest\，进程启动时已清空重建。
const std::wstring& TempRoot(void);

// TempRoot 下的一个新子目录（tag + 递增序号），已建好。
std::wstring MakeTempDir(const char* tag);

// 整目录写二进制文件（自动建父目录）。
bool WriteBytes(const std::wstring& path, const void* data, size_t bytes);
bool WriteBytes(const std::wstring& path, const std::string& bytes);

// 宽文本 → UTF-8 字节。
std::string Narrow(const wchar_t* text);  // WideCharToMultiByte CP_UTF8

// 宽文本按指定代码页编码成字节（GB18030 样本用）。
std::string EncodeWith(const std::wstring& text, UINT codepage);

}  // namespace unittest
