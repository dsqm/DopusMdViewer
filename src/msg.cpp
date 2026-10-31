#include "msg.h"

#include <cstdio>
#include <cwchar>
#include <vector>

namespace msg {

namespace {

const wchar_t kSep = L'|';

// 字体族名进 CSS 前先掐掉能改变声明结构的字符；族名里本来不会出现这些。
std::wstring CleanFamily(const std::wstring& name) {
  std::wstring out;
  out.reserve(name.size());
  for (wchar_t ch : name) {
    if (ch < 0x20) continue;
    if (ch == L'"' || ch == L'\'' || ch == L';' || ch == L'{' || ch == L'}' || ch == kSep) continue;
    out += ch;
  }
  return out;
}

std::wstring Join(const std::vector<std::wstring>& fields) {
  std::wstring out;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) out += kSep;
    out += fields[i];
  }
  return out;
}

std::wstring FormatDouble(double v) {
  wchar_t szBuf[32];
  // 两位小数足够表达行高倍数，且避免 "1.6500000000000001"
  ::swprintf_s(szBuf, sizeof(szBuf) / sizeof(szBuf[0]), L"%.2f", v);
  return szBuf;
}

}  // namespace

std::wstring FontStackToCss(const FontStack& stack) {
  std::wstring out;
  for (size_t i = 0; i < stack.size(); ++i) {
    const std::wstring name = CleanFamily(stack[i]);
    if (name.empty()) continue;
    if (!out.empty()) out += L", ";
    if (name.find(L' ') != std::wstring::npos) {
      out += L'"';
      out += name;
      out += L'"';
    } else {
      out += name;
    }
  }
  return out;
}

std::wstring MakeSkin(const std::wstring& id, const std::wstring& css) {
  return Join({L"skin", id, css});
}

std::wstring MakeFx(const FontSettings& font) {
  const std::wstring ui = FontStackToCss(font.ui);
  // 正文槽位为空时解析成界面槽位的实际值再写出，不留继承给皮肤（票 07 第 4 节）
  const std::wstring body = font.body.empty() ? ui : FontStackToCss(font.body);
  const std::wstring mono = FontStackToCss(font.mono);

  return Join({L"fx", ui, body, mono, std::to_wstring(font.sizePx), FormatDouble(font.lineHeight),
               std::to_wstring(font.measureEm)});
}

std::wstring MakeIndex(const std::wstring& docBase, uint32_t prefixBlocks,
                       uint32_t estTotal, uint64_t fileSize, int encoding,
                       uint32_t lastVisible) {
  return Join({L"index", docBase, std::to_wstring(prefixBlocks), std::to_wstring(estTotal),
               std::to_wstring(fileSize), std::to_wstring(encoding),
               std::to_wstring(lastVisible)});
}

std::wstring MakeIndexFull(uint32_t blockCount, uint32_t lastVisible) {
  return Join({L"index-full", std::to_wstring(blockCount), std::to_wstring(lastVisible)});
}

std::wstring MakeBig(const std::string& table) {
  std::wstring wide;
  wide.reserve(table.size());
  for (char c : table) wide += static_cast<wchar_t>(static_cast<unsigned char>(c));
  return Join({L"big", wide});
}

std::wstring MakeNetMode(int mode) {
  return Join({L"net", std::to_wstring(mode)});
}

std::wstring MakeClear(void) { return L"clear"; }

bool SplitType(const std::wstring& frame, std::wstring* type, std::wstring* rest) {
  const size_t pos = frame.find(kSep);
  if (pos == std::wstring::npos) {
    if (type) *type = frame;
    if (rest) rest->clear();
    return !frame.empty();
  }
  if (type) *type = frame.substr(0, pos);
  if (rest) *rest = frame.substr(pos + 1);
  return true;
}

std::wstring SecondField(const std::wstring& rest) {
  const size_t pos = rest.find(kSep);
  if (pos == std::wstring::npos) return rest;
  return rest.substr(0, pos);
}

}  // namespace msg
