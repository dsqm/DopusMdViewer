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
                 size_t n, doc::Encoding enc) {
  std::string utf8;
  switch (enc) {
    case doc::Encoding::Utf8:
      utf8.assign(reinterpret_cast<const char*>(p), n);
      break;
    case doc::Encoding::Gb18030:
      utf8 = DecodeMbcs(p, n, 54936);
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
  static const wchar_t kBase[] = L"https://mdblocks.local/__blocks";
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

// ---------------- 巨型块行窗口（票 05 第 4 节） ----------------

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
    pos += k;  // 本块没有（更多）换行：沿长行继续往前读
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
    if (reached) break;
  }
  if (line != from) return false;
  *outOff = pos;
  return true;
}

// 从 start（某行行首）读 count 行；got 含块尾没有换行的最后一行。
bool ReadLines(HANDLE h, LineCache* c, uint64_t start, uint32_t count, bool wide,
               bool bigEndian, std::string* raw, uint32_t* gotLines) {
  const size_t unit = wide ? 2 : 1;
  std::vector<unsigned char> buf(1u << 20);
  uint64_t pos = start;
  uint32_t got = 0;

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
        if (raw->size() + tailLen > kMaxWindowBytes) break;
        raw->append(reinterpret_cast<const char*>(buf.data() + lineStart), tailLen);
        ++got;
      }
      pos = c->blockEnd;
      break;
    }
    pos = chunkStart + i;
  }
  *gotLines = got;
  return true;
}

uint32_t QueryU32(const std::wstring& query, const wchar_t* key, uint32_t def) {
  const std::wstring k = std::wstring(key) + L"=";
  const size_t p = query.find(k);
  if (p == std::wstring::npos) return def;
  return static_cast<uint32_t>(_wtoi(query.c_str() + p + k.size()));
}

std::string ToUtf8(doc::Encoding enc, std::string raw) {
  switch (enc) {
    case doc::Encoding::Utf8:
      return raw;
    case doc::Encoding::Gb18030:
      return DecodeMbcs(reinterpret_cast<const unsigned char*>(raw.data()), raw.size(), 54936);
    case doc::Encoding::Utf16LE:
      return DecodeWide(reinterpret_cast<const unsigned char*>(raw.data()), raw.size(), false);
    case doc::Encoding::Utf16BE:
      return DecodeWide(reinterpret_cast<const unsigned char*>(raw.data()), raw.size(), true);
  }
  return raw;
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

bool Open(HWND owner, const std::wstring& path, DocumentInfo* info, std::wstring* error) {
  Close();

  const uint64_t fileSize = FileSizeOf(path);
  auto builder = std::unique_ptr<doc::IndexBuilder>(new doc::IndexBuilder());
  std::wstring err;
  if (!doc::ScanFile(path, kPrefixBytes, builder.get(), nullptr, nullptr, &err)) {
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
  LogLine(L"DOCSRC: 打开 \"%s\" prefix=%u est=%u 文件=%llu B 完整=%d", path.c_str(),
          prefixBlocks, estimated, static_cast<unsigned long long>(fileSize),
          complete ? 1 : 0);

  if (!complete) {
    std::thread([path, fileSize, docId, cancel]() {
      auto full = std::unique_ptr<doc::IndexBuilder>(new doc::IndexBuilder());
      std::wstring err2;
      if (!doc::ScanFile(path, 0, full.get(), nullptr, nullptr, &err2, cancel.get())) {
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

  static const wchar_t kAssetBase[] = L"https://mdblocks.local/__asset";
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

// 响应 https://mdblocks.local/__lines：巨型块的行窗口。
bool ServeLines(const std::wstring& uri, std::string* body, int* status) {
  if (!body || !status) return false;
  body->clear();
  *status = 404;

  static const wchar_t kBase[] = L"https://mdblocks.local/__lines";
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

  const bool wide = snap->enc.encoding == doc::Encoding::Utf16LE ||
                    snap->enc.encoding == doc::Encoding::Utf16BE;
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
    readOk = ReadLines(h, &c, startOff, want, wide, bigEndian, &raw, &got);
    if (readOk && got < want && c.totalLines == 0) {
      // 读到块尾：行数到此为止
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

  std::string utf8 = ToUtf8(snap->enc.encoding, std::move(raw));

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
  }
}

bool ServeDefs(const std::wstring& uri, std::string* body, int* status) {
  if (!body || !status) return false;
  body->clear();
  *status = 404;

  static const wchar_t kBase[] = L"https://mdblocks.local/__defs";
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
    const std::string utf8 = ToUtf8(snap->enc.encoding,
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
                snap->enc.encoding);
  }
  *status = 200;
  return true;
}

}  // namespace docsrc
