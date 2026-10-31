// encdetect.cpp - 票 19 的判码链原型：只验证判定顺序与准确率，不接进 DLL。
//
// 用法：
//   encdetect <file> [file...]
//
// 输出每文件一行：最终编码 / 代码页 / 判定出处 / CED 原始结果 / 样本位置。
// 判定链与票 19「判定顺序」一致：BOM → 无 BOM UTF-16 → 取样本 → 纯 ASCII → 合法 UTF-8 → CED。

#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ced_wrap.h"

namespace {

constexpr size_t kProbe = 8u << 20;     // 探测窗口，与同步前缀扫描的 8 MB 一致
constexpr size_t kSample = 64u << 10;   // 喂 CED 的样本上限
constexpr size_t kBack = 1u << 10;      // 第一个非 ASCII 之前回退的字节数
constexpr size_t kZeroScan = 4096;      // 无 BOM UTF-16 的零字节采样范围

enum class Pick {
  Bom,          // BOM 直接定
  Utf16NoBom,   // 零字节启发式
  Ascii,        // 整块纯 ASCII 且无 ESC
  Utf8Valid,    // 样本是合法 UTF-8
  Ced,          // CED 采信
  CedFallback,  // CED 不可靠或解不了，回退 GB18030
};

struct Detection {
  Pick pick = Pick::Ascii;
  unsigned codepage = 65001;
  const char* name = "UTF-8";
  size_t bom = 0;
  size_t sampleOff = 0;
  size_t sampleLen = 0;
  bool hasEsc = false;
  size_t firstHigh = 0;
  CedResult ced;
};

const char* PickName(Pick p) {
  switch (p) {
    case Pick::Bom: return "bom";
    case Pick::Utf16NoBom: return "utf16-nobom";
    case Pick::Ascii: return "ascii";
    case Pick::Utf8Valid: return "utf8-valid";
    case Pick::Ced: return "ced";
    case Pick::CedFallback: return "ced-fallback";
  }
  return "?";
}

// 与 doc_index.cpp 的同名判定一致：整段是否是合法 UTF-8（尾部被切断按合法算）。
bool LooksLikeUtf8(const unsigned char* p, size_t n) {
  size_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    uint32_t need = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
      need = 1;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      need = 2;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      need = 3;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (i + need >= n) return true;
    for (uint32_t k = 1; k <= need; ++k) {
      const unsigned char cc = p[i + k];
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (need == 1 && cp < 0x80) return false;
    if (need == 2 && cp < 0x800) return false;
    if (need == 3 && cp < 0x10000) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    i += need + 1;
  }
  return true;
}

// 无 BOM UTF-16：ASCII 为主的文本在固定奇偶位置有大量 0x00。
// 纯 CJK 的 UTF-16 一个 0x00 都没有，这个启发式判不出来（票 19 的已知限制）。
bool LooksLikeUtf16(const unsigned char* p, size_t n, bool* bigEndian) {
  if (n < 64) return false;
  const size_t lim = n < kZeroScan ? n : kZeroScan;
  size_t nulEven = 0;
  size_t nulOdd = 0;
  size_t even = 0;
  size_t odd = 0;
  for (size_t i = 0; i < lim; ++i) {
    if ((i & 1) != 0) {
      ++odd;
      if (p[i] == 0) ++nulOdd;
    } else {
      ++even;
      if (p[i] == 0) ++nulEven;
    }
  }
  // 一侧稀疏（<5%）、另一侧密集（>40%）才算；普通 8 位文件两侧都不会有 0x00。
  const bool le = nulOdd * 100 > odd * 40 && nulEven * 100 < even * 5;
  const bool be = nulEven * 100 > even * 40 && nulOdd * 100 < odd * 5;
  if (le == be) return false;
  *bigEndian = be;
  return true;
}

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

Detection Detect(const std::vector<unsigned char>& buf) {
  Detection d;
  const unsigned char* p = buf.data();
  const size_t n = buf.size();
  if (n == 0) return d;

  // ① BOM。4 字节先查：UTF-32LE 的 FF FE 00 00 会被 UTF-16 判定吞掉。
  if (n >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0x00 && p[3] == 0x00) {
    d.pick = Pick::Bom;
    d.name = "UTF-32LE";
    d.codepage = 12000;
    d.bom = 4;
    return d;
  }
  if (n >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFE && p[3] == 0xFF) {
    d.pick = Pick::Bom;
    d.name = "UTF-32BE";
    d.codepage = 12001;
    d.bom = 4;
    return d;
  }
  if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
    d.pick = Pick::Bom;
    d.name = "UTF-8";
    d.codepage = 65001;
    d.bom = 3;
    return d;
  }
  if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
    d.pick = Pick::Bom;
    d.name = "UTF-16LE";
    d.codepage = 1200;
    d.bom = 2;
    return d;
  }
  if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
    d.pick = Pick::Bom;
    d.name = "UTF-16BE";
    d.codepage = 1201;
    d.bom = 2;
    return d;
  }

  // ② 无 BOM UTF-16
  bool be = false;
  if (LooksLikeUtf16(p, n, &be)) {
    d.pick = Pick::Utf16NoBom;
    d.name = be ? "UTF-16BE" : "UTF-16LE";
    d.codepage = be ? 1201 : 1200;
    return d;
  }

  // ③ 取样本：跳过前导 ASCII，落在真有非 ASCII 的区段里
  size_t firstHigh = n;
  bool hasEsc = false;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] >= 0x80 && firstHigh == n) firstHigh = i;
    if (p[i] == 0x1B) hasEsc = true;
  }
  d.firstHigh = firstHigh;
  d.hasEsc = hasEsc;

  if (firstHigh == n && !hasEsc) {
    d.pick = Pick::Ascii;
    d.name = "UTF-8";
    d.codepage = 65001;
    return d;
  }

  d.sampleOff = firstHigh == n ? 0 : (firstHigh > kBack ? firstHigh - kBack : 0);
  d.sampleLen = n - d.sampleOff < kSample ? n - d.sampleOff : kSample;
  const unsigned char* s = p + d.sampleOff;

  // ④ 全段合法 UTF-8 且没有 ESC。含 ESC 不能短路：ISO-2022-JP 是纯 7 位字节流。
  if (!hasEsc && LooksLikeUtf8(s, d.sampleLen)) {
    d.pick = Pick::Utf8Valid;
    d.name = "UTF-8";
    d.codepage = 65001;
    return d;
  }

  // ⑤ CED。
  // 不拿 is_reliable 当门禁：它的门槛是「首位比第二位高 2^10 倍」，短文本几乎恒为 false，
  // 实测 windows-1251 / EUC-JP / KOI8-R / ISO-2022-JP 都判对却报 unreliable。
  // 只在「没判出来」或「说有非 ASCII 字节的文件是纯 ASCII」时回退。
  d.ced = CedDetect(reinterpret_cast<const char*>(s), static_cast<int>(d.sampleLen), false);
  const bool bogus = d.ced.unknown || (d.ced.ascii && firstHigh != n);
  if (!bogus && d.ced.codepage != 0) {
    d.pick = Pick::Ced;
    d.name = d.ced.mime;
    d.codepage = d.ced.codepage;
    return d;
  }
  d.pick = Pick::CedFallback;
  d.name = "GB18030";
  d.codepage = 54936;
  return d;
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
    const Detection d = Detect(buf);
    const wchar_t* slash = wcsrchr(argv[i], L'\\');
    std::wprintf(L"%-28ls %-13hs cp=%-5u %-13hs size=%llu high=%llu sample=%llu+%llu esc=%d  ced=[%hs id=%d cp=%u rel=%d used=%d]\n",
                 slash ? slash + 1 : argv[i],
                 d.name, d.codepage, PickName(d.pick),
                 static_cast<unsigned long long>(size),
                 static_cast<unsigned long long>(d.firstHigh),
                 static_cast<unsigned long long>(d.sampleOff),
                 static_cast<unsigned long long>(d.sampleLen),
                 d.hasEsc ? 1 : 0,
                 d.ced.mime, d.ced.id, d.ced.codepage, d.ced.reliable ? 1 : 0, d.ced.consumed);
  }
  return bad == 0 ? 0 : 1;
}
