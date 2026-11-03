// msg.h - C++ 与 shell.html 之间的控制消息。
//
// 不走 JSON：手写「类型 + 竖线分隔字段 + 载荷放最后」的定长头格式。
// 载荷永远在最后一个字段，所以正文与 CSS 里出现竖线不会把解析打乱。
// 块正文走二进制帧（doc_source.cpp），这里只承载小的控制消息。

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

// index|<docBase>|<prefixBlocks>|<estTotal>|<fileSize>|<encoding>|<lastVisible>|<complete>
std::wstring MakeIndex(const std::wstring& docBase, uint32_t prefixBlocks,
                       uint32_t estTotal, uint64_t fileSize, int encoding,
                       uint32_t lastVisible, bool complete);

// index-full|<blockCount>|<lastVisible>：后台全文索引完成。
std::wstring MakeIndexFull(uint32_t blockCount, uint32_t lastVisible);

// big|<i>:<lines>,<i>:<lines>…：巨型块行数表（只有 > 2000 行的块）。
std::wstring MakeBig(const std::string& table);

// net|<mode>：网络图片确认粒度（0 不询问 / 1 每主机一次 / 2 每文档一次）。
// 两个档位的答复都只对当前文档有效，换文件全部重问。
// 确认面板在页面内渲染，这里只推配置。
std::wstring MakeNetMode(int mode);

// cfg|<mermaid>|<katex>|<highlight>|<inlineHtml>|<localImages>|<copyCode>|<copyTable>
//    |<tableCopyMd>|<findBtn>|<outlineBtn>|<lineNumbers>|<wsMode>|<indentedBlocks>
//    |<tableWrap>：
//    渲染开关（1/0）。wsMode 是数字：0 = 关，1 = 仅选中，2 = 边界，3 = 全部。
// 前端按固定下标取字段，新字段只能追加在末尾；取不到时按「开」处理
// （wsMode 取不到时按 0，即关——老宿主本来就没有这个功能）。
std::wstring MakeCfg(const Config& config);

// clear：没有字段，只有类型名本身（页面按 type === 'clear' 匹配）。
std::wstring MakeClear();

// docerror|<文本>：打开失败。页面清空旧内容并居中显示原因；文本是最后一个字段，
// 可以包含竖线与换行。
std::wstring MakeDocError(const std::wstring& text);

// 解析一条来自页面的消息：切出第一个竖线前的类型，其余原样给出。
bool SplitType(const std::wstring& frame, std::wstring* type, std::wstring* rest);

// "event|rendered|12.3" 之类的第二个字段（没有则返回空串）。
std::wstring SecondField(const std::wstring& rest);

}  // namespace msg
