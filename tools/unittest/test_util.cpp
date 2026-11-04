// test_util.cpp - util.h 纯逻辑与文件助手的单元测试。
//
// UtilDataDir / UtilModuleDir 依赖进程环境（宿主模块、别名解析），不在这里测；
// 其余字符串与路径函数按 util.h 注释里的契约逐条断言。

#include "util.h"  // 构建脚本把 src 放进 INCLUDE

#include "test.h"

using namespace unittest;

namespace {

std::wstring W(const wchar_t* s) { return std::wstring(s ? s : L""); }

}  // namespace

TEST(util, trim) {
  CHECK_EQ_STR(UtilTrim(L"  a b  "), L"a b");
  CHECK_EQ_STR(UtilTrim(L"\t\r\nx\r\n"), L"x");
  CHECK_EQ_STR(UtilTrim(L"no-trim"), L"no-trim");
  CHECK_EQ_STR(UtilTrim(L"   "), L"");
  CHECK_EQ_STR(UtilTrim(L""), L"");
  // 内部空白保留；全角空格不是空白（契约里只有空格/Tab/CR/LF）
  CHECK_EQ_STR(UtilTrim(L" a  b "), L"a  b");
  CHECK_EQ_STR(UtilTrim(L"\u3000x"), L"\u3000x");
}

TEST(util, join) {
  CHECK_EQ_STR(UtilJoin(L"C:\\a", L"b"), L"C:\\a\\b");
  CHECK_EQ_STR(UtilJoin(L"C:\\a\\", L"b"), L"C:\\a\\b");
  CHECK_EQ_STR(UtilJoin(L"C:\\a/", L"b"), L"C:\\a/b");  // 有尾分隔符就原样接
  CHECK_EQ_STR(UtilJoin(L"", L"b"), L"b");
  CHECK_EQ_STR(UtilJoin(L"C:\\a", L""), L"C:\\a\\");
  CHECK_EQ_STR(UtilJoin(L"C:\\a", nullptr), L"C:\\a\\");
}

TEST(util, is_local_absolute_path) {
  CHECK(UtilIsLocalAbsolutePath(L"C:\\a\\b"));
  CHECK(UtilIsLocalAbsolutePath(L"c:/a/b"));
  CHECK(UtilIsLocalAbsolutePath(L"Z:\\"));
  CHECK(!UtilIsLocalAbsolutePath(L"C:"));      // 少了第三字符
  CHECK(!UtilIsLocalAbsolutePath(L"C:a"));     // 冒号后不是分隔符
  CHECK(!UtilIsLocalAbsolutePath(L"\\\\srv\\share"));  // UNC 拒绝
  CHECK(!UtilIsLocalAbsolutePath(L"relative\\path"));
  CHECK(!UtilIsLocalAbsolutePath(L""));
}

TEST(util, parent_dir) {
  CHECK_EQ_STR(UtilParentDir(L"C:\\a\\b"), L"C:\\a");
  CHECK_EQ_STR(UtilParentDir(L"C:\\a\\b\\"), L"C:\\a\\b");   // 尾分隔符是最后一段
  CHECK_EQ_STR(UtilParentDir(L"C:\\"), L"C:");               // 根的父目录是盘符
  CHECK_EQ_STR(UtilParentDir(L"plain"), L"");                // 无分隔符
  CHECK_EQ_STR(UtilParentDir(L"/x"), L"/");                  // pos==0 只留一个字符
}

TEST(util, leaf_name) {
  CHECK(UtilLeafName(L"C:\\a\\b.txt") == std::wstring(L"b.txt"));
  CHECK(UtilLeafName(L"C:/a/b.txt") == std::wstring(L"b.txt"));   // 正斜杠也算
  CHECK(UtilLeafName(L"C:\\a\\dir\\") == std::wstring(L""));      // 尾分隔符 → 空
  CHECK(UtilLeafName(L"file.md") == std::wstring(L"file.md"));
  CHECK(UtilLeafName(nullptr) == std::wstring(L""));
}

TEST(util, format_size) {
  CHECK_EQ_STR(UtilFormatSize(0), L"0 B");
  CHECK_EQ_STR(UtilFormatSize(512), L"512 B");
  CHECK_EQ_STR(UtilFormatSize(1024), L"1.0 KB");
  CHECK_EQ_STR(UtilFormatSize(1536), L"1.5 KB");
  CHECK_EQ_STR(UtilFormatSize(1048576), L"1.0 MB");
  CHECK_EQ_STR(UtilFormatSize(1073741824ull), L"1.0 GB");
  // 边界落在下一档：1073741823 还是 MB（< 1<<30）
  CHECK_EQ_STR(UtilFormatSize(1073741823ull), L"1024.0 MB");
}

TEST(util, decode_text) {
  std::wstring out;
  // 纯 UTF-8（含中文）
  REQUIRE(UtilDecodeText(std::string("a\xe4\xb8\xadz"), &out));
  CHECK_EQ_STR(out, L"a\u4e2dz");
  // UTF-8 BOM 要剥掉
  REQUIRE(UtilDecodeText(std::string("\xef\xbb\xbfhi"), &out));
  CHECK_EQ_STR(out, L"hi");
  // 空串
  CHECK(UtilDecodeText(std::string(), &out) && out.empty());
  // 仅 BOM
  CHECK(UtilDecodeText(std::string("\xef\xbb\xbf"), &out) && out.empty());
  // 非法 UTF-8 的 GB18030 字节（"中文" 的 GB18030 = D6 D0 CE C4）
  REQUIRE(UtilDecodeText(std::string("\xd6\xd0\xce\xc4"), &out));
  CHECK_EQ_STR(out, L"\u4e2d\u6587");
  // 两种编码都判不了 → GB18030 兜底也判不了才返回 false。实际 GB18030 是按
  // 默认字符 '?' 替换坏字节的（flags=0），几乎不失败；这里锁的就是这个行为。
  out = L"untouched";
  REQUIRE(UtilDecodeText(std::string("\xff\xff\xff"), &out));
  CHECK_EQ_STR(out, L"???");
  // 合法 ASCII 永远走 UTF-8
  REQUIRE(UtilDecodeText(std::string("plain ascii"), &out));
  CHECK_EQ_STR(out, L"plain ascii");
}

TEST(util, expand_env) {
  ::SetEnvironmentVariableW(L"MDVUT_VAR_XYZ", L"hello");
  CHECK_EQ_STR(UtilExpandEnv(L"%MDVUT_VAR_XYZ%\\x"), L"hello\\x");
  // 未知变量保持原样
  CHECK_EQ_STR(UtilExpandEnv(L"%MDVUT_VAR_MISSING_9%"), L"%MDVUT_VAR_MISSING_9%");
  CHECK_EQ_STR(UtilExpandEnv(L""), L"");
  CHECK_EQ_STR(UtilExpandEnv(L"no vars"), L"no vars");
  ::SetEnvironmentVariableW(L"MDVUT_VAR_XYZ", nullptr);
}

TEST(util, normalize_local_dir) {
  CHECK_EQ_STR(UtilNormalizeLocalDir(L"C:\\a\\b\\"), L"C:\\a\\b");   // 去尾分隔符
  CHECK_EQ_STR(UtilNormalizeLocalDir(L"C:/a/b"), L"C:\\a\\b");       // 转反斜杠
  CHECK_EQ_STR(UtilNormalizeLocalDir(L"  C:\\a\\b  "), L"C:\\a\\b"); // 掐空白
  CHECK_EQ_STR(UtilNormalizeLocalDir(L"C:\\a\\.\\b"), L"C:\\a\\b");  // . 段
  CHECK_EQ_STR(UtilNormalizeLocalDir(L"C:\\a\\..\\b"), L"C:\\b");    // .. 段
  // 非法输入返回空串
  CHECK_EQ_STR(UtilNormalizeLocalDir(L"relative\\path"), L"");
  CHECK_EQ_STR(UtilNormalizeLocalDir(L""), L"");
  CHECK_EQ_STR(UtilNormalizeLocalDir(L"\\\\srv\\share"), L"");
  // 环境变量展开后是合法路径时照常归一
  ::SetEnvironmentVariableW(L"MDVUT_DIR_XYZ", L"C:\\mdv-ut\\dir");
  CHECK_EQ_STR(UtilNormalizeLocalDir(L"%MDVUT_DIR_XYZ%"), L"C:\\mdv-ut\\dir");
  ::SetEnvironmentVariableW(L"MDVUT_DIR_XYZ", nullptr);
}

TEST(util, webview2_dir_names) {
  // 叶子目录名是 WebView2（大小写不敏感、尾分隔符可带），且必须是本地绝对路径
  CHECK(UtilIsWebView2UserDataDir(L"C:\\a\\WebView2"));
  CHECK(UtilIsWebView2UserDataDir(L"C:\\a\\webview2\\"));
  CHECK(UtilIsWebView2UserDataDir(L"C:\\WebView2"));
  CHECK(!UtilIsWebView2UserDataDir(L"C:\\a\\WebView"));       // 只差一段
  CHECK(!UtilIsWebView2UserDataDir(L"C:\\a\\WebView2x"));
  CHECK(!UtilIsWebView2UserDataDir(L"rel\\WebView2"));        // 非绝对路径
  CHECK(!UtilIsWebView2UserDataDir(L""));

  // 配置目录合法时直接接 WebView2；含正斜杠也归一
  CHECK_EQ_STR(UtilWebView2UserDataDir(L"C:\\cache"), L"C:\\cache\\WebView2");
  CHECK_EQ_STR(UtilWebView2UserDataDir(L"C:/cache/x"), L"C:\\cache\\x\\WebView2");
  // 配置为空/非法 → 回到 LOCALAPPDATA 默认（main 已把 LOCALAPPDATA 指进隔离区），
  // 落点是 <默认基目录>\WebView2
  const std::wstring fallback = UtilWebView2UserDataDir(L"");
  CHECK(fallback == UtilWebView2UserDataDir(L"relative only"));
  CHECK(fallback.size() > 9 &&
        fallback.compare(fallback.size() - 9, 9, L"\\WebView2") == 0);
}

TEST(util, dir_file_roundtrip) {
  const std::wstring dir = MakeTempDir("util-fs");
  const std::wstring sub = UtilJoin(dir, L"a\\b");
  CHECK(UtilEnsureDir(sub));
  CHECK(UtilDirExists(sub));
  CHECK(UtilDirExists(dir));
  CHECK(!UtilDirExists(UtilJoin(dir, L"nope")));

  const std::wstring file = UtilJoin(sub, L"f.txt");
  CHECK(WriteBytes(file, std::string("hello bytes")));
  CHECK(UtilFileExists(file));
  CHECK(!UtilFileExists(dir));  // 目录不算文件

  std::string bytes;
  CHECK(UtilReadFileBytes(file, 1024, &bytes));
  CHECK(bytes == "hello bytes");
  // 超过 maxBytes 直接失败
  CHECK(!UtilReadFileBytes(file, 5, &bytes));
  CHECK(!UtilReadFileBytes(UtilJoin(dir, L"missing"), 1024, &bytes));

  // 目录占用字节数
  CHECK_EQ_INT(static_cast<long long>(UtilDirSize(dir)), 11);
  // stopAt 提前收手，但同一条目录项内部不截断（单文件 11 字节照样返回 11）
  CHECK(static_cast<long long>(UtilDirSize(dir, 5)) >= 5);

  // 递归删除：连根拔掉
  CHECK(UtilRemoveTree(dir));
  CHECK(!UtilDirExists(dir));
  CHECK(UtilRemoveTree(dir));  // 再删一次也算成功
}

TEST(util, win32_error_text) {
  // 0 没有消息；取得到的错误码去掉尾随换行
  CHECK(UtilWin32ErrorText(0).empty());
  const std::wstring text = UtilWin32ErrorText(ERROR_FILE_NOT_FOUND);
  CHECK(!text.empty());
  CHECK(text.back() != L'\n' && text.back() != L'\r');
}

TEST(util, opus_viewer_ext) {
  // 带点、大小写不敏感；名单覆盖图片、文本/代码与 PDF
  CHECK(UtilOpusViewerHandlesExt(L".png"));
  CHECK(UtilOpusViewerHandlesExt(L".PNG"));
  CHECK(UtilOpusViewerHandlesExt(L".md"));
  CHECK(UtilOpusViewerHandlesExt(L".Markdown"));
  CHECK(UtilOpusViewerHandlesExt(L".pdf"));
  // 名单外与非法输入：交回系统默认程序
  CHECK(!UtilOpusViewerHandlesExt(L".exe"));
  CHECK(!UtilOpusViewerHandlesExt(L".7z"));
  CHECK(!UtilOpusViewerHandlesExt(L"png"));  // 没点
  CHECK(!UtilOpusViewerHandlesExt(L""));
  CHECK(!UtilOpusViewerHandlesExt(nullptr));
}

TEST(util, opus_viewer_show_missing_file) {
  // 不存在的文件不进 CreateProcess：直接 false，调用方回退系统默认程序
  CHECK(!UtilOpusViewerShow(L""));
  CHECK(!UtilOpusViewerShow(L"Z://definitely-not-here//x.png"));
}
