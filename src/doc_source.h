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

// 响应 https://mdblocks.local/__blocks?from=N&count=M。
// body 为二进制帧：u32 len | u32 blockIndex | u8 flags | UTF-8 字节。
bool Serve(const std::wstring& uri, std::string* body, int* status);

uint32_t PublishedBlockCount();

}  // namespace docsrc
