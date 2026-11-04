// doc_source.h - 当前预览文档的索引与块正文服务。
//
// 打开时同步扫前 8 MB 并发布一份可查询的快照（够首屏与滚动），大文件再起
// 后台线程扫完全文；扫完后换快照并通知 owner。WebResourceRequested 在宿主
// 线程上串行回调，Serve() 只做一次顺序读与解码，不做解析。

#pragma once

#include <windows.h>

#include <atomic>
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
// overrideCodePage 非 0 时按该代码页重开（查看器内的临时覆盖，不写 config.json）。
bool Open(HWND owner, const std::wstring& path, DocumentInfo* info, std::wstring* error,
          uint32_t overrideCodePage = 0);
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
// body：u32 total | u32 returned | u32 flags | (u32 block, u32 offUnits, u32 line)*。
// offUnits 是命中在块内解码文本里的 UTF-16 单元偏移（与 JS 字符串下标一致）；
// line 是块内行号（0 起，与 __lines 的 from 同口径），**只对巨型块有意义**，
// 普通块恒 0——前端只在巨型块上用它定位命中行。
// flags bit0 = 命中被截断（只返回导航上限内的条目）；
//       bit1 = 正则出错，帧尾跟一条 u32 长度 + UTF-8 消息（此时 returned=0）；
//       bit2 = 触发过 PCRE2 的匹配/深度上限，计数可能偏小（UI 在计数字尾显示 ?）。
// **SearchBody 要读整个文件，必须放在后台线程上跑**：WebResourceRequested 在宿主
// 线程上串行回调，直接扫 1 GB 会把输入与窗口消息一起卡住（webhost 只在拿不到
// deferral 的罕见情况下同步兜底）。
bool IsSearchUri(const std::wstring& uri);
bool SearchBody(const std::wstring& uri, std::string* body, int* status,
                const std::atomic<bool>* cancel);

// 响应 https://mdblocks.localhost/__outline：标题大纲。
// body：u32 total | u32 returned | u32 flags |
//       (u32 block, u8 level, u16 len, UTF-8 标题文本)*
bool ServeOutline(const std::wstring& uri, std::string* body, int* status);

// 响应 https://mdblocks.localhost/__defs：所有隐藏块（front matter 除外）的原文。
// 帧：u32 flags | u32 len | UTF-8 字节。链接引用定义与脚注定义都由前端消费，
// 总量上限 8 MB（超出就截断，定义极多的文档会丢尾部引用）。
bool ServeDefs(const std::wstring& uri, std::string* body, int* status);

// 响应 https://mdblocks.localhost/__asset?p=<相对路径>：读当前文档目录下的图片等资源。
// 只允许文档目录内的相对路径，任何绝对路径、盘符或 .. 段都拒绝。
bool ServeAsset(const std::wstring& uri, std::string* body, int* status,
                std::wstring* contentType);

// 把 https://mdblocks.localhost/__asset?p=<路径> 还原成磁盘路径，给右键菜单的
// 「打开方式」用。与 ServeAsset 同一套语义（文档目录内，或设置放行的目录外路径、
// 只认图片扩展名），但只还原路径、不读内容；路径非法或文件不存在时返回 false，
// 调用方只出原有全局菜单。
bool AssetUrlToPath(const std::wstring& url, std::wstring* out);

uint32_t PublishedBlockCount();
uint32_t LastVisible();

// 当前文档代号：每次 Open/Close 递增。WM_MDV_INDEX_FULL 的 wParam 就是发起扫描
// 时的代号，收到时比对一下就能丢掉"换文件瞬间"的过期通知。
uint32_t DocId();

// 把 https://mddoc.localhost/<相对路径> 还原成文档目录下的磁盘路径。
// 不是该主机的地址时返回 false（调用方按普通 URL 处理）。
bool DocUrlToPath(const std::wstring& url, std::wstring* out);

}  // namespace docsrc
