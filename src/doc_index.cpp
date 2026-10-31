// doc_index.cpp - 块扫描状态机与 4 KB 页相对索引。
//
// 扫描器与字符类型无关：同一套规则实例化在 char（UTF-8 / GB18030）与
// wchar_t（UTF-16）上，只有喂数据的方式不同。票 04 实测：0x0A 不会出现在
// UTF-8 / GB18030 的多字节序列里，所以字节路径不需要先判定编码。

#include "doc_index.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace doc {

namespace {

constexpr uint32_t kPrefix = 256;                  // 每行保留的有界前缀
constexpr uint64_t kMaxHtmlBytes = 4ull << 20;     // 票 05 6.1：HTML 区上限
constexpr uint64_t kMaxFileBytes = 4ull << 30;     // 票 04：按 4 GB 分段
constexpr uint64_t kReadChunk = 1ull << 20;

template <class Char>
inline bool IsSpace(Char c) {
  return c == static_cast<Char>(' ') || c == static_cast<Char>('\t');
}

template <class Char>
struct LineView {
  uint64_t off = 0;
  uint64_t len = 0;  // 不含行尾换行（以及 \r）
  const Char* p = nullptr;
  uint32_t plen = 0;
  bool truncated = false;

  Char At(uint32_t i) const { return i < plen ? p[i] : static_cast<Char>(0); }

  bool StartsWith(const char* s) const {
    const size_t n = std::strlen(s);
    if (plen < n) return false;
    for (size_t i = 0; i < n; ++i) {
      if (p[i] != static_cast<Char>(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
  }

  bool StartsWithNoCase(const char* s) const {
    const size_t n = std::strlen(s);
    if (plen < n) return false;
    for (size_t i = 0; i < n; ++i) {
      Char a = p[i];
      Char b = static_cast<Char>(static_cast<unsigned char>(s[i]));
      if (a >= static_cast<Char>('A') && a <= static_cast<Char>('Z')) {
        a = static_cast<Char>(a - static_cast<Char>('A') + static_cast<Char>('a'));
      }
      if (b >= static_cast<Char>('A') && b <= static_cast<Char>('Z')) {
        b = static_cast<Char>(b - static_cast<Char>('A') + static_cast<Char>('a'));
      }
      if (a != b) return false;
    }
    return true;
  }

  // 原始字符下标（tab 算 1 个），用于访问前缀。
  uint32_t FirstNonSpace() const {
    uint32_t i = 0;
    while (i < plen && IsSpace(p[i])) ++i;
    return i;
  }

  // 视觉缩进列（tab 记 4），用于列表缩进比较。
  uint32_t Indent() const {
    uint32_t c = 0;
    for (uint32_t i = 0; i < plen; ++i) {
      if (p[i] == static_cast<Char>(' ')) ++c;
      else if (p[i] == static_cast<Char>('\t')) c += 4;
      else break;
    }
    return c;
  }

  bool IsBlank() const { return !truncated && FirstNonSpace() >= plen; }

  bool Contains(Char c) const {
    for (uint32_t i = 0; i < plen; ++i) {
      if (p[i] == c) return true;
    }
    return false;
  }

  bool AllOf(Char c) const {
    const uint32_t s = FirstNonSpace();
    if (truncated || s >= plen) return false;
    for (uint32_t i = s; i < plen; ++i) {
      if (p[i] != c) return false;
    }
    return true;
  }
};

template <class Char>
class BlockScanner;

template <class Char>
const Char* FindNewline(const Char* p, size_t n);

template <>
const char* FindNewline<char>(const char* p, size_t n) {
  return static_cast<const char*>(std::memchr(p, '\n', n));
}

template <>
const wchar_t* FindNewline<wchar_t>(const wchar_t* p, size_t n) {
  return std::wmemchr(p, L'\n', n);
}

template <class Char>
class LineFeeder {
 public:
  explicit LineFeeder(BlockScanner<Char>* sink) : sink_(sink) {}

  // base 是 data[0] 在文件中的单位偏移。
  void Feed(const Char* data, size_t n, uint64_t base) {
    if (!started_) Reset(base);
    size_t i = 0;
    while (i < n) {
      const Char* nl = FindNewline(data + i, n - i);
      if (!nl) {
        Append(data + i, n - i);
        return;
      }
      const size_t seg = static_cast<size_t>(nl - (data + i));
      Append(data + i, seg);
      Emit();
      i += seg + 1;
      Reset(base + static_cast<uint64_t>(i));
    }
  }

  void Finish() {
    if (lineLen_ > 0) Emit();
    lineLen_ = 0;
    plen_ = 0;
  }

 private:
  void Reset(uint64_t off) {
    plen_ = 0;
    lineOff_ = off;
    lineLen_ = 0;
    last_ = 0;
    started_ = true;
  }

  void Append(const Char* s, size_t n) {
    if (n == 0) return;
    const size_t room = kPrefix - plen_;
    if (room) {
      const size_t take = n < room ? n : room;
      std::memcpy(prefix_ + plen_, s, take * sizeof(Char));
      plen_ = static_cast<uint32_t>(plen_ + take);
    }
    lineLen_ += n;
    last_ = s[n - 1];
  }

  void Emit() {
    LineView<Char> lv;
    lv.off = lineOff_;
    lv.len = (lineLen_ > 0 && last_ == static_cast<Char>('\r')) ? lineLen_ - 1 : lineLen_;
    lv.p = prefix_;
    lv.plen = static_cast<uint32_t>(std::min<uint64_t>(lv.len, plen_));
    lv.truncated = lineLen_ > kPrefix;
    sink_->Feed(lv);
  }

  Char prefix_[kPrefix];
  uint32_t plen_ = 0;
  uint64_t lineOff_ = 0;
  uint64_t lineLen_ = 0;
  Char last_ = 0;
  bool started_ = false;
  BlockScanner<Char>* sink_;
};

struct Bullet {
  bool ordered = false;
  uint32_t indent = 0;
  uint32_t contentIndent = 0;
};

template <class Char>
class BlockScanner {
 public:
  struct Sink {
    virtual void Push(uint64_t start, uint64_t end, uint8_t flags, uint32_t lines) = 0;
    virtual void Heading(uint64_t lineStart, uint64_t lineEnd, uint64_t textStart,
                         uint64_t textLen, uint8_t level) = 0;
    virtual ~Sink() {}
  };

  BlockScanner(Sink* sink, uint64_t maxHtmlUnits)
      : sink_(sink), maxHtmlUnits_(maxHtmlUnits) {}

  void Feed(const LineView<Char>& l) {
    if (inFence_) {
      if (kind_ == Kind::None) {
        Start(l, Kind::Fence, 0, 0);
      } else {
        Extend(l);
      }
      if (IsFenceClose(l)) {
        inFence_ = false;
        CloseBlock(0);
      }
      return;
    }

    if (inFm_) {
      Extend(l);
      if (IsFmClose(l)) {
        inFm_ = false;
        CloseBlock(0);
      }
      return;
    }

    if (inHtml_) {
      if (l.IsBlank()) {
        if (htmlDepth_ > 0) {
          Extend(l);
          return;
        }
        inHtml_ = false;
        CloseBlock(0);
        return;
      }
      UpdateHtmlDepth(l);
      Extend(l);
      if (end_ - start_ > maxHtmlUnits_) {
        // 外壳太大就放弃继续合并，接受外壳丢失（票 05 6.1）
        inHtml_ = false;
        htmlDepth_ = 0;
        CloseBlock(0);
      }
      return;
    }

    if (inTable_) {
      if (l.IsBlank()) {
        inTable_ = false;
        CloseBlock(0);
        return;
      }
      if (!l.Contains(static_cast<Char>('|'))) {
        inTable_ = false;
        CloseBlock(0);
      } else {
        Extend(l);
        return;
      }
    }

    if (l.IsBlank()) {
      if (kind_ == Kind::ListItem ||
          (kind_ == Kind::Para && (indent_ >= 4 || (flags_ & kFlagHidden) != 0))) {
        pendingBlank_ = true;
        return;
      }
      CloseBlock(0);
      return;
    }

    if (pendingBlank_) {
      pendingBlank_ = false;
      if (kind_ == Kind::ListItem) {
        Bullet b;
        const bool bullet = ParseBullet(l, &b);
        if (l.Indent() >= contentIndent_ || (bullet && b.indent > indent_)) {
          Extend(l);
          flags_ |= kFlagLoose;  // 项内出现空行
          return;
        }
        if (bullet && b.indent <= indent_ && b.ordered == ordered_) {
          // 同级项：先不关，下面 bullet 分支会把两侧都标成 loose
          looseAfterBlank_ = true;
          looseNext_ = true;
        } else {
          CloseBlock(0);
        }
      } else if (kind_ == Kind::Para) {
        if ((flags_ & kFlagHidden) != 0) {
          if (l.Indent() >= 2) {
            Extend(l);
            return;
          }
          CloseBlock(0);
        } else if (indent_ >= 4 && l.Indent() >= 4) {
          Extend(l);
          return;
        } else {
          CloseBlock(0);
        }
      } else if (kind_ != Kind::None) {
        CloseBlock(0);
      }
    }

    // 单行回看：Setext 下划线 / 表格分隔行
    if (kind_ == Kind::Para && lineCount_ == 1 && (flags_ & kFlagHidden) == 0) {
      if (l.AllOf(static_cast<Char>('='))) {
        kind_ = Kind::H1;
        Extend(l);
        AddSetextOutline(1);
        CloseBlock(0);
        return;
      }
      if (l.AllOf(static_cast<Char>('-')) && !l.Contains(static_cast<Char>('|'))) {
        kind_ = Kind::H2;
        Extend(l);
        AddSetextOutline(2);
        CloseBlock(0);
        return;
      }
      if (firstPipe_ && IsTableDelim(l)) {
        kind_ = Kind::Table;
        inTable_ = true;
        Extend(l);
        return;
      }
    }

    if (kind_ == Kind::None && l.off == 0 && IsFmOpen(l)) {
      Start(l, Kind::FrontMatter, kFlagHidden, 0);
      inFm_ = true;
      return;
    }

    Char fenceCh = 0;
    uint32_t fenceLen = 0;
    if (IsFenceStart(l, &fenceCh, &fenceLen)) {
      CloseBlock(0);
      Start(l, Kind::Fence, 0, 0);
      inFence_ = true;
      fenceCh_ = fenceCh;
      fenceLen_ = fenceLen;
      return;
    }

    uint32_t level = 0;
    uint32_t textOff = 0;
    uint32_t textLen = 0;
    if (ParseAtxHeading(l, &level, &textOff, &textLen)) {
      CloseBlock(0);
      sink_->Push(l.off, l.off + l.len,
                  static_cast<uint8_t>(static_cast<uint8_t>(Kind::H1) + (level - 1)), 1);
      sink_->Heading(l.off, l.off + l.len, l.off + textOff, textLen,
                     static_cast<uint8_t>(level));
      return;
    }

    if (IsHr(l)) {
      CloseBlock(0);
      sink_->Push(l.off, l.off + l.len, static_cast<uint8_t>(Kind::Hr), 1);
      return;
    }

    if (IsHtmlStart(l)) {
      CloseBlock(0);
      Start(l, Kind::Html, 0, 0);
      inHtml_ = true;
      htmlDepth_ = 0;
      UpdateHtmlDepth(l);
      return;
    }

    if (kind_ == Kind::None) {
      uint8_t extra = 0;
      if (IsDefinitionLine(l, &extra)) {
        Start(l, Kind::Para, static_cast<uint8_t>(kFlagHidden | extra), 0);
        return;
      }
    }

    Bullet bullet;
    if (ParseBullet(l, &bullet)) {
      if (kind_ == Kind::ListItem && bullet.indent <= indent_) {
        if (bullet.ordered == ordered_) {
          CloseBlock(looseAfterBlank_ ? kFlagLoose : 0);
          Start(l, Kind::ListItem, looseNext_ ? kFlagLoose : 0, bullet.contentIndent);
        } else {
          // 有序列表紧接无序列表：两条独立的列表，不共用松散状态
          CloseBlock(0);
          Start(l, Kind::ListItem, 0, bullet.contentIndent);
        }
        looseAfterBlank_ = false;
        looseNext_ = false;
        ordered_ = bullet.ordered;
        return;
      }
      if (kind_ == Kind::ListItem) {
        Extend(l);
        return;
      }
      CloseBlock(0);
      Start(l, Kind::ListItem, looseNext_ ? kFlagLoose : 0, bullet.contentIndent);
      looseNext_ = false;
      ordered_ = bullet.ordered;
      return;
    }

    if (l.At(l.FirstNonSpace()) == static_cast<Char>('>')) {
      if (kind_ == Kind::Quote) {
        Extend(l);
      } else {
        CloseBlock(0);
        Start(l, Kind::Quote, 0, 0);
      }
      return;
    }

    if (kind_ == Kind::Para) {
      if ((flags_ & kFlagHidden) != 0) {
        if (l.Indent() >= 2) {
          Extend(l);
          return;
        }
        // 定义块不做懒续行：紧贴的非缩进行另起一段（连续的定义各自成块）
        CloseBlock(0);
        uint8_t extra = 0;
        if (IsDefinitionLine(l, &extra)) {
          Start(l, Kind::Para, static_cast<uint8_t>(kFlagHidden | extra), 0);
        } else {
          Start(l, Kind::Para, 0, 0);
        }
        return;
      }
      Extend(l);
      return;
    }

    if (kind_ == Kind::ListItem) {
      Extend(l);
      return;
    }

    CloseBlock(0);
    Start(l, Kind::Para, 0, 0);
  }

  void Finish(uint64_t endUnits) {
    (void)endUnits;
    if (inFm_) {
      // 没闭合的 front matter 不剥（与 shell 的正则一致），按普通段落处理
      inFm_ = false;
      kind_ = Kind::Para;
      flags_ = static_cast<uint8_t>(flags_ & ~kFlagHidden);
    }
    CloseBlock(0);
  }

 private:
  void Start(const LineView<Char>& l, Kind k, uint8_t flags, uint32_t contentIndent) {
    start_ = l.off;
    end_ = l.off + l.len;
    kind_ = k;
    flags_ = flags;
    indent_ = l.Indent();
    contentIndent_ = contentIndent;
    lineCount_ = 1;
    const uint32_t s = l.FirstNonSpace();
    firstPipe_ = s < l.plen && l.At(s) == static_cast<Char>('|');

    uint32_t e = l.plen;
    while (e > s && IsSpace(l.At(e - 1))) --e;
    firstTextOff_ = s;
    firstTextLen_ = e > s ? e - s : 0;
  }

  void Extend(const LineView<Char>& l) {
    end_ = l.off + l.len;
    ++lineCount_;
  }

  void CloseBlock(uint8_t extraFlags) {
    if (kind_ != Kind::None && end_ > start_) {
      const uint8_t kindBits = static_cast<uint8_t>(kind_) & kKindMask;
      sink_->Push(start_, end_, static_cast<uint8_t>(kindBits | flags_ | extraFlags),
                  lineCount_);
    }
    kind_ = Kind::None;
    flags_ = 0;
    start_ = end_ = 0;
    lineCount_ = 0;
    indent_ = 0;
    contentIndent_ = 0;
    firstTextOff_ = 0;
    firstTextLen_ = 0;
    ordered_ = false;
  }

  void AddSetextOutline(uint8_t level) {
    if (firstTextLen_ == 0) return;
    sink_->Heading(start_, end_, start_ + firstTextOff_, firstTextLen_, level);
  }

  static bool IsAsciiAlpha(Char c) {
    return (c >= static_cast<Char>('a') && c <= static_cast<Char>('z')) ||
           (c >= static_cast<Char>('A') && c <= static_cast<Char>('Z'));
  }

  static bool IsAsciiAlnum(Char c) {
    return IsAsciiAlpha(c) || (c >= static_cast<Char>('0') && c <= static_cast<Char>('9'));
  }

  static bool TagEquals(const LineView<Char>& l, uint32_t at, uint32_t len, const char* tag) {
    const size_t n = std::strlen(tag);
    if (len != n) return false;
    for (size_t i = 0; i < n; ++i) {
      Char a = l.At(at + static_cast<uint32_t>(i));
      Char b = static_cast<Char>(static_cast<unsigned char>(tag[i]));
      if (a >= static_cast<Char>('A') && a <= static_cast<Char>('Z')) {
        a = static_cast<Char>(a - static_cast<Char>('A') + static_cast<Char>('a'));
      }
      if (a != b) return false;
    }
    return true;
  }

  static bool IsBlockTag(const LineView<Char>& l, uint32_t at, uint32_t len) {
    static const char* kTags[] = {
        "address", "article", "aside",  "blockquote", "details", "dialog", "div",
        "dl",      "fieldset", "figcaption", "figure", "footer", "form", "h1",
        "h2",      "h3",   "h4",   "h5",   "h6",   "header", "hr",   "iframe",
        "li",      "main", "nav",  "ol",   "p",    "pre",    "script", "section",
        "style",   "summary", "table", "ul", "video", "audio", "canvas", "noscript",
    };
    for (const char* tag : kTags) {
      if (TagEquals(l, at, len, tag)) return true;
    }
    return false;
  }

  void UpdateHtmlDepth(const LineView<Char>& l) {
    uint32_t i = 0;
    while (i + 1 < l.plen) {
      if (l.At(i) != static_cast<Char>('<')) {
        ++i;
        continue;
      }
      if (l.At(i + 1) == static_cast<Char>('!') ||
          l.At(i + 1) == static_cast<Char>('?')) {
        ++i;
        continue;
      }
      uint32_t j = i + 1;
      bool closing = false;
      if (j < l.plen && l.At(j) == static_cast<Char>('/')) {
        closing = true;
        ++j;
      }
      if (j >= l.plen || !IsAsciiAlpha(l.At(j))) {
        ++i;
        continue;
      }
      const uint32_t nameStart = j;
      while (j < l.plen && (IsAsciiAlnum(l.At(j)) || l.At(j) == static_cast<Char>('-') ||
                            l.At(j) == static_cast<Char>(':'))) {
        ++j;
      }
      const uint32_t nameLen = j - nameStart;

      bool selfClosing = false;
      uint32_t q = j;
      while (q < l.plen && l.At(q) != static_cast<Char>('>')) {
        if (l.At(q) == static_cast<Char>('/')) selfClosing = true;
        ++q;
      }
      if (q >= l.plen) break;  // 标签跨行，前缀里判不了
      if (!selfClosing && IsBlockTag(l, nameStart, nameLen)) {
        if (closing) {
          if (htmlDepth_ > 0) --htmlDepth_;
        } else {
          ++htmlDepth_;
        }
      }
      i = q + 1;
    }
  }

  static bool IsFmOpen(const LineView<Char>& l) {
    if (l.FirstNonSpace() != 0) return false;
    if (!l.StartsWith("---")) return false;
    for (uint32_t i = 3; i < l.plen; ++i) {
      if (!IsSpace(l.At(i))) return false;
    }
    return !l.truncated;
  }

  static bool IsFmClose(const LineView<Char>& l) {
    const char* delims[] = {"---", "..."};
    for (const char* d : delims) {
      if (!l.StartsWith(d)) continue;
      bool ok = true;
      for (uint32_t i = 3; i < l.plen; ++i) {
        if (!IsSpace(l.At(i))) {
          ok = false;
          break;
        }
      }
      if (ok && !l.truncated) return true;
    }
    return false;
  }

  static bool IsFenceStart(const LineView<Char>& l, Char* ch, uint32_t* len) {
    const uint32_t s = l.FirstNonSpace();
    if (s > 3 || s >= l.plen) return false;
    const Char c = l.At(s);
    if (c != static_cast<Char>('`') && c != static_cast<Char>('~')) return false;
    uint32_t n = 0;
    while (l.At(s + n) == c) ++n;
    if (n < 3) return false;
    *ch = c;
    *len = n;
    return true;
  }

  bool IsFenceClose(const LineView<Char>& l) const {
    const uint32_t s = l.FirstNonSpace();
    if (s > 3 || s >= l.plen || l.truncated) return false;
    uint32_t n = 0;
    while (l.At(s + n) == fenceCh_) ++n;
    if (n < fenceLen_) return false;
    for (uint32_t i = s + n; i < l.plen; ++i) {
      if (!IsSpace(l.At(i))) return false;
    }
    return true;
  }

  static bool IsTableDelim(const LineView<Char>& l) {
    if (!l.Contains(static_cast<Char>('|')) || l.truncated) return false;
    bool dash = false;
    for (uint32_t i = 0; i < l.plen; ++i) {
      const Char c = l.At(i);
      if (c == static_cast<Char>('-')) {
        dash = true;
      } else if (c != static_cast<Char>('|') && c != static_cast<Char>(':') &&
                 !IsSpace(c)) {
        return false;
      }
    }
    return dash;
  }

  static bool ParseAtxHeading(const LineView<Char>& l, uint32_t* level, uint32_t* textOff,
                              uint32_t* textLen) {
    const uint32_t s = l.FirstNonSpace();
    if (s > 3 || s >= l.plen) return false;
    uint32_t n = 0;
    while (s + n < l.plen && l.At(s + n) == static_cast<Char>('#')) ++n;
    if (n < 1 || n > 6) return false;
    if (s + n < l.plen && !IsSpace(l.At(s + n))) return false;

    uint32_t t0 = s + n;
    while (t0 < l.plen && IsSpace(l.At(t0))) ++t0;
    uint32_t t1 = l.plen;
    while (t1 > t0 && IsSpace(l.At(t1 - 1))) --t1;

    uint32_t h = t1;
    while (h > t0 && l.At(h - 1) == static_cast<Char>('#')) --h;
    if (h < t1 && (h == t0 || IsSpace(l.At(h - 1)))) {
      t1 = h;
      while (t1 > t0 && IsSpace(l.At(t1 - 1))) --t1;
    }

    *level = n;
    *textOff = t0;
    *textLen = t1 > t0 ? t1 - t0 : 0;
    return true;
  }

  static bool IsHr(const LineView<Char>& l) {
    const uint32_t s = l.FirstNonSpace();
    if (s >= l.plen || l.truncated) return false;
    const Char c = l.At(s);
    if (c != static_cast<Char>('-') && c != static_cast<Char>('*') &&
        c != static_cast<Char>('_')) {
      return false;
    }
    uint32_t n = 0;
    for (uint32_t i = s; i < l.plen; ++i) {
      if (l.At(i) == c) {
        ++n;
      } else if (!IsSpace(l.At(i))) {
        return false;
      }
    }
    return n >= 3;
  }

  static bool MatchTagAt(const LineView<Char>& l, uint32_t s, const char* tag) {
    const size_t n = std::strlen(tag);
    if (l.plen < s + n) return false;
    for (size_t i = 0; i < n; ++i) {
      Char a = l.At(s + static_cast<uint32_t>(i));
      Char b = static_cast<Char>(static_cast<unsigned char>(tag[i]));
      if (a >= static_cast<Char>('A') && a <= static_cast<Char>('Z')) {
        a = static_cast<Char>(a - static_cast<Char>('A') + static_cast<Char>('a'));
      }
      if (a != b) return false;
    }
    return true;
  }

  static bool IsHtmlStart(const LineView<Char>& l) {
    const uint32_t s = l.FirstNonSpace();
    if (l.At(s) != static_cast<Char>('<')) return false;
    static const char* kTags[] = {
        "<div", "<table", "<pre", "<script", "<style", "<section", "<article",
        "<details", "<figure", "<iframe", "<!--", "<summary", "<ul", "<ol", "<dl",
        "<blockquote", "<form", "<p", "<h1", "<h2", "<h3", "<h4", "<h5", "<h6",
    };
    for (const char* tag : kTags) {
      if (MatchTagAt(l, s, tag)) return true;
    }
    return false;
  }

  static bool ParseBullet(const LineView<Char>& l, Bullet* out) {
    const uint32_t s = l.FirstNonSpace();
    if (s >= l.plen) return false;
    const uint32_t indent = l.Indent();
    if (indent > 60) return false;

    const Char c = l.At(s);
    bool ordered = false;
    uint32_t markerLen = 0;
    if (c == static_cast<Char>('-') || c == static_cast<Char>('*') ||
        c == static_cast<Char>('+')) {
      markerLen = 1;
    } else if (c >= static_cast<Char>('0') && c <= static_cast<Char>('9')) {
      uint32_t d = 0;
      while (d < 9 && s + d < l.plen && l.At(s + d) >= static_cast<Char>('0') &&
             l.At(s + d) <= static_cast<Char>('9')) {
        ++d;
      }
      if (d == 0) return false;
      if (s + d >= l.plen) return false;
      const Char sep = l.At(s + d);
      if (sep != static_cast<Char>('.') && sep != static_cast<Char>(')')) return false;
      markerLen = d + 1;
      ordered = true;
    } else {
      return false;
    }

    const uint32_t after = s + markerLen;
    uint32_t spaces = 1;
    if (after < l.plen) {
      if (!IsSpace(l.At(after))) return false;
      spaces = 0;
      while (after + spaces < l.plen && IsSpace(l.At(after + spaces)) && spaces < 4) {
        ++spaces;
      }
      if (spaces == 0) spaces = 1;
    }

    out->ordered = ordered;
    out->indent = indent;
    out->contentIndent = indent + markerLen + spaces;
    return true;
  }

  static bool IsDefinitionLine(const LineView<Char>& l, uint8_t* extraFlags) {
    const uint32_t s = l.FirstNonSpace();
    if (s > 3 || s >= l.plen || l.At(s) != static_cast<Char>('[')) return false;
    uint32_t i = s + 1;
    bool footnote = false;
    if (i < l.plen && l.At(i) == static_cast<Char>('^')) {
      footnote = true;
      ++i;
    }
    const uint32_t labelStart = i;
    while (i < l.plen && l.At(i) != static_cast<Char>(']')) ++i;
    if (i >= l.plen || i == labelStart) return false;
    if (i - labelStart > 999) return false;
    ++i;
    if (i >= l.plen || l.At(i) != static_cast<Char>(':')) return false;
    *extraFlags = footnote ? static_cast<uint8_t>(kFlagFootnoteDef) : 0;
    return true;
  }

  Sink* sink_;
  uint64_t maxHtmlUnits_;

  Kind kind_ = Kind::None;
  uint8_t flags_ = 0;
  uint64_t start_ = 0;
  uint64_t end_ = 0;
  uint32_t lineCount_ = 0;
  uint32_t indent_ = 0;
  uint32_t contentIndent_ = 0;
  uint32_t firstTextOff_ = 0;
  uint32_t firstTextLen_ = 0;
  bool firstPipe_ = false;
  bool ordered_ = false;

  bool inFence_ = false;
  Char fenceCh_ = 0;
  uint32_t fenceLen_ = 0;
  bool inFm_ = false;
  bool inHtml_ = false;
  int htmlDepth_ = 0;
  bool inTable_ = false;

  bool pendingBlank_ = false;
  bool looseAfterBlank_ = false;
  bool looseNext_ = false;
};

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
    if (i + need >= n) return true;  // 尾巴被切在字符中间，判不了，按合法处理
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

double QpcMs(LONGLONG from, LONGLONG to) {
  static LONGLONG freq = 0;
  if (freq == 0) {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    freq = f.QuadPart ? f.QuadPart : 1;
  }
  return static_cast<double>(to - from) * 1000.0 / static_cast<double>(freq);
}

}  // namespace

// ---------------------------------------------------------------- 编码判定

EncodingInfo DetectEncoding(const void* data, size_t bytes) {
  EncodingInfo info;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  if (bytes >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
    info.encoding = Encoding::Utf8;
    info.bomBytes = 3;
    return info;
  }
  if (bytes >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
    info.encoding = Encoding::Utf16LE;
    info.bomBytes = 2;
    return info;
  }
  if (bytes >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
    info.encoding = Encoding::Utf16BE;
    info.bomBytes = 2;
    return info;
  }
  // 无 BOM：UTF-8 合法就是 UTF-8，否则按 GB18030；纯 ASCII 两者等价。
  info.encoding = LooksLikeUtf8(p, bytes) ? Encoding::Utf8 : Encoding::Gb18030;
  info.bomBytes = 0;
  return info;
}

// ---------------------------------------------------------------- BlockIndex

void BlockIndex::Clear() {
  pageBase_.clear();
  offInPage_.clear();
  len16_.clear();
  bigLen_.clear();
  bigBlock_.clear();
  kindFlags_.clear();
  bigBlockIndex_.clear();
  bigBlockLines_.clear();
  fileSize_ = 0;
  pageCount_ = 0;
  sealed_ = false;
}

void BlockIndex::Reserve(uint64_t fileSize, uint64_t reserveBytes) {
  Clear();
  fileSize_ = fileSize;
  pageCount_ = static_cast<uint32_t>(fileSize / kPageSize) + 1;
  pageBase_.assign(static_cast<size_t>(pageCount_) + 2, 0);

  // 票 04：按字节数 / 200 预留，避免 4 M 条记录反复搬家。前缀扫描只预留前缀。
  size_t est = static_cast<size_t>(reserveBytes / 200);
  if (est > 40000000u) est = 40000000u;
  offInPage_.reserve(est);
  len16_.reserve(est);
  kindFlags_.reserve(est);
}

void BlockIndex::PushBlock(uint64_t start, uint64_t len, uint8_t kindFlags, uint32_t lines) {
  if (sealed_) return;
  const uint32_t page = static_cast<uint32_t>(start >> kPageShift);
  if (static_cast<size_t>(page) + 2 >= pageBase_.size()) return;

  const uint32_t i = BlockCount();
  offInPage_.push_back(static_cast<uint16_t>(start & (kPageSize - 1)));

  const uint32_t len32 = len > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(len);
  if (len32 >= kBigLenMark) {
    len16_.push_back(static_cast<uint16_t>(kBigLenMark));
    bigLen_.push_back(len32);
    bigBlock_.push_back(i);
  } else {
    len16_.push_back(static_cast<uint16_t>(len32));
  }
  kindFlags_.push_back(kindFlags);
  if (lines > kBigBlockLines) {
    bigBlockIndex_.push_back(i);
    bigBlockLines_.push_back(lines);
  }
  ++pageBase_[static_cast<size_t>(page) + 1];
}

void BlockIndex::SealPages() {
  if (sealed_) return;
  for (size_t p = 1; p < pageBase_.size(); ++p) pageBase_[p] += pageBase_[p - 1];
  sealed_ = true;
}

uint32_t BlockIndex::PageOf(uint32_t i) const {
  uint32_t lo = 0;
  uint32_t hi = static_cast<uint32_t>(pageBase_.size()) - 1;
  while (lo + 1 < hi) {
    const uint32_t mid = (lo + hi) >> 1;
    if (pageBase_[mid] <= i) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

uint64_t BlockIndex::Start(uint32_t i) const {
  return static_cast<uint64_t>(PageOf(i)) * kPageSize + offInPage_[i];
}

uint32_t BlockIndex::Len(uint32_t i) const {
  const uint16_t v = len16_[i];
  if (v != kBigLenMark) return v;
  size_t lo = 0;
  size_t hi = bigBlock_.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (bigBlock_[mid] < i) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < bigBlock_.size() && bigBlock_[lo] == i) return bigLen_[lo];
  return kBigLenMark;
}

uint8_t BlockIndex::KindFlags(uint32_t i) const { return kindFlags_[i]; }

uint32_t BlockIndex::BigLines(uint32_t i) const {
  size_t lo = 0;
  size_t hi = bigBlockIndex_.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (bigBlockIndex_[mid] < i) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < bigBlockIndex_.size() && bigBlockIndex_[lo] == i) return bigBlockLines_[lo];
  return 0;
}

uint32_t BlockIndex::BlockAtOffset(uint64_t off) const {
  if (!sealed_ || offInPage_.empty()) return kInvalidBlock;
  const uint32_t page = static_cast<uint32_t>(off >> kPageShift);
  if (static_cast<size_t>(page) + 1 >= pageBase_.size()) return kInvalidBlock;

  const uint32_t lo = pageBase_[page];
  const uint32_t hi = pageBase_[static_cast<size_t>(page) + 1];
  uint32_t best = kInvalidBlock;
  uint32_t a = lo;
  uint32_t b = hi;
  while (a < b) {
    const uint32_t mid = (a + b) >> 1;
    if (Start(mid) <= off) {
      best = mid;
      a = mid + 1;
    } else {
      b = mid;
    }
  }
  if (best != kInvalidBlock) return best;
  if (lo > 0) return lo - 1;  // 空行区间，或超长块从前一页延续过来
  return kInvalidBlock;
}

size_t BlockIndex::MemoryBytes() const {
  return pageBase_.size() * sizeof(uint32_t) + offInPage_.size() * sizeof(uint16_t) +
         len16_.size() * sizeof(uint16_t) + bigLen_.size() * sizeof(uint32_t) +
         bigBlock_.size() * sizeof(uint32_t) + kindFlags_.size() * sizeof(uint8_t) +
         (bigBlockIndex_.size() + bigBlockLines_.size()) * sizeof(uint32_t);
}

// ---------------------------------------------------------------- IndexBuilder

struct IndexBuilder::Impl final : BlockScanner<char>::Sink,
                                  BlockScanner<wchar_t>::Sink {
  BlockIndex index;
  std::vector<OutlineNode> outline;

  EncodingInfo enc;
  uint64_t fileSize = 0;
  uint64_t limit = 0;
  uint64_t consumed = 0;
  uint64_t units = 0;
  uint64_t unitBytes = 1;
  uint64_t baseBytes = 0;
  bool started = false;
  bool finished = false;

  std::unique_ptr<BlockScanner<char>> charScanner;
  std::unique_ptr<LineFeeder<char>> charFeeder;
  std::unique_ptr<BlockScanner<wchar_t>> wideScanner;
  std::unique_ptr<LineFeeder<wchar_t>> wideFeeder;
  std::vector<wchar_t> conv;
  bool haveCarry = false;
  unsigned char carry = 0;

  void Push(uint64_t start, uint64_t end, uint8_t flags, uint32_t lines) override {
    const uint64_t s = baseBytes + start * unitBytes;
    const uint64_t e = baseBytes + end * unitBytes;
    if (e > s) index.PushBlock(s, e - s, flags, lines);
  }

  void Heading(uint64_t lineStart, uint64_t lineEnd, uint64_t textStart, uint64_t textLen,
               uint8_t level) override {
    (void)lineEnd;
    OutlineNode node = {};
    node.start = baseBytes + lineStart * unitBytes;
    node.textOff = static_cast<uint16_t>(
        std::min<uint64_t>((textStart - lineStart) * unitBytes, 0xFFFFu));
    node.textLen = static_cast<uint16_t>(std::min<uint64_t>(textLen * unitBytes, 0xFFFFu));
    node.level = level;
    outline.push_back(node);
  }
};

IndexBuilder::IndexBuilder() : impl_(new Impl()) {}

IndexBuilder::~IndexBuilder() {}

bool IndexBuilder::Begin(EncodingInfo enc, uint64_t fileSize, uint64_t limitBytes) {
  Impl& s = *impl_;
  if (s.started) return false;
  if (fileSize > kMaxFileBytes) return false;

  s.enc = enc;
  s.fileSize = fileSize;
  s.limit = limitBytes && limitBytes < fileSize ? limitBytes : fileSize;
  s.baseBytes = enc.bomBytes;
  s.unitBytes = (enc.encoding == Encoding::Utf16LE || enc.encoding == Encoding::Utf16BE)
                    ? 2u
                    : 1u;
  s.index.Reserve(fileSize, s.limit);

  if (s.unitBytes == 2) {
    s.wideScanner.reset(new BlockScanner<wchar_t>(&s, kMaxHtmlBytes / 2));
    s.wideFeeder.reset(new LineFeeder<wchar_t>(s.wideScanner.get()));
  } else {
    s.charScanner.reset(new BlockScanner<char>(&s, kMaxHtmlBytes));
    s.charFeeder.reset(new LineFeeder<char>(s.charScanner.get()));
  }
  s.started = true;
  return true;
}

void IndexBuilder::Feed(const void* data, size_t bytes) {
  Impl& s = *impl_;
  if (!s.started || s.finished || !data || bytes == 0) return;

  const unsigned char* p = static_cast<const unsigned char*>(data);
  size_t n = bytes;
  if (s.consumed + n > s.limit) n = static_cast<size_t>(s.limit - s.consumed);
  if (n == 0) return;

  if (s.consumed == 0 && s.enc.bomBytes) {
    const size_t skip = std::min<size_t>(s.enc.bomBytes, n);
    s.consumed += skip;
    p += skip;
    n -= skip;
    if (n == 0) return;
  }

  if (s.unitBytes == 1) {
    s.charFeeder->Feed(reinterpret_cast<const char*>(p), n, s.consumed);
    s.consumed += n;
    s.units += n;
    return;
  }

  s.conv.clear();
  s.conv.reserve((n + 1) / 2);
  size_t i = 0;
  if (s.haveCarry) {
    const unsigned char b0 = s.carry;
    const unsigned char b1 = p[0];
    s.conv.push_back(s.enc.encoding == Encoding::Utf16LE
                         ? static_cast<wchar_t>(b0 | (b1 << 8))
                         : static_cast<wchar_t>((b0 << 8) | b1));
    s.haveCarry = false;
    i = 1;
  }
  for (; i + 1 < n; i += 2) {
    const unsigned char b0 = p[i];
    const unsigned char b1 = p[i + 1];
    s.conv.push_back(s.enc.encoding == Encoding::Utf16LE
                         ? static_cast<wchar_t>(b0 | (b1 << 8))
                         : static_cast<wchar_t>((b0 << 8) | b1));
  }
  if (i < n) {
    s.carry = p[i];
    s.haveCarry = true;
  }
  if (!s.conv.empty()) {
    s.wideFeeder->Feed(s.conv.data(), s.conv.size(), s.units);
    s.units += s.conv.size();
  }
  s.consumed += n;
}

void IndexBuilder::Finish() {
  Impl& s = *impl_;
  if (!s.started || s.finished) return;
  s.finished = true;
  if (s.unitBytes == 1) {
    s.charFeeder->Finish();
    s.charScanner->Finish(s.units);
  } else {
    s.wideFeeder->Finish();
    s.wideScanner->Finish(s.units);
  }
  s.index.SealPages();
}

bool IndexBuilder::finished() const { return impl_->finished; }
uint64_t IndexBuilder::LimitBytes() const { return impl_->limit; }
uint64_t IndexBuilder::BytesScanned() const { return impl_->consumed; }
uint64_t IndexBuilder::UnitsScanned() const { return impl_->units; }
const BlockIndex& IndexBuilder::index() const { return impl_->index; }
const std::vector<OutlineNode>& IndexBuilder::outline() const { return impl_->outline; }
EncodingInfo IndexBuilder::encoding() const { return impl_->enc; }

bool IndexBuilder::CanFeedMore() const {
  const Impl& s = *impl_;
  return s.started && !s.finished && s.consumed < s.limit;
}

// ---------------------------------------------------------------- ScanFile

bool ScanFile(const std::wstring& path, uint64_t limitBytes, IndexBuilder* out,
              double* readMs, double* scanMs, std::wstring* error,
              const std::atomic<bool>* cancel) {
  if (error) error->clear();
  if (readMs) *readMs = 0;
  if (scanMs) *scanMs = 0;
  if (!out) return false;

  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (error) *error = L"打开文件失败";
    return false;
  }

  LARGE_INTEGER size = {};
  if (!::GetFileSizeEx(h, &size)) {
    ::CloseHandle(h);
    if (error) *error = L"读取文件大小失败";
    return false;
  }
  const uint64_t fileSize = static_cast<uint64_t>(size.QuadPart);
  const uint64_t limit =
      limitBytes && limitBytes < fileSize ? limitBytes : fileSize;

  std::vector<unsigned char> buf(static_cast<size_t>(kReadChunk));
  uint64_t consumed = 0;
  double rms = 0;
  double sms = 0;
  EncodingInfo enc;
  bool begun = false;

  LARGE_INTEGER t0;
  LARGE_INTEGER t1;

  while (consumed < limit) {
    if (cancel && cancel->load()) {
      ::CloseHandle(h);
      if (error) *error = L"已取消";
      return false;
    }
    size_t want = static_cast<size_t>(std::min<uint64_t>(kReadChunk, limit - consumed));
    DWORD got = 0;
    ::QueryPerformanceCounter(&t0);
    const BOOL ok = ::ReadFile(h, buf.data(), static_cast<DWORD>(want), &got, nullptr);
    ::QueryPerformanceCounter(&t1);
    rms += QpcMs(t0.QuadPart, t1.QuadPart);
    if (!ok || got == 0) break;

    ::QueryPerformanceCounter(&t0);
    if (!begun) {
      enc = DetectEncoding(buf.data(), got);
      if (!out->Begin(enc, fileSize, limit)) {
        ::QueryPerformanceCounter(&t1);
        ::CloseHandle(h);
        if (error) *error = L"文件超过 4 GB，需分段建索引";
        return false;
      }
      begun = true;
    }
    out->Feed(buf.data(), got);
    ::QueryPerformanceCounter(&t1);
    sms += QpcMs(t0.QuadPart, t1.QuadPart);
    consumed += got;
  }

  ::CloseHandle(h);
  if (!begun) {
    if (error) *error = L"空文件";
    return false;
  }
  out->Finish();
  if (readMs) *readMs = rms;
  if (scanMs) *scanMs = sms;
  return true;
}

}  // namespace doc
