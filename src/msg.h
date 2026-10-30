// msg.h - C++ 与 shell.html 之间的控制消息（票 11 第 5 节）。
//
// 不走 JSON：手写「类型 + 竖线分隔字段 + 载荷放最后」的定长头格式。
// 载荷永远在最后一个字段，所以正文与 CSS 里出现竖线不会把解析打乱。
// 块正文在 M4 改走二进制帧（webres.cpp），这里只承载小载荷与 M1 的整篇临时路径。

#pragma once

#include <string>

#include "config.h"

namespace msg {

// 有序回退链 → CSS font-family 值；含空格的族名加引号。
std::wstring FontStackToCss(const FontStack& stack);

// skin|<id>|<css>
std::wstring MakeSkin(const std::wstring& id, const std::wstring& css);

// fx|<ui>|<body>|<mono>|<size>|<lineHeight>|<measureEm>
std::wstring MakeFx(const FontSettings& font);

// doc|<docBase>|<text>
std::wstring MakeDoc(const std::wstring& docBase, const std::wstring& text);

// 解析一条来自页面的消息：切出第一个竖线前的类型，其余原样给出。
bool SplitType(const std::wstring& frame, std::wstring* type, std::wstring* rest);

// "event|rendered|12.3" 之类的第二个字段（没有则返回空串）。
std::wstring SecondField(const std::wstring& rest);

}  // namespace msg
