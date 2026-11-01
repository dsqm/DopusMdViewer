// localize.h - 界面文案的字符串表（票 09：只做中文，但文案一律走 T(StrId)）。
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
  NetAskWhen,
  NetPerDoc,
  NetPerHost,
  NetNever,
  OutsideImages,

  // 高级
  Version,
  GiantBlock,
  GiantBlockNote,
  ResidentWindow,
  ResidentNote,
  Prewarm,
  IdleRelease,
  IdleUnit,
  OpenLog,
  OpenConfig,
  OpenSkins,
  ConfigFolder,
  ConfigFolderHelp,  // 两个 %s：别名名、当前路径
  CopyAlias,
  Copied,
  Unknown,
  RuntimeMissing,

  // 底部按钮
  Restore,
  Apply,
  Ok,
  Cancel,

  // 提示
  SaveFailed,
  NoFolder,
  OpenFolderFailed,
};

// 返回当前语言的文案；未收录的 id 返回空串（便于一眼看出漏配）。
const wchar_t* T(Str id);

}  // namespace loc
