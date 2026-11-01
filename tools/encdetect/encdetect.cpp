// encdetect.cpp - 判码链的诊断入口。
//
// 直接调用 DLL 里的 DetectEncodingEx，避免工具与生产代码各写一套判定。
//
// 用法：
//   encdetect <file> [file...]
//
// 输出每文件一行：最终编码 / 代码页 / 判定出处 / CED 原始结果 / 样本位置。

#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "encoding_detect.h"

namespace {

constexpr size_t kProbe = 8u << 20;  // 与 ScanFile 的判码窗口一致

bool ReadHead(const wchar_t* path, std::vector<unsigned char>* buf, uint64_t* size) {
  HANDLE h = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER li;
  if (!::GetFileSizeEx(h, &li)) {
    ::CloseHandle(h);
    return false;
  }
  *size = static_cast<uint64_t>(li.QuadPart);
  buf->resize(kProbe);
  DWORD got = 0;
  const DWORD want = static_cast<DWORD>(*size < kProbe ? *size : kProbe);
  const BOOL ok = want == 0 ? FALSE : ::ReadFile(h, buf->data(), want, &got, nullptr);
  ::CloseHandle(h);
  if (!ok) return false;
  buf->resize(got);
  return true;
}

const char* PickName(doc::DetectPick p) {
  switch (p) {
    case doc::DetectPick::Bom: return "bom";
    case doc::DetectPick::Utf16NoBom: return "utf16-nobom";
    case doc::DetectPick::Ascii: return "ascii";
    case doc::DetectPick::Utf8Valid: return "utf8-valid";
    case doc::DetectPick::Ced: return "ced";
    case doc::DetectPick::CedFallback: return "ced-fallback";
  }
  return "?";
}

const char* NameOf(const doc::EncodingInfo& info, const doc::DetectDebug& d) {
  switch (info.encoding) {
    case doc::Encoding::Utf8: return "UTF-8";
    case doc::Encoding::Utf16LE: return "UTF-16LE";
    case doc::Encoding::Utf16BE: return "UTF-16BE";
    case doc::Encoding::Legacy:
      if (d.pick == doc::DetectPick::Ced) return d.cedMime;
      if (info.codepage == 12000) return "UTF-32LE";
      if (info.codepage == 12001) return "UTF-32BE";
      return "GB18030";
  }
  return "?";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::fwprintf(stderr, L"usage: encdetect <file> [file...]\n");
    return 2;
  }
  int bad = 0;
  for (int i = 1; i < argc; ++i) {
    std::vector<unsigned char> buf;
    uint64_t size = 0;
    if (!ReadHead(argv[i], &buf, &size)) {
      std::wprintf(L"ERR  %ls  (读不了)\n", argv[i]);
      ++bad;
      continue;
    }
    doc::DetectDebug d;
    const doc::EncodingInfo info = doc::DetectEncodingEx(buf.data(), buf.size(), &d);
    const wchar_t* slash = wcsrchr(argv[i], L'\\');
    std::wprintf(
        L"%-28ls %-13hs cp=%-5u %-13hs size=%llu high=%llu sample=%llu+%llu esc=%d  "
        L"ced=[%hs id=%d cp=%u rel=%d used=%d] enc=%d\n",
        slash ? slash + 1 : argv[i], NameOf(info, d), info.CodePage(), PickName(d.pick),
        static_cast<unsigned long long>(size),
        static_cast<unsigned long long>(d.firstHigh),
        static_cast<unsigned long long>(d.sampleOff),
        static_cast<unsigned long long>(d.sampleLen), d.hasEsc ? 1 : 0, d.cedMime, d.cedId,
        d.codepage, d.cedReliable ? 1 : 0, d.cedConsumed,
        static_cast<int>(info.encoding));
  }
  return bad == 0 ? 0 : 1;
}
