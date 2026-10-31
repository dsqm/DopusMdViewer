// doc_source.cpp - 当前文档索引的生命周期与块正文服务。

#include "doc_source.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "doc_index.h"
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
  uint64_t fileSize = 0;
  doc::EncodingInfo enc;
};

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

std::shared_ptr<const Snapshot> MakeSnapshot(std::unique_ptr<doc::IndexBuilder> builder,
                                             const std::wstring& path, uint64_t fileSize) {
  auto snap = std::make_shared<Snapshot>();
  snap->builder = std::move(builder);
  snap->path = path;
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
