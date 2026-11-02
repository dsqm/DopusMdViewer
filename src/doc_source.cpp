// doc_source.cpp - 当前文档索引的生命周期与块正文服务。

#define NOMINMAX
#include "doc_source.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "doc_index.h"
#include "encoding_detect.h"
#include "doc_regex.h"
#include "doc_search.h"
#include "config.h"
#include "log.h"

namespace docsrc {

namespace {

constexpr uint64_t kPrefixBytes = 8ull << 20;
constexpr uint32_t kMaxRequestBlocks = 200;
constexpr uint64_t kMaxRequestBytes = 1ull << 20;
constexpr uint32_t kMaxEstimatedBlocks = 8000000;
constexpr uint8_t kFlagTruncated = 0x80;

struct Snapshot {
  std::unique_ptr<doc::IndexBuilder> builder;
  std::wstring path;
  std::wstring folder;
  uint64_t fileSize = 0;
  doc::EncodingInfo enc;
};

std::wstring ParentDirOf(const std::wstring& path) {
  const size_t pos = path.find_last_of(L"/" + std::wstring(1, static_cast<wchar_t>(92)));
  if (pos == std::wstring::npos) return std::wstring();
  return path.substr(0, pos);
}

std::mutex g_mu;
std::shared_ptr<const Snapshot> g_snap;
std::shared_ptr<std::atomic<bool>> g_cancel;
HWND g_owner = nullptr;
uint32_t g_docId = 0;

uint64_t FileSizeOf(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
  return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

std::string WideToUtf8(const wchar_t* w, int n) {
  if (n <= 0) return std::string();
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return std::string();
  std::string out(static_cast<size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, n, &out[0], len, nullptr, nullptr);
  return out;
}

std::string DecodeWide(const unsigned char* p, size_t n, bool bigEndian) {
  const size_t units = n / 2;
  if (units == 0) return std::string();
  std::wstring w(units, L'\0');
  for (size_t i = 0; i < units; ++i) {
    const unsigned char a = p[2 * i];
    const unsigned char b = p[2 * i + 1];
    w[i] = bigEndian ? static_cast<wchar_t>((a << 8) | b)
                     : static_cast<wchar_t>(a | (b << 8));
  }
  return WideToUtf8(w.data(), static_cast<int>(units));
}

std::string DecodeMbcs(const unsigned char* p, size_t n, UINT cp) {
  if (n == 0) return std::string();
  const int wlen = ::MultiByteToWideChar(cp, 0, reinterpret_cast<const char*>(p),
                                         static_cast<int>(n), nullptr, 0);
  if (wlen <= 0) return std::string(reinterpret_cast<const char*>(p), n);
  std::wstring w(static_cast<size_t>(wlen), L'\0');
  ::MultiByteToWideChar(cp, 0, reinterpret_cast<const char*>(p), static_cast<int>(n),
                        &w[0], wlen);
  return WideToUtf8(w.data(), wlen);
}

void AppendU32(std::string* out, uint32_t v) {
  char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
               static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
  out->append(b, 4);
}

void AppendFrame(std::string* out, uint32_t index, uint8_t flags, const unsigned char* p,
                 size_t n, const doc::EncodingInfo& enc) {
  std::string utf8;
  switch (enc.encoding) {
    case doc::Encoding::Utf8:
      utf8.assign(reinterpret_cast<const char*>(p), n);
      break;
    case doc::Encoding::Legacy:
      utf8 = DecodeMbcs(p, n, static_cast<UINT>(enc.CodePage()));
      break;
    case doc::Encoding::Utf16LE:
      utf8 = DecodeWide(p, n, false);
      break;
    case doc::Encoding::Utf16BE:
      utf8 = DecodeWide(p, n, true);
      break;
  }
  const uint32_t len = static_cast<uint32_t>(utf8.size());
  AppendU32(out, len);
  AppendU32(out, index);
  out->push_back(static_cast<char>(flags));
  out->append(utf8);
}

bool ParseBlocksUri(const std::wstring& uri, uint32_t* from, uint32_t* count) {
  static const wchar_t kBase[] = L"https://mdblocks.localhost/__blocks";
  const size_t baseLen = sizeof(kBase) / sizeof(kBase[0]) - 1;
  if (uri.size() < baseLen || uri.compare(0, baseLen, kBase) != 0) return false;

  const size_t q = uri.find(L'?');
  if (q == std::wstring::npos) return true;
  const std::wstring query = uri.substr(q + 1);

  const size_t pFrom = query.find(L"from=");
  if (pFrom != std::wstring::npos) {
    *from = static_cast<uint32_t>(_wtoi(query.c_str() + pFrom + 5));
  }
  const size_t pCount = query.find(L"count=");
  if (pCount != std::wstring::npos) {
    *count = static_cast<uint32_t>(_wtoi(query.c_str() + pCount + 6));
  }
  return true;
}

// ---------------- 巨型块行窗口 ----------------

constexpr uint32_t kLineCheckpoint = 64;
constexpr size_t kLineCacheEntries = 8;
constexpr uint64_t kMaxWindowBytes = 1ull << 20;
constexpr size_t kBigTableMax = 4096;
constexpr size_t kNoNewline = static_cast<size_t>(-1);

struct LineCache {
  uint32_t docId = 0;
  uint64_t blockStart = 0;
  uint64_t blockEnd = 0;
  uint64_t lastUse = 0;
  std::vector<uint64_t> cp;   // cp[k] = 第 k*64 行的起始字节
  uint32_t scannedLines = 0;  // 已跨过的完整行数
  uint64_t scanOff = 0;       // 第 scannedLines 行的起始字节（只在换行处前移）
  uint32_t totalLines = 0;    // 0 = 还没扫到块尾
};

std::mutex g_lineMu;
std::map<uint32_t, LineCache> g_lineCache;
uint64_t g_lineClock = 0;

// 下一个换行的字节偏移（相对 p）；UTF-16 按 2 字节单元比较，不能直接搜 0x0A。
size_t NextNewline(const unsigned char* p, size_t n, bool wide, bool bigEndian) {
  if (!wide) {
    const void* q = std::memchr(p, '\n', n);
    return q ? static_cast<size_t>(static_cast<const unsigned char*>(q) - p) : kNoNewline;
  }
  for (size_t i = 0; i + 1 < n; i += 2) {
    const unsigned char lo = bigEndian ? p[i + 1] : p[i];
    const unsigned char hi = bigEndian ? p[i] : p[i + 1];
    if (lo == '\n' && hi == 0) return i;
  }
  return kNoNewline;
}

bool ReadAt(HANDLE h, uint64_t off, size_t want, std::vector<unsigned char>* buf, size_t* got) {
  LARGE_INTEGER pos = {};
  pos.QuadPart = static_cast<LONGLONG>(off);
  if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return false;
  DWORD n = 0;
  if (!::ReadFile(h, buf->data(), static_cast<DWORD>(want), &n, nullptr) || n == 0) return false;
  *got = n;
  return true;
}

size_t ChunkBytes(uint64_t remaining, size_t cap, bool wide) {
  size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, cap));
  if (wide) want &= ~size_t(1);
  return want;
}

// 扫到第 line 行之前，顺带把每 64 行一个 checkpoint 补上。
bool ScanToLine(HANDLE h, LineCache* c, uint32_t line, bool wide, bool bigEndian) {
  if (!c || c->scannedLines >= line) return true;
  // 总行数已知也可能只是末尾那一小段读到过（cp 还没覆盖到 line），
  // 不能因此跳过扫描；只有 line 超过总行数才是真的没有。
  if (c->totalLines != 0 && line > c->totalLines) return true;
  const size_t unit = wide ? 2 : 1;
  std::vector<unsigned char> buf(1u << 20);

  uint64_t pos = c->scanOff;  // 当前行的起点；块内的长行可能跨多次读
  while (c->scannedLines < line && pos < c->blockEnd) {
    const size_t want = ChunkBytes(c->blockEnd - pos, buf.size(), wide);
    if (want == 0) break;
    size_t got = 0;
    if (!ReadAt(h, pos, want, &buf, &got)) return false;

    size_t k = 0;
    bool stop = false;
    while (k + unit <= got) {
      const size_t nl = NextNewline(buf.data() + k, got - k, wide, bigEndian);
      if (nl == kNoNewline) {
        k = got - (got - k) % unit;
        break;
      }
      const size_t nlAt = k + nl;
      const uint64_t next = pos + nlAt + unit;
      ++c->scannedLines;
      if (c->scannedLines % kLineCheckpoint == 0) c->cp.push_back(next);
      c->scanOff = next;
      k = nlAt + unit;
      if (c->scannedLines >= line) {
        stop = true;
        break;
      }
    }
    if (stop) break;
    pos += k;
    if (k == 0) break;  // 文件在字符单元中间被截断：剩不足一个单元，别在这里打转
  }

  if (pos >= c->blockEnd && c->totalLines == 0) {
    // 块尾不是换行：最后一行还有字节时算一行
    c->totalLines = c->scannedLines + (c->scanOff < c->blockEnd ? 1 : 0);
  }
  return true;
}

// 第 from 行的起始字节。from 必须已扫过（ScanToLine 保证）。
bool SeekLine(HANDLE h, LineCache* c, uint32_t from, bool wide, bool bigEndian,
              uint64_t* outOff) {
  const uint32_t k = from / kLineCheckpoint;
  if (k >= c->cp.size()) return false;
  uint64_t pos = c->cp[k];
  uint32_t line = k * kLineCheckpoint;
  std::vector<unsigned char> buf(1u << 20);

  while (line < from) {
    if (pos >= c->blockEnd) return false;
    const size_t want = ChunkBytes(c->blockEnd - pos, buf.size(), wide);
    if (want == 0) return false;
    size_t got = 0;
    if (!ReadAt(h, pos, want, &buf, &got)) return false;

    size_t i = 0;
    bool reached = false;
    while (i + (wide ? 2 : 1) <= got) {
      const size_t nl = NextNewline(buf.data() + i, got - i, wide, bigEndian);
      if (nl == kNoNewline) {
        i = got - (got - i) % (wide ? 2 : 1);
        break;
      }
      const size_t nlAt = i + nl;
      ++line;
      i = nlAt + (wide ? 2 : 1);
      if (line == from) {
        reached = true;
        break;
      }
    }
    pos += i;
    if (i == 0) break;  // 文件在字符单元中间被截断：剩不足一个单元
    if (reached) break;
  }
  if (line != from) return false;
  *outOff = pos;
  return true;
}

// 从 start（某行行首）读 count 行；got 含块尾没有换行的最后一行。
// reachedEnd 只有真的读到块尾才是 true：1 MB 窗口装满提前返回时必须是 false，
// 否则上层会把 totalLines 写死，巨型块更深的行永远取不到。
bool ReadLines(HANDLE h, LineCache* c, uint64_t start, uint32_t count, bool wide,
               bool bigEndian, std::string* raw, uint32_t* gotLines, bool* reachedEnd) {
  const size_t unit = wide ? 2 : 1;
  std::vector<unsigned char> buf(1u << 20);
  uint64_t pos = start;
  uint32_t got = 0;
  bool end = false;

  while (got < count && pos < c->blockEnd && raw->size() < kMaxWindowBytes) {
    const size_t cap = static_cast<size_t>(
        std::min<uint64_t>(buf.size(), kMaxWindowBytes - raw->size()));
    const size_t want = ChunkBytes(c->blockEnd - pos, cap, wide);
    if (want == 0) break;
    size_t n = 0;
    if (!ReadAt(h, pos, want, &buf, &n)) return false;

    const uint64_t chunkStart = pos;
    size_t i = 0;
    size_t lineStart = 0;  // 最后一段没有换行的字节从 buf 的哪里开始
    bool stop = false;
    while (i + unit <= n) {
      const size_t nl = NextNewline(buf.data() + i, n - i, wide, bigEndian);
      if (nl == kNoNewline) {
        i = n - (n - i) % unit;
        break;
      }
      const size_t nlAt = i + nl;
      const size_t lineBytes = nlAt + unit - i;
      if (raw->size() + lineBytes > kMaxWindowBytes) {
        stop = true;  // 单行就放不下：停在完整行边界
        break;
      }
      raw->append(reinterpret_cast<const char*>(buf.data() + i), lineBytes);
      ++got;
      i = nlAt + unit;
      lineStart = i;
      if (got >= count) {
        stop = true;
        break;
      }
    }
    if (stop) break;

    // 读到块尾：lineStart 起的剩余字节就是最后一行（它没有换行结尾）。
    // 这一步必须在退出前做，否则每个巨型块的最后一行永远少一条，行总数也少 1。
    if (chunkStart + n >= c->blockEnd) {
      const size_t tailLen = n - lineStart;
      if (tailLen > 0) {
        if (raw->size() + tailLen > kMaxWindowBytes) {
          // 行还在，只是内容放不进窗口：计数不能丢，否则 totalLines 少一行。
          ++got;
        } else {
          raw->append(reinterpret_cast<const char*>(buf.data() + lineStart), tailLen);
          ++got;
        }
      }
      pos = c->blockEnd;
      end = true;
      break;
    }
    pos = chunkStart + i;
  }
  *gotLines = got;
  if (reachedEnd) *reachedEnd = end;
  return true;
}

uint32_t QueryU32(const std::wstring& query, const wchar_t* key, uint32_t def) {
  const std::wstring k = std::wstring(key) + L"=";
  const size_t p = query.find(k);
  if (p == std::wstring::npos) return def;
  return static_cast<uint32_t>(_wtoi(query.c_str() + p + k.size()));
}

std::string ToUtf8(const doc::EncodingInfo& enc, std::string raw) {
  switch (enc.encoding) {
    case doc::Encoding::Utf8:
      return raw;
    case doc::Encoding::Legacy:
      return DecodeMbcs(reinterpret_cast<const unsigned char*>(raw.data()), raw.size(),
                        static_cast<UINT>(enc.CodePage()));
    case doc::Encoding::Utf16LE:
      return DecodeWide(reinterpret_cast<const unsigned char*>(raw.data()), raw.size(), false);
    case doc::Encoding::Utf16BE:
      return DecodeWide(reinterpret_cast<const unsigned char*>(raw.data()), raw.size(), true);
  }
  return raw;
}

void AppendU16(std::string* out, uint16_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
}

std::string WideToUtf8String(const std::wstring& s) {
  return WideToUtf8(s.data(), static_cast<int>(s.size()));
}

std::string WideToMbcs(const std::wstring& s, UINT cp) {
  if (s.empty()) return std::string();
  const int n = ::WideCharToMultiByte(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  if (!::WideCharToMultiByte(cp, 0, s.data(), static_cast<int>(s.size()), &out[0], n, nullptr,
                             nullptr)) {
    return std::string();
  }
  return out;
}

inline bool IsAsciiWordUnit(int c) {
  return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
         c == L'_';
}

// UTF-16 源文件上的宽字符搜索：单线程顺序读，返回源字节偏移。
// 只折叠 ASCII 字母；UTF-16 文档通常不大，不为此再拆线程。
doc::SearchHits SearchWideFile(const std::wstring& path, const std::wstring& needle,
                               bool caseSensitive, bool bigEndian, bool wholeWord,
                               const std::atomic<bool>* cancel) {
  doc::SearchHits result;
  const size_t m = needle.size();
  if (m == 0) return result;

  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return result;

  // 整词匹配要看命中后一个单元，它在顺序读的前面，只能定位读：用独立句柄，
  // 不在同步句柄上玩 OVERLAPPED（规范外用法，个别文件系统会动顺序指针）。
  HANDLE hp = INVALID_HANDLE_VALUE;
  if (wholeWord) {
    hp = ::CreateFileW(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  }

  LARGE_INTEGER fsize = {};
  ::GetFileSizeEx(h, &fsize);
  const uint64_t fileUnits = static_cast<uint64_t>(fsize.QuadPart) / 2;

  auto fold = [caseSensitive](wchar_t c) -> wchar_t {
    if (!caseSensitive && c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c + 32);
    return c;
  };

  std::vector<size_t> shift(65536, m);
  for (size_t i = 0; i + 1 < m; ++i) shift[static_cast<uint16_t>(fold(needle[i]))] = m - 1 - i;
  const wchar_t last = fold(needle[m - 1]);

  constexpr size_t kChunkUnits = 1u << 19;  // 512 K 个 wchar_t
  std::vector<unsigned char> raw(kChunkUnits * 2);
  std::vector<wchar_t> buf;
  buf.reserve(kChunkUnits + m);
  uint64_t bufStartUnit = 0;
  uint64_t nextUnit = 0;
  std::vector<wchar_t> carry;
  int prevUnit = -1;  // 缓冲区起点前一个单元；文件开头没有

  for (;;) {
    if (cancel && cancel->load(std::memory_order_relaxed)) break;
    DWORD got = 0;
    if (!::ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &got, nullptr) || got == 0) {
      break;
    }
    const size_t units = got / 2;
    buf.assign(carry.begin(), carry.end());
    const size_t carryLen = buf.size();
    for (size_t i = 0; i < units; ++i) {
      const unsigned char a = raw[2 * i];
      const unsigned char b = raw[2 * i + 1];
      buf.push_back(bigEndian ? static_cast<wchar_t>((a << 8) | b)
                              : static_cast<wchar_t>(a | (b << 8)));
    }
    const uint64_t startUnit = nextUnit - carryLen;
    bufStartUnit = startUnit;

    // 整词匹配要看命中后一个单元：它在下一个读块里，用独立句柄定位读一次。
    int nextAfter = -1;
    const bool moreData = nextUnit + units < fileUnits;
    if (wholeWord && moreData && hp != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER p = {};
      p.QuadPart = static_cast<LONGLONG>((nextUnit + units) * 2);
      if (::SetFilePointerEx(hp, p, nullptr, FILE_BEGIN)) {
        unsigned char pair[2] = {};
        DWORD got2 = 0;
        if (::ReadFile(hp, pair, 2, &got2, nullptr) && got2 == 2) {
          nextAfter = bigEndian ? static_cast<int>((pair[0] << 8) | pair[1])
                                : static_cast<int>(pair[0] | (pair[1] << 8));
        }
      }
    }

    size_t k = m - 1;
    while (k < buf.size()) {
      const wchar_t c = fold(buf[k]);
      if (c == last) {
        bool match = true;
        for (size_t j = 0; j < m; ++j) {
          if (fold(buf[k - (m - 1) + j]) != fold(needle[j])) {
            match = false;
            break;
          }
        }
        if (match) {
          bool whole = true;
          if (wholeWord) {
            const int pb = (k == m - 1) ? prevUnit : static_cast<uint16_t>(buf[k - m]);
            const int nb = (k + 1 < buf.size()) ? static_cast<uint16_t>(buf[k + 1])
                                                : nextAfter;
            whole = !IsAsciiWordUnit(pb) && !IsAsciiWordUnit(nb);
          }
          if (whole) {
            ++result.total;
            if (result.offsets.size() < 10000) {
              result.offsets.push_back((bufStartUnit + k - (m - 1)) * 2);
            }
          }
        }
        ++k;
      } else {
        k += shift[static_cast<uint16_t>(c)];
      }
    }

    nextUnit += units;
    const size_t keep = std::min<size_t>(m - 1, buf.size());
    if (wholeWord && keep < buf.size()) {
      prevUnit = static_cast<uint16_t>(buf[buf.size() - keep - 1]);
    }
    carry.assign(buf.end() - static_cast<ptrdiff_t>(keep), buf.end());
  }
  ::CloseHandle(h);
  if (hp != INVALID_HANDLE_VALUE) ::CloseHandle(hp);
  result.truncated = result.total > result.offsets.size();
  return result;
}

struct SearchEntry {
  uint64_t off = 0;
  uint32_t block = 0;
  uint32_t units = 0;
  uint32_t line = 0;  // 块内行号（0 起，只对巨型块填；其余块前端不看）
};

// 命中在巨型块里的行号：行号 = 命中偏移之前出现过的换行数，与 __lines 的行下标一致。
// 只对巨型块调用（一块一次顺读，命中按偏移升序，边扫边填）。
void FillBlockLines(HANDLE h, const doc::EncodingInfo& enc, uint64_t blockStart,
                    std::vector<SearchEntry>* list, size_t first, size_t last) {
  const bool wide = enc.IsUtf16();
  const bool bigEndian = enc.encoding == doc::Encoding::Utf16BE;
  const size_t step = wide ? 2 : 1;
  const uint64_t lastOff = (*list)[last].off;
  if (lastOff <= blockStart) return;

  std::vector<unsigned char> buf(1u << 20);
  uint64_t pos = blockStart;
  uint32_t base = 0;  // 本块之前的行数
  size_t idx = first;

  while (idx <= last && pos <= lastOff) {
    const uint64_t want =
        std::min<uint64_t>(buf.size(), lastOff - pos + step);
    size_t got = 0;
    if (want == 0 || !ReadAt(h, pos, static_cast<size_t>(want), &buf, &got) || got == 0) break;

    uint32_t seen = 0;
    size_t i = 0;
    while (i + step <= got) {
      const uint64_t abs = pos + i;
      while (idx <= last && (*list)[idx].off <= abs) {
        (*list)[idx].line = base + seen;
        ++idx;
      }
      const bool nl = wide ? (bigEndian ? (buf[i] == 0 && buf[i + 1] == 0x0A)
                                        : (buf[i] == 0x0A && buf[i + 1] == 0))
                           : (buf[i] == 0x0A);
      if (nl) ++seen;
      i += step;
    }
    base += seen;
    pos += i;
    if (i == 0) break;
  }
}

// 该块是否是巨型块（与 __big 表同源：索引里的 bigBlockIndex_ 递增）。
bool IsBigBlock(const doc::BlockIndex& ix, uint32_t block) {
  const std::vector<uint32_t>& v = ix.bigBlockIndex();
  return std::binary_search(v.begin(), v.end(), block);
}

// 块内命中 → 解码文本里的 UTF-16 单元偏移。命中按偏移升序，同一块的命中一次扫完。
// UTF-8 按首字节数，UTF-16 直接除以 2，其余编码按 MB2WC 顺解计数（ISO-2022 的
// ESC 状态天然正确，代理对由 MB2WC 给出 2 单元）。
void FillBlockUnits(HANDLE h, const doc::EncodingInfo& enc, uint64_t blockStart,
                    std::vector<SearchEntry>* list, size_t first, size_t last) {
  if (enc.IsUtf16()) {
    for (size_t i = first; i <= last; ++i) {
      (*list)[i].units = static_cast<uint32_t>(((*list)[i].off - blockStart) / 2);
    }
    return;
  }

  const uint64_t lastOff = (*list)[last].off;
  uint64_t pos = blockStart;
  uint32_t units = 0;
  size_t idx = first;
  std::vector<unsigned char> buf(1u << 20);

  if (enc.encoding == doc::Encoding::Legacy) {
    const UINT cp = static_cast<UINT>(enc.CodePage());
    while (idx <= last) {
      while (idx <= last && (*list)[idx].off <= pos) {
        (*list)[idx].units = units;
        ++idx;
      }
      if (idx > last || pos >= lastOff) break;

      const uint64_t want = std::min<uint64_t>(buf.size(), (*list)[idx].off - pos);
      size_t got = 0;
      if (want == 0 || !ReadAt(h, pos, static_cast<size_t>(want), &buf, &got) || got == 0) break;
      // 切在换行之后：ISO-2022 的移位状态按行复位，每次解码都从行首起。
      size_t use = got;
      for (size_t k = got; k > 0; --k) {
        if (buf[k - 1] == 0x0A) {
          use = k;
          break;
        }
      }
      // 段尾正好停在 DBCS lead byte 上（trail 还没读进来）时退掉这个字节：
      // 让 lead 归下一个读段，别让本段把残缺序列折算成一个假单元。trail 已在
      // 段内时边界是完整的，不能再退（GBK 的 trail 可能同时是合法 lead）。
      if (use == got && use > 0 && ::IsDBCSLeadByteEx(cp, buf[use - 1])) --use;
      if (use == 0) break;  // 整段只有一个 lead byte：pos 无法前进，别打转
      const int n = ::MultiByteToWideChar(cp, 0, reinterpret_cast<LPCCH>(buf.data()),
                                          static_cast<int>(use), nullptr, 0);
      if (n > 0) units += static_cast<uint32_t>(n);
      pos += use;
    }
    while (idx <= last) {
      (*list)[idx].units = units;
      ++idx;
    }
    return;
  }

  while (idx <= last) {
    while (idx <= last && (*list)[idx].off <= pos) {
      (*list)[idx].units = units;
      ++idx;
    }
    if (idx > last || pos >= lastOff) break;

    const uint64_t want =
        std::min<uint64_t>(buf.size(), lastOff - pos + static_cast<uint64_t>(4));
    size_t got = 0;
    if (!ReadAt(h, pos, static_cast<size_t>(want), &buf, &got) || got == 0) break;

    size_t i = 0;
    while (i < got) {
      const unsigned char b = buf[i];
      uint32_t len = 0;
      uint32_t add = 1;
      if (b < 0x80) {
        len = 1;
      } else if ((b & 0xE0) == 0xC0) {
        len = 2;
      } else if ((b & 0xF0) == 0xE0) {
        len = 3;
      } else if ((b & 0xF8) == 0xF0) {
        len = 4;
        add = 2;  // 4 字节 UTF-8 在 JS 里占两个 UTF-16 单元
      } else {
        len = 1;
      }
      if (i + len > got) break;

      while (idx <= last && (*list)[idx].off < pos + i) {
        (*list)[idx].units = units;
        ++idx;
      }
      units += add;
      i += len;
    }

    if (i == 0) break;  // 读块太小或数据异常，避免死循环
    pos += i;
  }

  while (idx <= last) {
    (*list)[idx].units = units;
    ++idx;
  }
}

std::shared_ptr<const Snapshot> MakeSnapshot(std::unique_ptr<doc::IndexBuilder> builder,
                                             const std::wstring& path, uint64_t fileSize) {  auto snap = std::make_shared<Snapshot>();
  snap->builder = std::move(builder);
  snap->path = path;
  snap->folder = ParentDirOf(path);
  snap->fileSize = fileSize;
  snap->enc = snap->builder->encoding();
  return snap;
}

}  // namespace

bool Open(HWND owner, const std::wstring& path, DocumentInfo* info, std::wstring* error,
          uint32_t overrideCodePage) {
  Close();

  doc::EncodingInfo overrideEnc;
  const bool hasOverride = doc::EncodingFromCodePage(overrideCodePage, &overrideEnc);
  const doc::EncodingInfo* overridePtr = hasOverride ? &overrideEnc : nullptr;

  const uint64_t fileSize = FileSizeOf(path);
  auto builder = std::unique_ptr<doc::IndexBuilder>(new doc::IndexBuilder());
  std::wstring err;
  if (!doc::ScanFile(path, kPrefixBytes, builder.get(), nullptr, nullptr, &err, nullptr,
                     overridePtr)) {
    if (error) *error = err.empty() ? L"读取失败" : err;
    LogLine(L"DOCSRC: 打开失败 %s (%s)", path.c_str(), err.c_str());
    return false;
  }

  auto snap = MakeSnapshot(std::move(builder), path, fileSize);
  const uint32_t prefixBlocks = snap->builder->index().BlockCount();
  const uint64_t scanned = snap->builder->BytesScanned();
  const bool complete = scanned >= fileSize || fileSize == 0;

  uint32_t estimated = prefixBlocks;
  if (!complete && prefixBlocks > 0) {
    const uint64_t avg = std::max<uint64_t>(1, scanned / prefixBlocks);
    uint64_t est = fileSize / avg;
    if (est < prefixBlocks) est = prefixBlocks;
    if (est > kMaxEstimatedBlocks) est = kMaxEstimatedBlocks;
    estimated = static_cast<uint32_t>(est);
  }

  std::shared_ptr<std::atomic<bool>> cancel;
  uint32_t docId = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_snap = snap;
    g_owner = owner;
    ++g_docId;
    docId = g_docId;
    if (!complete) {
      cancel = std::make_shared<std::atomic<bool>>(false);
      g_cancel = cancel;
    } else {
      g_cancel.reset();
    }
  }

  if (info) {
    info->prefixBlocks = prefixBlocks;
    info->estimatedBlocks = estimated;
    info->lastVisible = 0;
    const doc::BlockIndex& ix = snap->builder->index();
    for (uint32_t i = ix.BlockCount(); i > 0; --i) {
      if (!ix.IsHidden(i - 1)) {
        info->lastVisible = i - 1;
        break;
      }
    }
    info->fileSize = fileSize;
    info->encoding = static_cast<uint8_t>(snap->enc.encoding);
    info->complete = complete;
  }
  LogLine(L"DOCSRC: 打开 \"%s\" prefix=%u est=%u 文件=%llu B 完整=%d 编码=%s cp=%u",
          path.c_str(), prefixBlocks, estimated,
          static_cast<unsigned long long>(fileSize), complete ? 1 : 0,
          snap->enc.encoding == doc::Encoding::Utf8        ? L"UTF-8"
          : snap->enc.encoding == doc::Encoding::Utf16LE   ? L"UTF-16LE"
          : snap->enc.encoding == doc::Encoding::Utf16BE   ? L"UTF-16BE"
                                                           : L"Legacy",
          snap->enc.CodePage());

  if (!complete) {
    std::thread([path, fileSize, docId, cancel, hasOverride, overrideEnc]() {
      auto full = std::unique_ptr<doc::IndexBuilder>(new doc::IndexBuilder());
      std::wstring err2;
      if (!doc::ScanFile(path, 0, full.get(), nullptr, nullptr, &err2, cancel.get(),
                         hasOverride ? &overrideEnc : nullptr)) {
        if (!cancel->load()) LogLine(L"DOCSRC: 后台扫描失败 %s", err2.c_str());
        return;
      }
      if (cancel->load()) return;
      auto fullSnap = MakeSnapshot(std::move(full), path, fileSize);
      HWND notify = nullptr;
      {
        std::lock_guard<std::mutex> lock(g_mu);
        if (docId != g_docId) return;  // 期间又换了文件
        g_snap = fullSnap;
        g_cancel.reset();
        notify = g_owner;
      }
      LogLine(L"DOCSRC: 全文索引完成 %u 块", fullSnap->builder->index().BlockCount());
      if (notify && ::IsWindow(notify)) {
        ::PostMessageW(notify, WM_MDV_INDEX_FULL, docId, 0);
      }
    }).detach();
  }
  return true;
}

void Close() {
  std::shared_ptr<std::atomic<bool>> cancel;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    ++g_docId;
    cancel = g_cancel;
    g_cancel.reset();
    g_snap.reset();
    g_owner = nullptr;
  }
  if (cancel) cancel->store(true);
  {
    std::lock_guard<std::mutex> lock(g_lineMu);
    g_lineCache.clear();
  }
}

bool IsOwner(HWND owner) {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_owner == owner;
}

HWND Owner() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_owner;
}

void CloseIfOwner(HWND owner) {
  bool mine = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    mine = g_owner == owner;
  }
  if (mine) Close();
}

uint32_t PublishedBlockCount() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_snap) return 0;
  return g_snap->builder->index().BlockCount();
}

uint32_t LastVisible() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_snap) return 0;
  const doc::BlockIndex& ix = g_snap->builder->index();
  for (uint32_t i = ix.BlockCount(); i > 0; --i) {
    if (!ix.IsHidden(i - 1)) return i - 1;
  }
  return 0;
}

int HexVal(wchar_t c) {
  if (c >= L'0' && c <= L'9') return static_cast<int>(c - L'0');
  if (c >= L'a' && c <= L'f') return static_cast<int>(c - L'a') + 10;
  if (c >= L'A' && c <= L'F') return static_cast<int>(c - L'A') + 10;
  return -1;
}

// 把 URL 里的百分号编码还原成 UTF-8，再转成宽字符路径。
bool PercentDecode(const std::wstring& in, std::wstring* out) {
  std::string bytes;
  bytes.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    const wchar_t c = in[i];
    if (c == L'%' && i + 2 < in.size()) {
      const int hi = HexVal(in[i + 1]);
      const int lo = HexVal(in[i + 2]);
      if (hi < 0 || lo < 0) return false;
      bytes.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
      continue;
    }
    if (c < 0x80) {
      bytes.push_back(static_cast<char>(c));
      continue;
    }
    char buf[4];
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, &c, 1, buf, sizeof(buf), nullptr, nullptr);
    if (n <= 0) return false;
    bytes.append(buf, static_cast<size_t>(n));
  }
  if (bytes.empty()) return false;
  const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                         static_cast<int>(bytes.size()), nullptr, 0);
  if (wlen <= 0) return false;
  out->resize(static_cast<size_t>(wlen));
  ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &(*out)[0],
                        wlen);
  return true;
}

bool SafeRelativePath(const std::wstring& rel) {
  if (rel.empty()) return false;
  if (rel[0] == L'/' || rel[0] == L'\\') return false;
  if (rel.find(L':') != std::wstring::npos) return false;
  size_t start = 0;
  while (start <= rel.size()) {
    const size_t pos = rel.find_first_of(L"/\\", start);
    const std::wstring part =
        rel.substr(start, pos == std::wstring::npos ? std::wstring::npos : pos - start);
    if (part == L"..") return false;
    if (part.empty() && pos != std::wstring::npos) return false;
    if (pos == std::wstring::npos) break;
    start = pos + 1;
  }
  return true;
}

bool IsAbsolutePath(const std::wstring& p) {
  if (p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
      p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) {
    return true;
  }
  if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return true;  // UNC
  if (!p.empty() && (p[0] == L'\\' || p[0] == L'/')) return true;    // 根相对
  return false;
}

// 设置里允许文档目录外图片时：相对路径按文档目录解析（允许 ..），绝对路径直接用。
// 统一走 GetFullPathNameW 归一化；超长路径加 \\?\ 前缀，否则 CreateFile 打不开。
bool ResolveAssetPath(const std::wstring& folder, const std::wstring& rel,
                      std::wstring* full) {
  std::wstring candidate = rel;
  for (wchar_t& c : candidate) {
    if (c == L'/') c = L'\\';
  }
  if (!IsAbsolutePath(candidate)) {
    candidate = folder;
    if (!candidate.empty() && candidate.back() != L'\\') candidate += L'\\';
    candidate += rel;
    for (wchar_t& c : candidate) {
      if (c == L'/') c = L'\\';
    }
  }

  std::vector<wchar_t> buf(32768);
  const DWORD n = ::GetFullPathNameW(candidate.c_str(), static_cast<DWORD>(buf.size()),
                                     buf.data(), nullptr);
  if (n == 0 || n >= buf.size()) return false;
  *full = std::wstring(buf.data(), n);

  if (full->size() >= MAX_PATH) {
    if (full->size() >= 2 && (*full)[0] == L'\\' && (*full)[1] == L'\\') {
      *full = L"\\\\?\\UNC\\" + full->substr(2);
    } else {
      *full = L"\\\\?\\" + *full;
    }
  }
  return true;
}

std::wstring ContentTypeFor(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos) return L"application/octet-stream";
  std::wstring ext = path.substr(dot + 1);
  for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
  if (ext == L"png") return L"image/png";
  if (ext == L"jpg" || ext == L"jpeg") return L"image/jpeg";
  if (ext == L"gif") return L"image/gif";
  if (ext == L"webp") return L"image/webp";
  if (ext == L"avif") return L"image/avif";
  if (ext == L"bmp") return L"image/bmp";
  if (ext == L"ico") return L"image/x-icon";
  if (ext == L"svg") return L"image/svg+xml";
  if (ext == L"tif" || ext == L"tiff") return L"image/tiff";
  return L"application/octet-stream";
}

bool ReadAsset(const std::wstring& full, std::string* body) {
  constexpr uint64_t kMaxAssetBytes = 64ull << 20;
  HANDLE h = ::CreateFileW(full.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size = {};
  if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
      static_cast<uint64_t>(size.QuadPart) > kMaxAssetBytes) {
    ::CloseHandle(h);
    return false;
  }
  body->resize(static_cast<size_t>(size.QuadPart));
  DWORD got = 0;
  const BOOL ok = ::ReadFile(h, &(*body)[0], static_cast<DWORD>(body->size()), &got, nullptr);
  ::CloseHandle(h);
  if (!ok || got != body->size()) {
    body->clear();
    return false;
  }
  return true;
}

bool ServeAsset(const std::wstring& uri, std::string* body, int* status,
                std::wstring* contentType) {
  if (!body || !status || !contentType) return false;
  body->clear();
  *status = 404;

  static const wchar_t kAssetBase[] = L"https://mdblocks.localhost/__asset";
  const size_t baseLen = sizeof(kAssetBase) / sizeof(kAssetBase[0]) - 1;
  if (uri.size() < baseLen || uri.compare(0, baseLen, kAssetBase) != 0) return false;

  const size_t q = uri.find(L"?p=");
  if (q == std::wstring::npos) return true;
  std::wstring raw = uri.substr(q + 3);
  const size_t amp = raw.find(L'&');
  if (amp != std::wstring::npos) raw.resize(amp);

  std::wstring rel;
  if (!PercentDecode(raw, &rel) || rel.empty()) return true;

  std::shared_ptr<const Snapshot> snap;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    snap = g_snap;
  }
  if (!snap || snap->folder.empty()) return true;

  std::wstring full;
  const bool allowOutside = ConfigAllowOutsideImages();
  if (allowOutside) {
    if (!ResolveAssetPath(snap->folder, rel, &full)) return true;
    // 放行目录外路径时只认图片扩展名，别让页面借 __asset 读任意文件
    if (ContentTypeFor(full) == L"application/octet-stream") return true;
  } else {
    if (!SafeRelativePath(rel)) return true;
    full = snap->folder;
    if (full.back() != L'\\' && full.back() != L'/') full += L'\\';
    for (wchar_t c : rel) full += (c == L'/') ? L'\\' : c;
  }

  const DWORD attr = ::GetFileAttributesW(full.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) return true;
  if (!ReadAsset(full, body)) return true;

  *contentType = ContentTypeFor(full);
  *status = 200;
  return true;
}

// 响应 https://mdblocks.localhost/__lines：巨型块的行窗口。
bool ServeLines(const std::wstring& uri, std::string* body, int* status) {
  if (!body || !status) return false;
  body->clear();
  *status = 404;

  static const wchar_t kBase[] = L"https://mdblocks.localhost/__lines";
  const size_t baseLen = sizeof(kBase) / sizeof(kBase[0]) - 1;
  if (uri.size() < baseLen || uri.compare(0, baseLen, kBase) != 0) return false;

  const size_t q = uri.find(L'?');
  const std::wstring query =
      q == std::wstring::npos ? std::wstring() : uri.substr(q + 1);
  const uint32_t block = QueryU32(query, L"i", 0xFFFFFFFFu);
  const uint32_t from = QueryU32(query, L"from", 0);
  uint32_t count = QueryU32(query, L"count", 128);
  if (count == 0) count = 128;
  if (count > 512) count = 512;

  std::shared_ptr<const Snapshot> snap;
  uint32_t docId = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    snap = g_snap;
    docId = g_docId;
  }
  *status = 200;
  if (!snap) return true;

  const doc::BlockIndex& ix = snap->builder->index();
  if (block >= ix.BlockCount()) return true;
  const uint64_t blockStart = ix.Start(block);
  const uint64_t blockEnd = ix.End(block);
  if (blockEnd <= blockStart) return true;

  const bool wide = snap->enc.IsUtf16();
  const bool bigEndian = snap->enc.encoding == doc::Encoding::Utf16BE;

  HANDLE h = ::CreateFileW(snap->path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    LogLine(L"DOCSRC: __lines 打开失败 i=%u err=%lu", block, ::GetLastError());
    *status = 404;
    return true;
  }
  const ULONGLONG t0 = ::GetTickCount64();

  std::lock_guard<std::mutex> guard(g_lineMu);
  auto it = g_lineCache.find(block);
  if (it == g_lineCache.end() || it->second.docId != docId ||
      it->second.blockStart != blockStart || it->second.blockEnd != blockEnd) {
    LineCache fresh;
    fresh.docId = docId;
    fresh.blockStart = blockStart;
    fresh.blockEnd = blockEnd;
    fresh.cp.push_back(blockStart);
    fresh.scanOff = blockStart;
    g_lineCache[block] = fresh;
    it = g_lineCache.find(block);
    if (g_lineCache.size() > kLineCacheEntries) {
      auto victim = g_lineCache.begin();
      for (auto cur = g_lineCache.begin(); cur != g_lineCache.end(); ++cur) {
        if (cur->first != block && cur->second.lastUse < victim->second.lastUse) victim = cur;
      }
      if (victim->first != block) g_lineCache.erase(victim);
    }
  }
  LineCache& c = it->second;
  c.lastUse = ++g_lineClock;

  bool ok = ScanToLine(h, &c, from, wide, bigEndian);
  const uint32_t total = c.totalLines;
  uint64_t startOff = blockStart;
  const bool seekOk = !ok || from == 0 || SeekLine(h, &c, from, wide, bigEndian, &startOff);
  ok = ok && seekOk;
  std::string raw;
  uint32_t got = 0;
  bool readOk = true;
  if (ok && (total == 0 || from < total)) {
    const uint32_t want = total ? std::min<uint32_t>(count, total - from) : count;
    bool reachedEnd = false;
    readOk = ReadLines(h, &c, startOff, want, wide, bigEndian, &raw, &got, &reachedEnd);
    // 只有真读到块尾才能定总行数：窗口装满提前返回时后面还有行。
    if (readOk && got < want && c.totalLines == 0 && reachedEnd) {
      c.totalLines = from + got;
    }
  } else if (ok && total != 0 && from >= total) {
    got = 0;
  }
  ::CloseHandle(h);
  const ULONGLONG elapsed = ::GetTickCount64() - t0;
  if (!ok || !readOk) {
    LogLine(L"DOCSRC: __lines 失败 i=%u from=%u cnt=%u scan=%d seek=%d read=%d 扫描到第 %u 行",
            block, from, count, ok ? 1 : 0, seekOk ? 1 : 0, readOk ? 1 : 0, c.scannedLines);
    *status = 404;
    return true;
  }
  if (elapsed > 200) {
    LogLine(L"DOCSRC: __lines 慢请求 i=%u from=%u cnt=%u 用 %llu ms（扫到第 %u 行）", block,
            from, count, static_cast<unsigned long long>(elapsed), c.scannedLines);
  }

  std::string utf8 = ToUtf8(snap->enc, std::move(raw));

  AppendU32(body, block);
  AppendU32(body, from);
  AppendU32(body, got);
  AppendU32(body, c.totalLines);
  AppendU32(body, static_cast<uint32_t>(utf8.size()));
  body->append(utf8);
  return true;
}

void BigTable(std::string* out) {
  if (!out) return;
  out->clear();
  std::shared_ptr<const Snapshot> snap;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    snap = g_snap;
  }
  if (!snap) return;
  const doc::BlockIndex& ix = snap->builder->index();
  const std::vector<uint32_t>& blocks = ix.bigBlockIndex();
  const std::vector<uint32_t>& lines = ix.bigBlockLines();
  const size_t n = std::min(blocks.size(), kBigTableMax);
  for (size_t k = 0; k < n; ++k) {
    if (k) out->push_back(',');
    out->append(std::to_string(blocks[k]));
    out->push_back(':');
    out->append(std::to_string(lines[k]));
    // 第三段：表格标记。表格与其它巨型块的前端渲染路径不同（分块真表格 / 行窗口）。
    // 第四段：块类型（doc::Kind）。代码块靠它把围栏行去掉、行号按代码行算；
    // 老前端只读到第三段，多的字段忽略。
    const uint8_t kind = static_cast<uint8_t>(ix.KindFlags(blocks[k]) & doc::kKindMask);
    out->append(kind == static_cast<uint8_t>(doc::Kind::Table) ? ":1:" : ":0:");
    out->append(std::to_string(kind));
  }
}

void PatchU32(std::string* out, size_t off, uint32_t v) {
  if (off + 4 > out->size()) return;
  (*out)[off] = static_cast<char>(v & 0xFF);
  (*out)[off + 1] = static_cast<char>((v >> 8) & 0xFF);
  (*out)[off + 2] = static_cast<char>((v >> 16) & 0xFF);
  (*out)[off + 3] = static_cast<char>((v >> 24) & 0xFF);
}

bool IsSearchUri(const std::wstring& uri) {
  static const wchar_t kBase[] = L"https://mdblocks.localhost/__search";
  const size_t baseLen = sizeof(kBase) / sizeof(kBase[0]) - 1;
  if (uri.size() < baseLen || uri.compare(0, baseLen, kBase) != 0) return false;
  // 前缀匹配之后必须紧跟 ? 或结束，否则 __searchfoo 也会被当成搜索。
  return uri.size() == baseLen || uri[baseLen] == L'?';
}

bool SearchBody(const std::wstring& uri, std::string* body, int* status,
                const std::atomic<bool>* cancel) {
  if (!body || !status) return false;
  body->clear();
  *status = 404;

  if (!IsSearchUri(uri)) return false;
  *status = 200;

  const size_t q = uri.find(L'?');
  const std::wstring query = q == std::wstring::npos ? std::wstring() : uri.substr(q + 1);
  const size_t pQ = query.find(L"q=");
  if (pQ == std::wstring::npos) return true;
  std::wstring raw = query.substr(pQ + 2);
  const size_t amp = raw.find(L'&');
  if (amp != std::wstring::npos) raw.resize(amp);

  std::wstring needle;
  if (!PercentDecode(raw, &needle)) return true;
  const bool caseSensitive = QueryU32(query, L"cs", 0) != 0;
  const bool regex = QueryU32(query, L"re", 0) != 0;
  const bool wholeWord = !regex && QueryU32(query, L"ww", 0) != 0;
  const size_t maxNeedle = regex ? 1024 : 200;
  if (needle.size() > maxNeedle) {
    needle.resize(maxNeedle);
    if (needle.back() >= 0xD800 && needle.back() <= 0xDBFF) needle.pop_back();
  }
  if (needle.empty()) return true;

  std::shared_ptr<const Snapshot> snap;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    snap = g_snap;
  }
  if (!snap) return true;

  const ULONGLONG t0 = ::GetTickCount64();
  const bool wide = snap->enc.IsUtf16();
  const UINT cp = static_cast<UINT>(snap->enc.CodePage());
  // ISO-2022 的针脚字节里带移位序列，直接搜字节会随行首状态变化而漏配，只能整块解码后匹配。
  const bool stateful = snap->enc.encoding == doc::Encoding::Legacy && doc::IsIso2022CodePage(cp);
  doc::SearchHits hits;
  bool limited = false;
  const wchar_t* how = L"";
  if (regex) {
    doc::RegexHits rh;
    std::wstring reError;
    if (!doc::RegexSearchFile(snap->path, snap->enc, needle, caseSensitive, cancel,
                              &rh, &reError)) {
      // flags bit1：正则错误，帧尾跟一条 u32 长度 + UTF-8 消息。
      AppendU32(body, 0);
      AppendU32(body, 0);
      AppendU32(body, 2);
      const std::string msg = WideToUtf8String(std::wstring(L"正则无效：") + reError);
      AppendU32(body, static_cast<uint32_t>(msg.size()));
      body->append(msg);
      LogLine(L"DOCSRC: __search re \"%s\" 失败: %s ms=%llu", needle.c_str(),
              reError.c_str(),
              static_cast<unsigned long long>(::GetTickCount64() - t0));
      return true;
    }
    hits.offsets = std::move(rh.offsets);
    hits.total = rh.total;
    hits.truncated = rh.truncated;
    limited = rh.limited;
  } else if (stateful || (!caseSensitive && doc::LiteralNeedsUnicodeFold(needle))) {
    // 非 ASCII 的大小写折叠只有 PCRE2 有（字节与宽字符扫描器只折叠 ASCII）。
    // 纯 ASCII 与无大小写的文字（中日韩）不会走到这里，快路径不受影响。
    how = L" pcre";
    doc::RegexHits rh;
    std::wstring foldError;
    if (doc::LiteralSearchFile(snap->path, snap->enc, caseSensitive, needle, wholeWord,
                               cancel, &rh, &foldError)) {
      hits.offsets = std::move(rh.offsets);
      hits.total = rh.total;
      hits.truncated = rh.truncated;
      limited = rh.limited;
    } else {
      LogLine(L"DOCSRC: __search \"%s\" 折叠路径失败: %s", needle.c_str(), foldError.c_str());
    }
  } else if (wide) {
    how = L" w16";
    hits = SearchWideFile(snap->path, needle, caseSensitive,
                          snap->enc.encoding == doc::Encoding::Utf16BE, wholeWord, cancel);
  } else {
    const std::string bytes = snap->enc.encoding == doc::Encoding::Legacy
                                  ? WideToMbcs(needle, cp)
                                  : WideToUtf8String(needle);
    if (!bytes.empty()) {
      hits = doc::SearchFileBytes(snap->path, bytes, 4, nullptr, caseSensitive, wholeWord,
                                  cancel);
    }
  }

  if (cancel && cancel->load(std::memory_order_relaxed)) {
    LogLine(L"DOCSRC: __search \"%s\" 已取消 ms=%llu", needle.c_str(),
            static_cast<unsigned long long>(::GetTickCount64() - t0));
    return true;
  }

  const doc::BlockIndex& ix = snap->builder->index();
  constexpr size_t kMaxEntries = 2000;  // 导航与高亮的实际上限
  std::vector<SearchEntry> list;
  uint64_t visibleTotal = 0;
  for (uint64_t off : hits.offsets) {
    const uint32_t bi = ix.BlockAtOffset(off);
    if (bi == doc::BlockIndex::kInvalidBlock || bi >= ix.BlockCount()) continue;
    if (ix.IsHidden(bi)) continue;
    ++visibleTotal;
    if (list.size() < kMaxEntries) {
      SearchEntry e;
      e.off = off;
      e.block = bi;
      list.push_back(e);
    }
  }

  HANDLE h = ::CreateFileW(snap->path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    size_t i = 0;
    while (i < list.size()) {
      size_t j = i;
      while (j + 1 < list.size() && list[j + 1].block == list[i].block) ++j;
      FillBlockUnits(h, snap->enc, ix.Start(list[i].block), &list, i, j);
      // 巨型块要给出块内行号，前端才能跳到命中那一行；普通块用不上，省掉这次顺读。
      if (IsBigBlock(ix, list[i].block)) {
        FillBlockLines(h, snap->enc, ix.Start(list[i].block), &list, i, j);
      }
      i = j + 1;
    }
    ::CloseHandle(h);
  }

  const bool truncated = hits.truncated || visibleTotal > list.size();
  // 计数给源文本的精确命中数（含隐藏块）；导航仍只覆盖前面 list.size() 条。
  AppendU32(body, static_cast<uint32_t>(std::min<uint64_t>(hits.total, 0xFFFFFFFFu)));
  AppendU32(body, static_cast<uint32_t>(list.size()));
  AppendU32(body, (truncated ? 1u : 0u) | (limited ? 4u : 0u));
  for (const SearchEntry& e : list) {
    AppendU32(body, e.block);
    AppendU32(body, e.units);
    AppendU32(body, e.line);
  }

  LogLine(L"DOCSRC: __search%s%s \"%s\" hits=%llu visible=%llu added=%zu%s ms=%llu",
          how, regex ? L" re" : L"", needle.c_str(),
          static_cast<unsigned long long>(hits.total),
          static_cast<unsigned long long>(visibleTotal), list.size(),
          limited ? L" limited" : L"",
          static_cast<unsigned long long>(::GetTickCount64() - t0));
  return true;
}

bool ServeOutline(const std::wstring& uri, std::string* body, int* status) {
  if (!body || !status) return false;
  body->clear();
  *status = 404;

  static const wchar_t kBase[] = L"https://mdblocks.localhost/__outline";
  const size_t baseLen = sizeof(kBase) / sizeof(kBase[0]) - 1;
  if (uri.size() < baseLen || uri.compare(0, baseLen, kBase) != 0) return false;
  *status = 200;

  std::shared_ptr<const Snapshot> snap;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    snap = g_snap;
  }
  if (!snap) return true;

  constexpr size_t kMaxItems = 10000;
  const std::vector<doc::OutlineNode>& nodes = snap->builder->outline();
  const doc::BlockIndex& ix = snap->builder->index();

  AppendU32(body, static_cast<uint32_t>(std::min<size_t>(nodes.size(), 0xFFFFFFFFu)));
  AppendU32(body, 0);  // returned，最后回填
  AppendU32(body, 0);  // flags

  HANDLE h = ::CreateFileW(snap->path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  uint32_t added = 0;
  if (h != INVALID_HANDLE_VALUE) {
    std::vector<unsigned char> buf;
    for (const doc::OutlineNode& node : nodes) {
      if (added >= kMaxItems) break;
      const uint32_t bi = ix.BlockAtOffset(node.start);
      if (bi == doc::BlockIndex::kInvalidBlock || bi >= ix.BlockCount()) continue;
      if (ix.IsHidden(bi)) continue;
      const doc::Kind kind = ix.KindOf(bi);
      if (kind < doc::Kind::H1 || kind > doc::Kind::H6) continue;

      std::string text;
      if (node.textLen > 0) {
        buf.resize(node.textLen);
        size_t got = 0;
        if (ReadAt(h, node.start + node.textOff, node.textLen, &buf, &got) && got > 0) {
          text = ToUtf8(snap->enc,
                        std::string(reinterpret_cast<const char*>(buf.data()), got));
        }
      }
      if (text.size() > 0xFFFF) text.resize(0xFFFF);

      AppendU32(body, bi);
      body->push_back(static_cast<char>(node.level));
      AppendU16(body, static_cast<uint16_t>(text.size()));
      body->append(text);
      ++added;
    }
    ::CloseHandle(h);
  }
  PatchU32(body, 4, added);
  PatchU32(body, 8, nodes.size() > added ? 1u : 0u);
  LogLine(L"DOCSRC: __outline %zu 节点，返回 %u", nodes.size(), added);
  return true;
}

bool ServeDefs(const std::wstring& uri, std::string* body, int* status) {
  if (!body || !status) return false;
  body->clear();
  *status = 404;

  static const wchar_t kBase[] = L"https://mdblocks.localhost/__defs";
  const size_t baseLen = sizeof(kBase) / sizeof(kBase[0]) - 1;
  if (uri.size() < baseLen || uri.compare(0, baseLen, kBase) != 0) return false;

  std::shared_ptr<const Snapshot> snap;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    snap = g_snap;
  }
  *status = 200;
  if (!snap) return true;

  const doc::BlockIndex& ix = snap->builder->index();
  const uint32_t total = ix.BlockCount();
  constexpr uint64_t kMaxDefsBytes = 8ull << 20;
  constexpr uint32_t kMaxDefBlocks = 20000;

  HANDLE h = ::CreateFileW(snap->path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    LogLine(L"DOCSRC: __defs 打开失败 err=%lu", ::GetLastError());
    *status = 404;
    return true;
  }

  std::vector<unsigned char> buf(64 * 1024);
  uint32_t blocks = 0;
  for (uint32_t i = 0; i < total && body->size() < kMaxDefsBytes && blocks < kMaxDefBlocks; ++i) {
    const uint8_t flags = ix.KindFlags(i);
    if ((flags & doc::kFlagHidden) == 0) continue;
    if (ix.KindOf(i) == doc::Kind::FrontMatter) continue;

    const uint64_t bs = ix.Start(i);
    const uint64_t be = ix.End(i);
    if (be <= bs) continue;

    const uint64_t want = std::min<uint64_t>(be - bs, kMaxDefsBytes - body->size());
    if (want == 0) break;
    if (want > buf.size()) continue;  // 单个定义超过 64 KB：跳过（极不常见）
    size_t got = 0;
    if (!ReadAt(h, bs, static_cast<size_t>(want), &buf, &got)) break;
    const std::string utf8 = ToUtf8(snap->enc,
                                    std::string(reinterpret_cast<const char*>(buf.data()), got));
    AppendU32(body, static_cast<uint32_t>(flags));
    AppendU32(body, static_cast<uint32_t>(utf8.size()));
    body->append(utf8);
    ++blocks;
  }
  ::CloseHandle(h);
  LogLine(L"DOCSRC: __defs %u 个隐藏块 / %zu B", blocks, body->size());
  return true;
}

bool Serve(const std::wstring& uri, std::string* body, int* status) {
  if (!body || !status) return false;
  body->clear();
  *status = 404;

  uint32_t from = 0;
  uint32_t count = 100;
  if (!ParseBlocksUri(uri, &from, &count)) return false;
  if (count == 0) count = 100;
  if (count > kMaxRequestBlocks) count = kMaxRequestBlocks;

  std::shared_ptr<const Snapshot> snap;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    snap = g_snap;
  }
  if (!snap) return true;

  const doc::BlockIndex& ix = snap->builder->index();
  const uint32_t total = ix.BlockCount();
  if (from >= total) {
    *status = 200;
    return true;
  }

  uint32_t last = std::min<uint32_t>(total, from + count) - 1;
  uint64_t start = ix.Start(from);
  uint64_t end = ix.End(last);
  while (last > from && end - start > kMaxRequestBytes) {
    --last;
    end = ix.End(last);
  }
  // 单个块就可能超过上限：只读前缀，帧上打 kFlagTruncated。
  const uint64_t want = std::min<uint64_t>(end - start, kMaxRequestBytes);

  HANDLE h = ::CreateFileW(snap->path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *status = 404;
    return true;
  }
  std::vector<unsigned char> buf(static_cast<size_t>(want));
  LARGE_INTEGER pos = {};
  pos.QuadPart = static_cast<LONGLONG>(start);
  DWORD got = 0;
  const BOOL seekOk = ::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
  const BOOL readOk = seekOk && ::ReadFile(h, buf.data(), static_cast<DWORD>(want), &got, nullptr);
  ::CloseHandle(h);
  if (!readOk || got == 0) {
    *status = 200;
    return true;
  }

  const uint64_t bufEnd = start + got;
  body->reserve(static_cast<size_t>(got) + 9 * static_cast<size_t>(last - from + 1));
  for (uint32_t i = from; i <= last; ++i) {
    const uint64_t bs = ix.Start(i);
    if (bs >= bufEnd) break;
    const uint64_t be = ix.End(i);
    const uint64_t ce = std::min<uint64_t>(be, bufEnd);
    if (ce <= bs) continue;
    const uint8_t flags =
        static_cast<uint8_t>(ix.KindFlags(i) | (be > bufEnd ? kFlagTruncated : 0));
    AppendFrame(body, i, flags, buf.data() + (bs - start), static_cast<size_t>(ce - bs),
                snap->enc);
  }
  *status = 200;
  return true;
}

}  // namespace docsrc
