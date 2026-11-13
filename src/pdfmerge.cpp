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
  int delta = 0;
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
std::string RewriteRefs(const std::string& s, int delta) {
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

std::string RewriteObject(const std::string& body, int delta) {
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
void SetRefInDict(std::string* body, const char* key, int num) {
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

void AppendRef(std::string* out, int num) {
  out->append(std::to_string(num));
  out->append(" 0 R ");
}

bool WriteAll(const std::wstring& path, const std::string& data, std::wstring* err) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *err = L"无法写入输出文件";
    return false;
  }
  size_t done = 0;
  while (done < data.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20));
    DWORD written = 0;
    if (!::WriteFile(h, data.data() + done, chunk, &written, nullptr) || written == 0) {
      ::CloseHandle(h);
      *err = L"写入过程中失败";
      return false;
    }
    done += written;
  }
  ::CloseHandle(h);
  return true;
}

}  // namespace

bool PdfMergeFiles(const std::vector<std::wstring>& parts, const std::wstring& outPath,
                   std::wstring* err) {
  if (parts.size() < 2) {
    if (err) *err = L"至少需要两份";
    return false;
  }

  std::vector<Part> ps(parts.size());
  for (size_t i = 0; i < parts.size(); ++i) {
    if (!UtilReadFileBytes(parts[i], kMaxPartBytes, &ps[i].bytes)) {
      if (err) *err = L"读不到输入文件";
      return false;
    }
    if (!ParsePart(&ps[i], err)) return false;
  }

  // 对象号分配：每份的 Size 覆盖 0..Size-1，第二份起整体平移
  int base = 0;
  int totalCount = 0;
  for (Part& p : ps) {
    p.delta = base;
    base += p.size - 1;
    totalCount += p.count;
  }
  const int totalMax = base;
  const int newPages = totalMax + 1;
  const int catalog = ps[0].root;  // 复用第一份的 Catalog（它的 delta 是 0）
  const int info = ps[0].info;

  std::string out;
  size_t estimate = 0;
  for (const Part& p : ps) estimate += p.bytes.size();
  out.reserve(estimate + 4096);
  out += "%PDF-1.4\n%\xd3\xeb\xe9\xe1\n";

  std::vector<long long> newOffsets(static_cast<size_t>(newPages) + 2, -1);

  for (size_t i = 0; i < ps.size(); ++i) {
    Part& p = ps[i];
    for (size_t k = 0; k < p.sorted.size(); ++k) {
      const int num = p.sorted[k].second;
      std::string body = ObjectBody(p, k);
      if (body.empty()) continue;
      body = RewriteObject(body, p.delta);
      // 第一份的 Catalog 要指向新的页树根；每份自己的页树根要挂到新根下
      if (i == 0 && num == p.root) SetRefInDict(&body, "/Pages", newPages);
      if (num == p.pages) SetRefInDict(&body, "/Parent", newPages);
      const int newNum = num + p.delta;
      if (newNum >= 0 && static_cast<size_t>(newNum) < newOffsets.size()) {
        newOffsets[static_cast<size_t>(newNum)] = static_cast<long long>(out.size());
      }
      out += body;
      if (out.empty() || out.back() != '\n') out += '\n';
    }
    // 这一份已经写完，早点释放，合并十几份时峰值内存能少一半
    p.bytes.clear();
    p.bytes.shrink_to_fit();
  }

  // 新的页树根：Kids 指向各份原来的页树根，Count 相加
  newOffsets[static_cast<size_t>(newPages)] = static_cast<long long>(out.size());
  out += std::to_string(newPages);
  out += " 0 obj\n<< /Type /Pages /Count ";
  out += std::to_string(totalCount);
  out += " /Kids [";
  for (const Part& p : ps) AppendRef(&out, p.pages + p.delta);
  out += "] >>\nendobj\n";

  // xref：对象号 0..newPages
  const long long xrefPos = static_cast<long long>(out.size());
  const int xrefCount = newPages + 1;
  out += "xref\n0 ";
  out += std::to_string(xrefCount);
  out += "\n";
  for (int n = 0; n < xrefCount; ++n) {
    const long long off = n == 0 ? -1 : newOffsets[static_cast<size_t>(n)];
    if (off < 0) {
      out += "0000000000 65535 f \n";
      continue;
    }
    char buf[32];
    ::_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%010lld 00000 n \n", off);
    out += buf;
  }
  out += "trailer\n<< /Size ";
  out += std::to_string(xrefCount);
  out += " /Root ";
  out += std::to_string(catalog);
  out += " 0 R";
  if (info > 0) {
    out += " /Info ";
    out += std::to_string(info);
    out += " 0 R";
  }
  out += " >>\nstartxref\n";
  out += std::to_string(xrefPos);
  out += "\n%%EOF\n";

  return WriteAll(outPath, out, err);
}
