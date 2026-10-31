// doc_source.h - 当前预览文档的索引与块正文服务（M4）。
//
// 打开时同步扫前 8 MB 并发布一份可查询的快照（够首屏与滚动），大文件再起
// 后台线程扫完全文；扫完后换快照并通知 owner。WebResourceRequested 在宿主
// 线程上串行回调，Serve() 只做一次顺序读与解码，不做解析。

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// 后台全量索引完成（wParam = 打开时的文档代号）。
#define WM_MDV_INDEX_FULL (WM_APP + 0x310)

namespace docsrc {

struct DocumentInfo {
  uint32_t prefixBlocks = 0;
  uint32_t estimatedBlocks = 0;
  uint32_t lastVisible = 0;  // 最后一个非隐藏块（脚注区挂在它后面）
  uint64_t fileSize = 0;
  uint8_t encoding = 0;  // doc::Encoding
  bool complete = false;  // 前缀已覆盖全文，没有后台扫描
};

// 打开文档并同步扫前缀。失败时返回 false，error 给一行原因。
bool Open(HWND owner, const std::wstring& path, DocumentInfo* info, std::wstring* error);
void Close();
bool IsOwner(HWND owner);
HWND Owner();
void CloseIfOwner(HWND owner);

// 响应 https://mdblocks.localhost/__blocks?from=N&count=M。
// body 为二进制帧：u32 len | u32 blockIndex | u8 flags | UTF-8 字节。
bool Serve(const std::wstring& uri, std::string* body, int* status);

// 响应 https://mdblocks.localhost/__lines?i=<块号>&from=<行>&count=<行数>。
// 巨型块（> 2000 行）的块内行窗口，body 为：
//   u32 blockIndex | u32 from | u32 got | u32 totalLines(0=未知) | u32 len | UTF-8 字节
// 行按源文件顺序连续切分；每 64 行一个 checkpoint 缓存，跳读不必从头扫。
bool ServeLines(const std::wstring& uri, std::string* body, int* status);

// 巨型块表："块号:行数,块号:行数…"（只含 > 2000 行的块，最多 4096 项）。
void BigTable(std::string* out);

// 响应 https://mdblocks.localhost/__search?q=<utf8>&cs=<0|1>：页内搜索。
// body：u32 total | u32 returned | u32 flags | (u32 block, u32 offUnits)*。
// offUnits 是命中在块内解码文本里的 UTF-16 单元偏移（与 JS 字符串下标一致）。
// flags bit0 = 命中被截断（只返回导航上限内的条目）。
bool ServeSearch(const std::wstring& uri, std::string* body, int* status);

// 响应 https://mdblocks.localhost/__outline：标题大纲。
// body：u32 total | u32 returned | u32 flags |
//       (u32 block, u8 level, u16 len, UTF-8 标题文本)*
bool ServeOutline(const std::wstring& uri, std::string* body, int* status);

// 响应 https://mdblocks.localhost/__defs：所有隐藏块（front matter 除外）的原文。
// 帧：u32 flags | u32 len | UTF-8 字节。链接引用定义与脚注定义都由前端消费，
// 总量上限 8 MB（超出就截断，定义极多的文档会丢尾部引用）。
bool ServeDefs(const std::wstring& uri, std::string* body, int* status);

// 按块顺序拼接可见正文（块间空行，隐藏块跳过），最多 maxSourceBytes 源字节。
// truncated = 达到上限被截断；complete = 当前索引已覆盖全文。
bool CopyText(uint64_t maxSourceBytes, std::wstring* text, bool* truncated, bool* complete);

// 响应 https://mdblocks.localhost/__asset?p=<相对路径>：读当前文档目录下的图片等资源。
// 只允许文档目录内的相对路径，任何绝对路径、盘符或 .. 段都拒绝。
bool ServeAsset(const std::wstring& uri, std::string* body, int* status,
                std::wstring* contentType);

uint32_t PublishedBlockCount();
uint32_t LastVisible();

}  // namespace docsrc
