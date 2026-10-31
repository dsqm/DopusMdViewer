// msg.h - C++ 与 shell.html 之间的控制消息（票 11 第 5 节）。
//
// 不走 JSON：手写「类型 + 竖线分隔字段 + 载荷放最后」的定长头格式。
// 载荷永远在最后一个字段，所以正文与 CSS 里出现竖线不会把解析打乱。
// M4 起块正文走二进制帧（doc_source.cpp），这里只承载小的控制消息。

#pragma once

#include <cstdint>
#include <string>

#include "config.h"

namespace msg {

// 有序回退链 → CSS font-family 值；含空格的族名加引号。
std::wstring FontStackToCss(const FontStack& stack);

// skin|<id>|<css>
std::wstring MakeSkin(const std::wstring& id, const std::wstring& css);

// fx|<ui>|<body>|<mono>|<size>|<lineHeight>|<measureEm>
std::wstring MakeFx(const FontSettings& font);

// index|<docBase>|<prefixBlocks>|<estTotal>|<fileSize>|<encoding>
std::wstring MakeIndex(const std::wstring& docBase, uint32_t prefixBlocks,
                       uint32_t estTotal, uint64_t fileSize, int encoding);

// index-full|<blockCount>：后台全文索引完成。
std::wstring MakeIndexFull(uint32_t blockCount);

// netimg-allow|<origin> / netimg-deny|<origin>
std::wstring MakeNetImageReply(const std::wstring& origin, bool allow);

// clear|
std::wstring MakeClear();

// 解析一条来自页面的消息：切出第一个竖线前的类型，其余原样给出。
bool SplitType(const std::wstring& frame, std::wstring* type, std::wstring* rest);

// "event|rendered|12.3" 之类的第二个字段（没有则返回空串）。
std::wstring SecondField(const std::wstring& rest);

}  // namespace msg
