// doc_search.cpp - 4 线程 Horspool。票 04 实测：1 GB 热缓存 127–215 ms。

#include "doc_search.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace doc {

namespace {

constexpr size_t kHitsCap = 10000;     // 票 04
constexpr size_t kChunk = 1u << 20;

struct RangeResult {
  std::vector<uint64_t> hits;
  uint64_t total = 0;
};

// 在 [begin, end) 里找起点 < end 的命中。缓冲区尾部保留 m-1 字节做跨块回看：
// 上一轮只报「整段都落在已读区间内」的命中，下一轮从 bufferEnd-m+1 重新开始，
// 因此既不漏跨块命中，也不重复计数。
void SearchRange(HANDLE h, uint64_t begin, uint64_t end, uint64_t fileSize,
                 const std::string& pat, RangeResult* out) {
  const size_t m = pat.size();
  if (m == 0 || begin >= end || m > fileSize) return;

  size_t shift[256];
  for (size_t i = 0; i < 256; ++i) shift[i] = m;
  for (size_t i = 0; i + 1 < m; ++i) {
    shift[static_cast<unsigned char>(pat[i])] = m - 1 - i;
  }
  const unsigned char last = static_cast<unsigned char>(pat[m - 1]);

  const uint64_t overlap = m - 1;
  const uint64_t readLimit = std::min<uint64_t>(fileSize, end + overlap);

  LARGE_INTEGER pos = {};
  pos.QuadPart = static_cast<LONGLONG>(begin);
  if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return;

  std::vector<unsigned char> buf(static_cast<size_t>(overlap) + kChunk);
  size_t carry = 0;
  uint64_t bufferStart = begin;

  for (;;) {
    const uint64_t toRead =
        std::min<uint64_t>(kChunk, readLimit > bufferStart ? readLimit - bufferStart : 0);
    if (toRead == 0 && carry == 0) break;

    DWORD got = 0;
    if (toRead > 0) {
      if (!::ReadFile(h, buf.data() + carry, static_cast<DWORD>(toRead), &got, nullptr)) break;
    }

    const size_t n = carry + got;
    if (n < m) {
      // 还没凑够一个模式长度：把已读内容整体留作 carry
      carry = n;
      if (got == 0) break;
      bufferStart += got;
      continue;
    }

    // Horspool
    size_t k = m - 1;
    while (k < n) {
      const unsigned char c = buf[k];
      if (c == last) {
        const uint64_t startAbs = bufferStart + k - (m - 1);
        if (startAbs < end && std::memcmp(buf.data() + k - (m - 1), pat.data(), m) == 0) {
          ++out->total;
          if (out->hits.size() < kHitsCap) out->hits.push_back(startAbs);
        }
        ++k;
      } else {
        k += shift[c];
      }
    }

    if (got == 0) break;

    // 下一个缓冲区从 bufferEnd - overlap 开始（= 本轮最后一处可能没判完的起点）
    const size_t keep = static_cast<size_t>(std::min<uint64_t>(overlap, n));
    if (keep > 0) std::memmove(buf.data(), buf.data() + (n - keep), keep);
    bufferStart = bufferStart + n - keep;
    carry = keep;
  }
}

}  // namespace

SearchHits SearchFileBytes(const std::wstring& path, const std::string& needle, int threads,
                           std::wstring* error) {
  if (error) error->clear();
  SearchHits result;
  if (needle.empty()) return result;

  HANDLE probe = ::CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (probe == INVALID_HANDLE_VALUE) {
    if (error) *error = L"打开文件失败";
    return result;
  }
  LARGE_INTEGER size = {};
  if (!::GetFileSizeEx(probe, &size)) {
    ::CloseHandle(probe);
    if (error) *error = L"读取文件大小失败";
    return result;
  }
  const uint64_t fileSize = static_cast<uint64_t>(size.QuadPart);
  if (fileSize == 0) {
    ::CloseHandle(probe);
    return result;
  }

  int nThreads = threads <= 0 ? 4 : threads;
  if (nThreads > 4) nThreads = 4;
  if (static_cast<uint64_t>(nThreads) > fileSize / (64 * 1024)) {
    nThreads = static_cast<int>(std::max<uint64_t>(1, fileSize / (64 * 1024)));
  }

  const uint64_t seg = (fileSize + static_cast<uint64_t>(nThreads) - 1) /
                       static_cast<uint64_t>(nThreads);
  std::vector<RangeResult> parts(static_cast<size_t>(nThreads));
  std::vector<std::thread> workers;

  for (int i = 0; i < nThreads; ++i) {
    const uint64_t begin = static_cast<uint64_t>(i) * seg;
    if (begin >= fileSize) break;
    const uint64_t end = std::min<uint64_t>(fileSize, begin + seg);
    workers.emplace_back([&, i, begin, end]() {
      HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      if (h == INVALID_HANDLE_VALUE) return;
      SearchRange(h, begin, end, fileSize, needle, &parts[static_cast<size_t>(i)]);
      ::CloseHandle(h);
    });
  }
  for (std::thread& t : workers) t.join();
  ::CloseHandle(probe);

  for (const RangeResult& r : parts) {
    result.total += r.total;
    result.offsets.insert(result.offsets.end(), r.hits.begin(), r.hits.end());
  }
  std::sort(result.offsets.begin(), result.offsets.end());
  if (result.offsets.size() > kHitsCap) result.offsets.resize(kHitsCap);
  result.truncated = result.total > result.offsets.size();
  return result;
}

}  // namespace doc
