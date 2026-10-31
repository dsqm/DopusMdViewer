// doc_regex_common.h - 两种宽度共用的分块、行对齐与边界工具。
#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace doc {
namespace regex_detail {

constexpr uint64_t kChunkBytes = 4ull << 20;
constexpr uint64_t kLineSlack = 1ull << 20;   // 行尾最多再往前找 1 MB
constexpr uint64_t kLookahead = 64ull << 10;  // 块后前瞻
constexpr uint64_t kWindowBytes = kChunkBytes + kLineSlack + kLookahead + 16;
constexpr size_t kMaxStored = 10000;

struct LineCut {
  uint64_t pos = 0;        // 相对窗口起点
  bool atLineStart = false;  // 切点紧跟在换行之后
};

inline bool OpenForScan(const std::wstring& path, HANDLE* h, uint64_t* size,
                        std::wstring* error) {
  *h = ::CreateFileW(path.c_str(), GENERIC_READ,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                     OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (*h == INVALID_HANDLE_VALUE) {
    if (error) *error = L"打开文件失败";
    return false;
  }
  LARGE_INTEGER s = {};
  if (!::GetFileSizeEx(*h, &s) || s.QuadPart <= 0) {
    ::CloseHandle(*h);
    *h = INVALID_HANDLE_VALUE;
    if (error) *error = L"读取文件大小失败";
    return false;
  }
  *size = static_cast<uint64_t>(s.QuadPart);
  return true;
}

inline bool ReadWindow(HANDLE h, uint64_t off, size_t want,
                       std::vector<unsigned char>* buf, size_t* got) {
  LARGE_INTEGER pos = {};
  pos.QuadPart = static_cast<LONGLONG>(off);
  if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
  buf->resize(want);
  DWORD n = 0;
  if (!::ReadFile(h, buf->data(), static_cast<DWORD>(want), &n, nullptr)) return false;
  *got = n;
  return n > 0;
}

inline bool IsUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

inline uint64_t Utf8CharLen(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

// 把切点夹到一个合法的 UTF-8 边界：是续字节就回退到 lead；末尾不完整的字符整个丢掉。
inline uint64_t Utf8Boundary(const unsigned char* p, uint64_t len, uint64_t target) {
  uint64_t pos = std::min<uint64_t>(target, len);
  int steps = 0;
  while (pos > 0 && pos < len && IsUtf8Continuation(p[pos]) && steps < 3) {
    --pos;
    ++steps;
  }
  if (pos == len) {
    uint64_t q = len;
    int back = 0;
    while (q > 0 && back < 4) {
      --q;
      ++back;
      if (!IsUtf8Continuation(p[q])) break;
    }
    if (q + Utf8CharLen(p[q]) > len) pos = q;
  }
  return pos;
}

// UTF-8/GB18030 的换行是单字节 0x0A。
inline LineCut FindLineEnd8(const unsigned char* p, size_t len, uint64_t targetRel,
                            uint64_t slack) {
  LineCut cut;
  const uint64_t limit = std::min<uint64_t>(len, targetRel + slack + 1);
  for (uint64_t i = targetRel; i < limit; ++i) {
    if (p[i] == 0x0A) {
      cut.pos = i + 1;
      cut.atLineStart = true;
      return cut;
    }
  }
  cut.pos = Utf8Boundary(p, len, targetRel);
  cut.atLineStart = false;
  return cut;
}

inline uint16_t ReadUnit(const unsigned char* p, bool bigEndian) {
  return bigEndian ? static_cast<uint16_t>((p[0] << 8) | p[1])
                   : static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// UTF-16 的换行是 0x000A（按文件字节序）。单位偏移一律偶数。
inline LineCut FindLineEnd16(const unsigned char* p, size_t len, uint64_t targetRel,
                             uint64_t slack, bool bigEndian) {
  LineCut cut;
  const uint64_t limit = std::min<uint64_t>(len, targetRel + slack + 2);
  uint64_t i = targetRel & ~1ull;
  for (; i + 1 < limit; i += 2) {
    if (ReadUnit(p + i, bigEndian) == 0x000A) {
      cut.pos = i + 2;
      cut.atLineStart = true;
      return cut;
    }
  }
  uint64_t pos = std::min<uint64_t>(targetRel, len) & ~1ull;
  // 末尾是高代理项就整对丢掉
  if (pos >= 2 && pos + 1 < len) {
    const uint16_t last = ReadUnit(p + pos - 2, bigEndian);
    if (last >= 0xD800 && last <= 0xDBFF) pos -= 2;
  }
  cut.pos = pos;
  cut.atLineStart = false;
  return cut;
}

inline std::wstring PcreErrorWide(const wchar_t* prefix, int code) {
  std::wstring msg = prefix;
  msg += L"（错误码 ";
  msg += std::to_wstring(code);
  msg += L"）";
  return msg;
}

}  // namespace regex_detail
}  // namespace doc
