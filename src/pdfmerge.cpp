// pdfmerge.cpp - 见 pdfmerge.h。
//
// 只支持 Chromium 打印出来的那种结构：PDF 1.4 + 传统 xref 表 + 不带对象流。
// 合并的做法是给每份的对象号加一个偏移量，再重建一份统一的 xref；跨份的引用
// 不需要去重——各份的字体、图片对象本就独立，重编号后互不干扰。

#include "pdfmerge.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "util.h"

namespace {

constexpr size_t kMaxPartBytes = 512ull * 1024 * 1024;
constexpr long long kMaxObjects = 4000000;  // 单份对象数上限，防御性

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

long long ReadInt(const std::string& s, size_t* pos) {
  size_t i = *pos;
  while (i < s.size() && IsSpace(s[i])) ++i;
  if (i >= s.size() || !IsDigit(s[i])) return -1;
  long long v = 0;
  while (i < s.size() && IsDigit(s[i])) {
    if (v < (1ll << 40)) v = v * 10 + (s[i] - '0');
    ++i;
  }
  *pos = i;
  return v;
}

long long ReadFixed(const std::string& s, size_t pos, int width) {
  long long v = 0;
  for (int i = 0; i < width && pos + static_cast<size_t>(i) < s.size(); ++i) {
    const char c = s[pos + static_cast<size_t>(i)];
    if (!IsDigit(c)) break;
    v = v * 10 + (c - '0');
  }
  return v;
}

// key 之后的第一个整数。对 /Size 是数值本身，对 /Root 是引用里的对象号。
long long NumberAfter(const std::string& s, const char* key) {
  const size_t pos = s.find(key);
  if (pos == std::string::npos) return -1;
  size_t i = pos + std::strlen(key);
  return ReadInt(s, &i);
}

struct Part {
  std::string bytes;
  std::vector<long long> offsets;                 // 对象号 → 文件偏移（-1 = 空闲）
  std::vector<std::pair<long long, int>> sorted;  // (偏移, 对象号) 升序
  long long xrefPos = 0;
  size_t trailerPos = 0;
  size_t trailerEnd = 0;
  int size = 0;
  int root = 0;
  int pages = 0;
  int count = 0;
  int info = 0;
  long long delta = 0;  // 累计对象号偏移，可能超过 int（上百份时）
};

bool ParsePart(Part* p, std::wstring* err) {
  const std::string& b = p->bytes;
  const size_t sp = b.rfind("startxref");
  if (sp == std::string::npos) {
    *err = L"缺少 startxref";
    return false;
  }
  size_t i = sp + 9;
  const long long xrefPos = ReadInt(b, &i);
  if (xrefPos <= 0 || static_cast<size_t>(xrefPos) + 4 > b.size()) {
    *err = L"startxref 偏移无效";
    return false;
  }
  p->xrefPos = xrefPos;

  size_t q = static_cast<size_t>(xrefPos);
  if (b.compare(q, 4, "xref") != 0) {
    *err = L"不是传统 xref 表（可能是对象流）";
    return false;
  }
  q += 4;

  for (;;) {
    while (q < b.size() && IsSpace(b[q])) ++q;
    if (q + 7 <= b.size() && b.compare(q, 7, "trailer") == 0) break;
    const long long start = ReadInt(b, &q);
    if (start < 0) {
      *err = L"xref 子段头缺失";
      return false;
    }
    const long long count = ReadInt(b, &q);
    if (count <= 0 || count > kMaxObjects) {
      *err = L"xref 子段条目数异常";
      return false;
    }
    while (q < b.size() && b[q] != '\n') ++q;
    if (q < b.size()) ++q;
    for (long long k = 0; k < count; ++k) {
      const size_t lineEnd = b.find('\n', q);
      if (lineEnd == std::string::npos || lineEnd - q < 18) {
        *err = L"xref 条目被截断";
        return false;
      }
      const long long off = ReadFixed(b, q, 10);
      const char type = b[q + 17];
      const int num = static_cast<int>(start + k);
      if (type == 'n' && num >= 0) {
        if (p->offsets.size() <= static_cast<size_t>(num)) {
          p->offsets.resize(static_cast<size_t>(num) + 1, -1);
        }
        p->offsets[static_cast<size_t>(num)] = off;
      }
      q = lineEnd + 1;
    }
  }

  // trailer 字典：逐层数 << >> 找闭合位置
  q += 7;
  const size_t dictStart = b.find("<<", q);
  if (dictStart == std::string::npos) {
    *err = L"trailer 字典缺失";
    return false;
  }
  int depth = 0;
  size_t dictEnd = std::string::npos;
  for (size_t t = dictStart; t + 1 < b.size(); ++t) {
    if (b[t] == '<' && b[t + 1] == '<') {
      ++depth;
      ++t;
    } else if (b[t] == '>' && b[t + 1] == '>') {
      --depth;
      ++t;
      if (depth == 0) {
        dictEnd = t + 1;
        break;
      }
    }
  }
  if (dictEnd == std::string::npos) {
    *err = L"trailer 字典未闭合";
    return false;
  }
  p->trailerPos = dictStart;
  p->trailerEnd = dictEnd;
  const std::string dict = b.substr(dictStart, dictEnd - dictStart);

  p->size = static_cast<int>(NumberAfter(dict, "/Size"));
  p->root = static_cast<int>(NumberAfter(dict, "/Root"));
  const long long info = NumberAfter(dict, "/Info");
  p->info = info > 0 ? static_cast<int>(info) : 0;
  if (p->size <= 0 || p->root <= 0) {
    *err = L"trailer 缺少 Size/Root";
    return false;
  }

  p->sorted.reserve(p->offsets.size());
  for (size_t n = 0; n < p->offsets.size(); ++n) {
    if (p->offsets[n] >= 0) {
      p->sorted.emplace_back(p->offsets[n], static_cast<int>(n));
    }
  }
  std::sort(p->sorted.begin(), p->sorted.end());

  // Catalog 里的页树根与页数
  const auto find = [&](int num) -> std::string {
    for (const auto& e : p->sorted) {
      if (e.second == num) {
        const size_t idx = static_cast<size_t>(&e - p->sorted.data());
        long long end = p->xrefPos;
        if (idx + 1 < p->sorted.size()) {
          const long long next = p->sorted[idx + 1].first;
          if (next < end) end = next;
        }
        if (e.first < 0 || end <= e.first) return std::string();
        return b.substr(static_cast<size_t>(e.first), static_cast<size_t>(end - e.first));
      }
    }
    return std::string();
  };

  const std::string rootBody = find(p->root);
  if (rootBody.empty()) {
    *err = L"找不到 Catalog 对象";
    return false;
  }
  p->pages = static_cast<int>(NumberAfter(rootBody, "/Pages"));
  if (p->pages <= 0) {
    *err = L"Catalog 缺少 /Pages";
    return false;
  }
  const std::string pagesBody = find(p->pages);
  if (pagesBody.empty()) {
    *err = L"找不到页树根对象";
    return false;
  }
  p->count = static_cast<int>(NumberAfter(pagesBody, "/Count"));
  if (p->count <= 0) {
    *err = L"页树缺少 /Count";
    return false;
  }
  return true;
}

std::string ObjectBody(const Part& p, size_t idx) {
  const long long off = p.sorted[idx].first;
  long long end = p.xrefPos;
  if (idx + 1 < p.sorted.size()) {
    const long long next = p.sorted[idx + 1].first;
    if (next < end) end = next;
  }
  if (off < 0 || end <= off) return std::string();
  return p.bytes.substr(static_cast<size_t>(off), static_cast<size_t>(end - off));
}

// 找真正的 stream 关键字：endstream 里也含 stream，要跳过
size_t FindStreamKeyword(const std::string& s) {
  size_t pos = 0;
  while ((pos = s.find("stream", pos)) != std::string::npos) {
    if (pos >= 3 && s.compare(pos - 3, 3, "end") == 0) {
      pos += 6;
      continue;
    }
    return pos;
  }
  return std::string::npos;
}

// 把「对象号 代次 R/obj」里的对象号加 delta。流数据整段跳过：那里的字节是
// 压缩内容，改写会毁掉图像或文字。
std::string RewriteRefs(const std::string& s, long long delta) {
  if (delta == 0) return s;
  std::string out;
  out.reserve(s.size() + s.size() / 64 + 16);
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (!IsDigit(c) || (i > 0 && IsDigit(s[i - 1]))) {
      out.push_back(c);
      ++i;
      continue;
    }
    size_t j = i;
    long long num = 0;
    while (j < s.size() && IsDigit(s[j])) {
      if (num < (1ll << 40)) num = num * 10 + (s[j] - '0');
      ++j;
    }
    size_t a = j;
    while (a < s.size() && IsSpace(s[a])) ++a;
    size_t g = a;
    while (g < s.size() && IsDigit(s[g])) ++g;
    size_t b = g;
    while (b < s.size() && IsSpace(s[b])) ++b;
    if (g > a && b < s.size() && (s[b] == 'R' || s[b] == 'o')) {
      out += std::to_string(num + delta);
      out.append(s, j, b - j);  // 空白 + 代次 + 空白，R/obj 交给下一轮
      i = b;
      continue;
    }
    out.append(s, i, j - i);
    i = j;
  }
  return out;
}

std::string RewriteObject(const std::string& body, long long delta) {
  const size_t sp = FindStreamKeyword(body);
  if (sp != std::string::npos) {
    size_t dataStart = sp + 6;
    if (body.compare(dataStart, 2, "\r\n") == 0) {
      dataStart += 2;
    } else if (dataStart < body.size() && (body[dataStart] == '\n' || body[dataStart] == '\r')) {
      dataStart += 1;
    }
    const size_t endStream = body.find("endstream", dataStart);
    if (endStream != std::string::npos && dataStart < endStream) {
      return RewriteRefs(body.substr(0, dataStart), delta) +
             body.substr(dataStart, endStream - dataStart) +
             RewriteRefs(body.substr(endStream), delta);
    }
  }
  return RewriteRefs(body, delta);
}

// 把字典里 key 引用的对象号改成 num；没有这个键就插在字典开头。
void SetRefInDict(std::string* body, const char* key, long long num) {
  const size_t pos = body->find(key);
  if (pos != std::string::npos) {
    size_t i = pos + std::strlen(key);
    while (i < body->size() && IsSpace((*body)[i])) ++i;
    const size_t numStart = i;
    while (i < body->size() && IsDigit((*body)[i])) ++i;
    size_t j = i;
    while (j < body->size() && IsSpace((*body)[j])) ++j;
    size_t g = j;
    while (g < body->size() && IsDigit((*body)[g])) ++g;
    size_t r = g;
    while (r < body->size() && IsSpace((*body)[r])) ++r;
    if (numStart < i && r < body->size() && (*body)[r] == 'R') {
      body->replace(numStart, i - numStart, std::to_string(num));
    }
    return;
  }
  const size_t dictPos = body->find("<<");
  if (dictPos == std::string::npos) return;
  body->insert(dictPos + 2, std::string(" ") + key + " " + std::to_string(num) + " 0 R");
}

void AppendRef(std::string* out, long long num) {
  out->append(std::to_string(num));
  out->append(" 0 R ");
}

// 边写边记运行位置：xref 里要的是「对象写入前」的绝对文件偏移。
// 不再把整份 PDF 攒成一个 std::string，峰值内存就只跟单份 part 有关。
bool WriteBytes(HANDLE h, const char* data, size_t len, long long* pos, std::wstring* err) {
  while (len > 0) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(len, 1u << 20));
    DWORD written = 0;
    if (!::WriteFile(h, data, chunk, &written, nullptr) || written != chunk) {
      *err = L"写入过程中失败";
      return false;
    }
    data += written;
    len -= written;
    *pos += written;
  }
  return true;
}

bool WriteStr(HANDLE h, const std::string& s, long long* pos, std::wstring* err) {
  return WriteBytes(h, s.data(), s.size(), pos, err);
}

bool WriteCStr(HANDLE h, const char* s, long long* pos, std::wstring* err) {
  return WriteBytes(h, s, std::strlen(s), pos, err);
}

struct PartMeta {
  int root = 0;
  int pages = 0;
  int count = 0;
  int info = 0;
  long long delta = 0;
};

}  // namespace

bool PdfMergeFiles(const std::vector<std::wstring>& parts, const std::wstring& outPath,
                   std::wstring* err) {
  std::wstring localErr;
  if (!err) err = &localErr;
  if (parts.size() < 2) {
    *err = L"至少需要两份";
    return false;
  }

  // 第一遍只留元信息：第一份 Catalog 的 /Pages 要指向新页树根，而新根编号取决于
  // 全部份的对象总数。先算出来才能动笔，否则要回头改已写出的对象。
  std::vector<PartMeta> meta(parts.size());
  long long base = 0;
  long long totalCount = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    Part p;
    if (!UtilReadFileBytes(parts[i], kMaxPartBytes, &p.bytes)) {
      *err = L"读不到输入文件";
      return false;
    }
    if (!ParsePart(&p, err)) return false;
    meta[i].root = p.root;
    meta[i].pages = p.pages;
    meta[i].count = p.count;
    meta[i].info = p.info;
    meta[i].delta = base;
    // 对象 0 恒为空闲，每份实际写出的对象是 1..Size-1
    base += static_cast<long long>(p.size) - 1;
    totalCount += p.count;
  }
  const long long newPages = base + 1;
  const long long catalog = meta[0].root;  // 复用第一份的 Catalog（delta 为 0）
  const long long info = meta[0].info;
  std::vector<long long> newOffsets(static_cast<size_t>(newPages) + 2, -1);

  const std::wstring tmp = outPath + L".tmp";
  HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *err = L"无法写入输出文件";
    return false;
  }

  bool ok = true;
  long long pos = 0;
  ok = WriteCStr(h, "%PDF-1.4\n%\xd3\xeb\xe9\xe1\n", &pos, err);

  // 第二遍：逐份读入、逐对象写出，内存里始终只有一份 part
  for (size_t i = 0; ok && i < parts.size(); ++i) {
    Part p;
    if (!UtilReadFileBytes(parts[i], kMaxPartBytes, &p.bytes)) {
      *err = L"读不到输入文件";
      ok = false;
      break;
    }
    if (!ParsePart(&p, err)) {
      ok = false;
      break;
    }
    p.delta = meta[i].delta;
    for (size_t k = 0; k < p.sorted.size(); ++k) {
      const int num = p.sorted[k].second;
      std::string body = ObjectBody(p, k);
      if (body.empty()) continue;
      body = RewriteObject(body, p.delta);
      // 第一份的 Catalog 要指向新的页树根；每份自己的页树根要挂到新根下
      if (i == 0 && num == p.root) SetRefInDict(&body, "/Pages", newPages);
      if (num == p.pages) SetRefInDict(&body, "/Parent", newPages);
      const long long newNum = static_cast<long long>(num) + p.delta;
      if (newNum >= 0 && static_cast<size_t>(newNum) < newOffsets.size()) {
        newOffsets[static_cast<size_t>(newNum)] = pos;
      }
      if (!WriteStr(h, body, &pos, err)) {
        ok = false;
        break;
      }
      if (body.back() != '\n' && !WriteCStr(h, "\n", &pos, err)) {
        ok = false;
        break;
      }
    }
  }

  // 新的页树根：Kids 指向各份原来的页树根，Count 相加
  if (ok) {
    newOffsets[static_cast<size_t>(newPages)] = pos;
    std::string root = std::to_string(newPages);
    root += " 0 obj\n<< /Type /Pages /Count ";
    root += std::to_string(totalCount);
    root += " /Kids [";
    for (const PartMeta& m : meta) {
      AppendRef(&root, static_cast<long long>(m.pages) + m.delta);
    }
    root += "] >>\nendobj\n";
    ok = WriteStr(h, root, &pos, err);
  }

  // xref：每行恰好 20 字节、偏移 10 位定宽。分块生成，不再攒一份与 PDF 等大的串。
  const long long xrefPos = pos;
  const long long xrefCount = newPages + 1;
  if (ok) {
    const std::string head = "xref\n0 " + std::to_string(xrefCount) + "\n";
    ok = WriteStr(h, head, &pos, err);
  }
  if (ok) {
    std::string buf;
    buf.reserve(64 * 1024);
    char line[32];
    for (long long n = 0; n < xrefCount; ++n) {
      const long long off = n == 0 ? -1 : newOffsets[static_cast<size_t>(n)];
      if (off > 9999999999LL) {
        *err = L"PDF 超过 10 GB，xref 偏移写不下";
        ok = false;
        break;
      }
      if (off < 0) {
        buf += "0000000000 65535 f \n";
      } else {
        ::_snprintf_s(line, sizeof(line), _TRUNCATE, "%010lld 00000 n \n", off);
        buf += line;
      }
      if (buf.size() >= 32 * 1024) {
        if (!WriteStr(h, buf, &pos, err)) {
          ok = false;
          break;
        }
        buf.clear();
      }
    }
    if (ok && !buf.empty()) ok = WriteStr(h, buf, &pos, err);
  }
  if (ok) {
    std::string tail = "trailer\n<< /Size " + std::to_string(xrefCount) +
                       " /Root " + std::to_string(catalog) + " 0 R";
    if (info > 0) tail += " /Info " + std::to_string(info) + " 0 R";
    tail += " >>\nstartxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";
    ok = WriteStr(h, tail, &pos, err);
  }

  ::CloseHandle(h);
  if (!ok) {
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  // 先落 .tmp 再替换：中途失败不会留下半个 PDF，分份文件也还在磁盘上可补救
  if (!::MoveFileExW(tmp.c_str(), outPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    *err = L"无法替换输出文件";
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}
