// doc_regex8.cpp - UTF-8 源文件上的 PCRE2 8 位扫描。
//
// 分块（4 MB + 64 KB 前瞻，行对齐），逐块做全局匹配。UTF 校验只付一次：第一次
// pcre2_match 校验整块，之后都带 PCRE2_NO_UTF_CHECK——不这样做，逐次匹配会把
// 整段重新校验，1 GB 上会从毫秒级退化到分钟级。

#define PCRE2_CODE_UNIT_WIDTH 8
#include "pcre2.h"

#include "doc_regex_common.h"
#include "doc_regex_internal.h"

namespace doc {
namespace {

using namespace regex_detail;

std::wstring CompileMessage(int code, PCRE2_SIZE offset) {
  unsigned char buf[256];
  const int n = pcre2_get_error_message(code, buf, sizeof(buf));
  std::wstring msg = L"正则无效";
  if (n > 0) {
    msg += L"：";
    for (int i = 0; i < n; ++i) msg += static_cast<wchar_t>(buf[i]);
  } else {
    msg += L"（错误码 ";
    msg += std::to_wstring(code);
    msg += L"）";
  }
  if (offset != PCRE2_UNSET) {
    msg += L"（位置 ";
    msg += std::to_wstring(static_cast<unsigned long long>(offset));
    msg += L"）";
  }
  return msg;
}

bool IsResourceLimit(int rc) {
  return rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT ||
         rc == PCRE2_ERROR_HEAPLIMIT || rc == PCRE2_ERROR_JIT_STACKLIMIT;
}

}  // namespace

bool RegexScanUtf8(const std::wstring& path, uint64_t startOffset, const std::string& pattern,
                   bool caseless,
                   const std::atomic<bool>* cancel, RegexHits* out, std::wstring* error) {
  HANDLE h = INVALID_HANDLE_VALUE;
  uint64_t fileSize = 0;
  if (!OpenForScan(path, &h, &fileSize, error)) return false;

  const uint32_t opts =
      PCRE2_UTF | PCRE2_UCP | PCRE2_MULTILINE | (caseless ? PCRE2_CASELESS : 0);
  int err = 0;
  PCRE2_SIZE erroff = 0;
  pcre2_code* re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                                 static_cast<PCRE2_SIZE>(pattern.size()), opts, &err,
                                 &erroff, nullptr);
  if (!re) {
    if (error) *error = CompileMessage(err, erroff);
    ::CloseHandle(h);
    return false;
  }
  pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
  pcre2_match_context* mctx = pcre2_match_context_create(nullptr);
  if (mctx) {
    pcre2_set_match_limit(mctx, 10000000);
    pcre2_set_depth_limit(mctx, 1000);
  }
  pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);

  std::vector<unsigned char> win;
  uint64_t chunkStart = std::min<uint64_t>(startOffset, fileSize);
  bool lineStart = true;  // 文件开头算行首
  bool ok = true;

  while (chunkStart < fileSize) {
    if (cancel && cancel->load(std::memory_order_relaxed)) break;

    size_t got = 0;
    const uint64_t remain = fileSize - chunkStart;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(kWindowBytes, remain));
    if (!ReadWindow(h, chunkStart, want, &win, &got)) break;
    if (got == 0) break;
    const unsigned char* p = win.data();

    const uint64_t rawEnd = std::min<uint64_t>(chunkStart + kChunkBytes, fileSize);
    uint64_t chunkEnd = rawEnd;
    bool chunkLineStart = true;
    if (rawEnd < fileSize) {
      const LineCut cut = FindLineEnd8(p, got, rawEnd - chunkStart, kLineSlack);
      chunkEnd = chunkStart + cut.pos;
      chunkLineStart = cut.atLineStart;
    }
    if (chunkEnd > chunkStart + got) {
      chunkEnd = chunkStart + got;
      chunkLineStart = false;
    }
    if (chunkEnd <= chunkStart) chunkEnd = chunkStart + got;

    uint64_t subjectEnd = fileSize;
    if (chunkEnd < fileSize) {
      const uint64_t target = std::min<uint64_t>(chunkEnd + kLookahead, fileSize);
      if (target < fileSize) {
        const LineCut cut = FindLineEnd8(p, got, target - chunkStart, kLineSlack);
        subjectEnd = chunkStart + cut.pos;
      }
    }
    if (subjectEnd > chunkStart + got) subjectEnd = chunkStart + got;
    if (subjectEnd < chunkEnd) subjectEnd = chunkEnd;

    const size_t subjectLen = static_cast<size_t>(subjectEnd - chunkStart);
    const size_t chunkRel = static_cast<size_t>(chunkEnd - chunkStart);
    uint32_t flags = 0;
    if (!lineStart) flags |= PCRE2_NOTBOL;
    if (subjectEnd < fileSize) flags |= PCRE2_NOTEOL;

    PCRE2_SIZE start = 0;
    bool validated = false;
    bool canceled = false;
    while (start < chunkRel) {
      if ((out->total & 0xFFF) == 0 && cancel && cancel->load(std::memory_order_relaxed)) {
        canceled = true;
        break;
      }
      const uint32_t callFlags = flags | (validated ? PCRE2_NO_UTF_CHECK : 0);
      const int rc = pcre2_match(re, p, subjectLen, start, callFlags, md, mctx);
      if (rc == PCRE2_ERROR_NOMATCH) break;
      if (rc < 0) {
        if (IsResourceLimit(rc)) {
          out->limited = true;
        } else if (!validated && error) {
          *error = L"文件含无效的 UTF-8 序列（偏移 " +
                   std::to_wstring(static_cast<unsigned long long>(chunkStart + start)) + L"）";
          ok = false;
        }
        break;
      }
      validated = true;
      const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
      const PCRE2_SIZE s = ov[0];
      if (s >= chunkRel) break;  // 起点在下一块，交给下一块统计
      ++out->total;
      if (out->offsets.size() < kMaxStored) out->offsets.push_back(chunkStart + s);
      if (ov[1] > s) {
        start = ov[1];
      } else {
        if (s >= subjectLen) break;
        start = s + Utf8CharLen(p[s]);
      }
    }
    if (!ok || canceled) break;
    if (cancel && cancel->load(std::memory_order_relaxed)) break;

    lineStart = chunkLineStart;
    chunkStart = chunkEnd;
  }

  if (md) pcre2_match_data_free(md);
  if (mctx) pcre2_match_context_free(mctx);
  pcre2_code_free(re);
  ::CloseHandle(h);
  out->truncated = out->total > out->offsets.size() || out->limited;
  return ok;
}

}  // namespace doc
