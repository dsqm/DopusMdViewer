// localize.h - 界面文案的字符串表（只做中文，但文案一律走 T(StrId)）。
//
// 起点是设置窗口；以后加语言就是 localize.cpp 里再放一张表，调用方不用改。
// 不放语言文件：文案跟着 DLL 走，用户目录里多一个文件只会多一条出错路径。

#pragma once

namespace loc {

enum class Str : int {
  // 窗口与页签
  SettingsTitle,
  TabAppearance,
  TabFont,
  TabRender,
  TabAdvanced,

  // 外观
  Skin,
  FontSize,
  FontSizeUnit,
  LineHeight,
  LineHeightUnit,
  Measure,
  MeasureUnit,
  Unlimited,
  MeasureHint,

  // 字体
  Slot,
  SlotUi,
  SlotBody,
  SlotMono,
  Family,
  FamilyAdd,
  Chain,
  ChainUp,
  ChainDown,
  ChainDel,
  Preview,
  RefreshFonts,
  PreviewSample,
  PreviewMissing,   // 带一个 %s：字体名
  PreviewInherited,

  // 渲染
  RenderMermaid,
  RenderKatex,
  RenderHighlight,
  RenderHtml,
  RenderLocal,
  RenderCopyCode,
  RenderCopyTable,
  RenderCopyFormat,
  RenderFindButton,
  RenderOutlineButton,
  RenderLineNumbers,
  CodeBlockGroup,
  RenderCodeWhitespace,
  CodeWsRange,
  CodeWsBoundary,
  CodeWsAll,
  RenderIndentedBlocks,
  CopyFormatTsv,
  CopyFormatMarkdown,
  NetAskWhen,
  NetPerDoc,
  NetPerHost,
  NetNever,
  OutsideImages,

  // 高级
  Version,
  Prewarm,
  IdleRelease,
  IdleUnit,
  LogEnabled,
  OpenLog,
  OpenConfig,
  OpenSkins,
  ConfigFolder,
  ConfigFolderHelp,  // 两个 %s：别名名、当前路径
  WebViewCache,
  WebViewBrowse,
  WebViewOpen,
  WebViewDefault,
  WebViewClear,
  WebViewCacheInfo,  // 两个 %s：当前大小、自动清理上限
  WebViewDirHelp,    // 三个 %s：默认目录、实际目录、清理上限
  WebViewClearConfirm,
  WebViewClearCloseFirst,
  WebViewClearFailed,
  WebViewPathInvalid,
  CopyAlias,
  Copied,
  CopyPath,
  Unknown,
  RuntimeMissing,

  // 底部按钮
  Restore,
  Apply,
  Ok,
  Cancel,

  // 提示
  SaveFailed,
  SaveFailedPath,     // 一个 %s：目标文件
  SaveFailedError,    // 一个 %lu、一个 %s：错误码、系统消息
  SaveFailedNoPath,
  SaveAdviceMissing,
  SaveAdviceDenied,
  SaveAdviceBusy,
  SaveAdviceDisk,
  SaveAdviceOther,
  NoFolder,
  OpenFolderFailed,
};

// 返回当前语言的文案；未收录的 id 返回空串（便于一眼看出漏配）。
const wchar_t* T(Str id);

}  // namespace loc
