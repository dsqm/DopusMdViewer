// pdfmerge.h - 把多份 Chromium 打印出来的 PDF 合成一份。
//
// WebView2 没有「向已有 PDF 追加页」的接口：一次 PrintToPdfStream 只能产出一份
// 完整文件，而整篇一次性打印在 5 MB 档就长时间不返回。所以大文档只能先分段生成，
// 再把结果拼成一份。Chromium 的输出是 PDF 1.4、传统 xref 表、不带对象流，
// 因此按对象块解析、重编号、重建交叉引用即可，不需要引入第三方 PDF 库。
#pragma once

#include <string>
#include <vector>

// 按 parts 的顺序合并到 outPath（已存在则覆盖）。失败返回 false 并写 err。
// 只处理传统 xref 表：碰到对象流或加密的输入直接失败，不做猜测。
bool PdfMergeFiles(const std::vector<std::wstring>& parts, const std::wstring& outPath,
                   std::wstring* err);
