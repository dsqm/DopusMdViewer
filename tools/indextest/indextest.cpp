// indextest.cpp - M3 验收驱动：只调 doc_index / doc_search，不复制扫描逻辑。
//
// 用法：
//   indextest scanmulti <file>
//   indextest prefix <file> <MB> [reps]
//   indextest stats <file>
//   indextest outline <file> [max]
//   indextest dump <file> [max]
//   indextest search <file> <pattern>
//   indextest blockat <file> <offset>

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "doc_index.h"
#include "doc_search.h"

namespace {

double QpcMs(LONGLONG from, LONGLONG to) {
  static LONGLONG freq = 0;
  if (freq == 0) {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    freq = f.QuadPart ? f.QuadPart : 1;
  }
  return static_cast<double>(to - from) * 1000.0 / static_cast<double>(freq);
}

bool FileSizeOf(const std::wstring& path, uint64_t* size) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
  *size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  return true;
}

uint64_t FnvBlocks(const doc::BlockIndex& ix) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < ix.BlockCount(); ++i) {
    const uint64_t v[3] = {ix.Start(i), ix.Len(i), ix.KindFlags(i)};
    for (uint64_t x : v) {
      h ^= x;
      h *= 1099511628211ull;
    }
  }
  return h;
}

const char* KindName(uint8_t flags) {
  static const char* kNames[] = {"none", "para", "h1", "h2", "h3", "h4", "h5", "h6",
                                 "fence", "table", "list", "quote", "html", "hr", "fm"};
  const uint8_t k = static_cast<uint8_t>(flags & doc::kKindMask);
  return k <= 14 ? kNames[k] : "?";
}

// 按指定读块尺寸扫一遍；返回总耗时与扫描耗时（不含 Open）。
bool ScanWithChunk(const std::wstring& path, size_t chunkBytes, uint64_t limit,
                   doc::IndexBuilder* out, double* totalMs, double* scanMs,
                   double* prepMs, std::wstring* error) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (error) *error = L"open failed";
    return false;
  }

  LARGE_INTEGER size = {};
  if (!::GetFileSizeEx(h, &size) || size.QuadPart == 0) {
    ::CloseHandle(h);
    if (error) *error = L"empty file";
    return false;
  }
  const uint64_t fileSize = static_cast<uint64_t>(size.QuadPart);
  const uint64_t maxBytes = limit && limit < fileSize ? limit : fileSize;

  std::vector<unsigned char> buf(chunkBytes);
  uint64_t consumed = 0;
  bool begun = false;
  double idxMs = 0;
  double prep = 0;

  LARGE_INTEGER t0;
  LARGE_INTEGER t1;
  ::QueryPerformanceCounter(&t0);

  while (consumed < maxBytes) {
    const size_t want =
        static_cast<size_t>(std::min<uint64_t>(chunkBytes, maxBytes - consumed));
    DWORD got = 0;
    if (!::ReadFile(h, buf.data(), static_cast<DWORD>(want), &got, nullptr) || got == 0) {
      break;
    }

    LARGE_INTEGER r0;
    LARGE_INTEGER r1;
    ::QueryPerformanceCounter(&r0);

    if (!begun) {
      ::QueryPerformanceCounter(&r0);
      const doc::EncodingInfo enc = doc::DetectEncoding(buf.data(), got);
      if (!out->Begin(enc, fileSize, maxBytes)) {
        ::CloseHandle(h);
        if (error) *error = L"Begin failed (file > 4 GB?)";
        return false;
      }
      ::QueryPerformanceCounter(&r1);
      prep += QpcMs(r0.QuadPart, r1.QuadPart);
      begun = true;
    }
    out->Feed(buf.data(), got);

    ::QueryPerformanceCounter(&r1);
    idxMs += QpcMs(r0.QuadPart, r1.QuadPart);
    consumed += got;
  }

  out->Finish();
  ::QueryPerformanceCounter(&t1);
  ::CloseHandle(h);

  if (!begun) {
    if (error) *error = L"no data";
    return false;
  }
  if (totalMs) *totalMs = QpcMs(t0.QuadPart, t1.QuadPart);
  if (scanMs) *scanMs = idxMs;
  if (prepMs) *prepMs = prep;
  return true;
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

bool ReadRange(const std::wstring& path, uint64_t off, uint32_t len, std::string* out) {
  out->clear();
  if (len == 0) return true;
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER pos = {};
  pos.QuadPart = static_cast<LONGLONG>(off);
  if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
    ::CloseHandle(h);
    return false;
  }
  out->resize(len);
  DWORD got = 0;
  const BOOL ok = ::ReadFile(h, &(*out)[0], len, &got, nullptr);
  ::CloseHandle(h);
  out->resize(got);
  return ok != FALSE;
}

int ModeScanMulti(const std::wstring& path) {
  const size_t kChunks[] = {4, 16, 64, 256, 1024, 4096};
  for (size_t kb : kChunks) {
    doc::IndexBuilder b;
    double total = 0;
    double scan = 0;
    std::wstring err;
    if (!ScanWithChunk(path, kb * 1024, 0, &b, &total, &scan, nullptr, &err)) {
      wprintf(L"chunk %4zu KB: FAILED %s\n", kb, err.c_str());
      return 1;
    }
    const doc::BlockIndex& ix = b.index();
    wprintf(L"chunk %4zu KB: blocks=%u hash=%016llx total=%7.1f ms scan=%7.1f ms\n", kb,
            ix.BlockCount(), static_cast<unsigned long long>(FnvBlocks(ix)), total, scan);
  }
  return 0;
}

int ModePrefix(const std::wstring& path, uint64_t mb, int reps) {
  uint64_t size = 0;
  if (!FileSizeOf(path, &size)) {
    wprintf(L"cannot stat file\n");
    return 1;
  }
  const uint64_t limit = std::min<uint64_t>(mb << 20, size);
  double best = 1e18;
  double bestScan = 1e18;
  double bestPrep = 1e18;
  uint32_t blocks = 0;
  int ok = 0;
  for (int i = 0; i < reps; ++i) {
    doc::IndexBuilder b;
    double total = 0;
    double scan = 0;
    double prep = 0;
    std::wstring err;
    if (!ScanWithChunk(path, 1u << 20, limit, &b, &total, &scan, &prep, &err)) {
      wprintf(L"prefix scan failed: %s\n", err.c_str());
      return 1;
    }
    best = std::min(best, total);
    bestScan = std::min(bestScan, scan);
    bestPrep = std::min(bestPrep, prep);
    blocks = b.index().BlockCount();
    ++ok;
  }
  wprintf(L"prefix %llu MB (file %llu MB): best total %.2f ms (prep %.2f + feed %.2f) blocks=%u runs=%d\n",
          static_cast<unsigned long long>(limit >> 20),
          static_cast<unsigned long long>(size >> 20), best, bestPrep, bestScan, blocks, ok);
  return 0;
}

int ModeStats(const std::wstring& path) {
  doc::IndexBuilder b;
  double total = 0;
  double scan = 0;
  std::wstring err;
  if (!ScanWithChunk(path, 1u << 20, 0, &b, &total, &scan, nullptr, &err)) {
    wprintf(L"scan failed: %s\n", err.c_str());
    return 1;
  }
  const doc::BlockIndex& ix = b.index();
  uint64_t hist[15] = {0};
  uint64_t loose = 0;
  uint64_t hidden = 0;
  uint64_t fndef = 0;
  for (uint32_t i = 0; i < ix.BlockCount(); ++i) {
    const uint8_t f = ix.KindFlags(i);
    ++hist[f & doc::kKindMask];
    if (f & doc::kFlagLoose) ++loose;
    if (f & doc::kFlagHidden) ++hidden;
    if (f & doc::kFlagFootnoteDef) ++fndef;
  }
  wprintf(L"blocks=%u bytes=%llu total=%.1f ms scan=%.1f ms index=%.2f MB outline=%zu\n",
          ix.BlockCount(), static_cast<unsigned long long>(b.BytesScanned()), total, scan,
          static_cast<double>(ix.MemoryBytes()) / 1048576.0, b.outline().size());
  wprintf(L"kinds:");
  for (int k = 1; k <= 14; ++k) {
    if (hist[k]) wprintf(L" %hs=%llu", KindName(static_cast<uint8_t>(k)),
                         static_cast<unsigned long long>(hist[k]));
  }
  wprintf(L"\nflags: loose=%llu hidden=%llu footnotedef=%llu enc=%d hash=%016llx\n", loose,
          hidden, fndef, static_cast<int>(b.encoding().encoding),
          static_cast<unsigned long long>(FnvBlocks(ix)));
  return 0;
}

int ModeOutline(const std::wstring& path, int maxPrint) {
  doc::IndexBuilder b;
  double total = 0;
  double scan = 0;
  std::wstring err;
  if (!ScanWithChunk(path, 1u << 20, 0, &b, &total, &scan, nullptr, &err)) {
    wprintf(L"scan failed: %s\n", err.c_str());
    return 1;
  }
  const std::vector<doc::OutlineNode>& nodes = b.outline();
  wprintf(L"outline nodes=%zu (showing %d)\n", nodes.size(),
          std::min<int>(static_cast<int>(nodes.size()), maxPrint));
  const int n = std::min<int>(static_cast<int>(nodes.size()), maxPrint);
  for (int i = 0; i < n; ++i) {
    const doc::OutlineNode& node = nodes[static_cast<size_t>(i)];
    std::string text;
    ReadRange(path, node.start + node.textOff, node.textLen, &text);
    wprintf(L"h%u block@%llu off=%u len=%u text=\"%hs\"\n", node.level,
            static_cast<unsigned long long>(node.start), node.textOff, node.textLen,
            text.c_str());
  }
  return 0;
}

int ModeDump(const std::wstring& path, int maxPrint) {
  doc::IndexBuilder b;
  double total = 0;
  double scan = 0;
  std::wstring err;
  if (!ScanWithChunk(path, 1u << 20, 0, &b, &total, &scan, nullptr, &err)) {
    wprintf(L"scan failed: %s\n", err.c_str());
    return 1;
  }
  const doc::BlockIndex& ix = b.index();
  const int n = std::min<int>(static_cast<int>(ix.BlockCount()), maxPrint);
  for (int i = 0; i < n; ++i) {
    std::string head;
    const uint32_t iu = static_cast<uint32_t>(i);
    ReadRange(path, ix.Start(iu), std::min<uint32_t>(ix.Len(iu), 60), &head);
    std::wstring whead = Utf8ToWide(head);
    for (wchar_t& c : whead) {
      if (c == L'\r' || c == L'\n') c = L' ';
    }
    wprintf(L"#%d %hs flags=0x%02X start=%llu len=%u \"%s\"\n", i,
            KindName(ix.KindFlags(iu)), ix.KindFlags(iu),
            static_cast<unsigned long long>(ix.Start(iu)), ix.Len(iu), whead.c_str());
  }
  return 0;
}

int ModeBlockAt(const std::wstring& path, uint64_t off) {
  doc::IndexBuilder b;
  double total = 0;
  double scan = 0;
  std::wstring err;
  if (!ScanWithChunk(path, 1u << 20, 0, &b, &total, &scan, nullptr, &err)) {
    wprintf(L"scan failed: %s\n", err.c_str());
    return 1;
  }
  const doc::BlockIndex& ix = b.index();
  const uint32_t k = ix.BlockAtOffset(off);
  if (k == doc::BlockIndex::kInvalidBlock) {
    wprintf(L"offset %llu -> no block\n", static_cast<unsigned long long>(off));
    return 1;
  }
  wprintf(L"offset %llu -> block #%u start=%llu len=%u kind=%hs\n",
          static_cast<unsigned long long>(off), k,
          static_cast<unsigned long long>(ix.Start(k)), ix.Len(k), KindName(ix.KindFlags(k)));
  return 0;
}

int ModeSearch(const std::wstring& path, const std::string& pattern, bool caseSensitive,
               bool wholeWord) {
  LARGE_INTEGER t0;
  LARGE_INTEGER t1;
  ::QueryPerformanceCounter(&t0);
  std::wstring err;
  const doc::SearchHits hits =
      doc::SearchFileBytes(path, pattern, 4, &err, caseSensitive, wholeWord);
  ::QueryPerformanceCounter(&t1);
  wprintf(L"search \"%hs\" cs=%d ww=%d: total=%llu kept=%zu truncated=%d %.1f ms %s\n",
          pattern.c_str(), caseSensitive ? 1 : 0, wholeWord ? 1 : 0,
          static_cast<unsigned long long>(hits.total), hits.offsets.size(),
          hits.truncated ? 1 : 0, QpcMs(t0.QuadPart, t1.QuadPart), err.c_str());
  for (size_t i = 0; i < std::min<size_t>(hits.offsets.size(), 40); ++i) {
    wprintf(L"  #%zu @%llu\n", i, static_cast<unsigned long long>(hits.offsets[i]));
  }
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    wprintf(L"usage: indextest scanmulti|prefix|stats|outline|dump|blockat|search ...\n");
    return 2;
  }
  const std::wstring mode = argv[1];
  const std::wstring path = argv[2];

  if (mode == L"scanmulti") return ModeScanMulti(path);
  if (mode == L"stats") return ModeStats(path);
  if (mode == L"outline") {
    const int maxPrint = argc >= 4 ? _wtoi(argv[3]) : 20;
    return ModeOutline(path, maxPrint);
  }
  if (mode == L"dump") {
    const int maxPrint = argc >= 4 ? _wtoi(argv[3]) : 40;
    return ModeDump(path, maxPrint);
  }
  if (mode == L"prefix") {
    const uint64_t mb = argc >= 4 ? static_cast<uint64_t>(_wtoi64(argv[3])) : 8;
    const int reps = argc >= 5 ? _wtoi(argv[4]) : 5;
    return ModePrefix(path, mb, reps);
  }
  if (mode == L"blockat") {
    const uint64_t off = argc >= 4 ? static_cast<uint64_t>(_wtoi64(argv[3])) : 0;
    return ModeBlockAt(path, off);
  }
  if (mode == L"search") {
    if (argc < 4) {
      wprintf(L"search needs a pattern\n");
      return 2;
    }
    std::string pattern;
    const std::wstring wp = argv[3];
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), -1, nullptr, 0, nullptr, nullptr);
    pattern.resize(static_cast<size_t>(n > 0 ? n - 1 : 0));
    if (n > 1) {
      ::WideCharToMultiByte(CP_UTF8, 0, wp.c_str(), -1, &pattern[0], n, nullptr, nullptr);
    }
    const std::wstring flags = argc >= 5 ? argv[4] : L"";
    return ModeSearch(path, pattern, flags.find(L"cs") != std::wstring::npos,
                      flags.find(L"ww") != std::wstring::npos);
  }

  wprintf(L"unknown mode %s\n", mode.c_str());
  return 2;
}
