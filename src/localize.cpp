// localize.cpp - 中文文案表。表用 index 直达，不查字典：几百个字符串，
// 直线查表比任何容器都省事，漏配时返回空串。

#include "localize.h"

namespace loc {
namespace {

constexpr int kCount = static_cast<int>(Str::OpenFolderFailed) + 1;

const wchar_t* const kZh[kCount] = {
    /* SettingsTitle    */ L"Markdown 查看器设置",
    /* TabAppearance    */ L"外观",
    /* TabFont          */ L"字体",
    /* TabRender        */ L"渲染",
    /* TabAdvanced      */ L"高级",

    /* Skin             */ L"皮肤",
    /* FontSize         */ L"字号",
    /* FontSizeUnit     */ L"px（8–48）",
    /* LineHeight       */ L"行高",
    /* LineHeightUnit   */ L"倍（1.20–2.20，步进 0.05）",
    /* Measure          */ L"行宽",
    /* MeasureUnit      */ L"em（8–80）",
    /* Unlimited        */ L"不限宽",
    /* MeasureHint      */ L"行高与字号改变后，预览按当前锚点恢复位置。",

    /* Slot             */ L"槽位",
    /* SlotUi           */ L"界面",
    /* SlotBody         */ L"正文",
    /* SlotMono         */ L"等宽",
    /* Family           */ L"字体",
    /* FamilyAdd        */ L"添加",
    /* Chain            */ L"回退链",
    /* ChainUp          */ L"上移",
    /* ChainDown        */ L"下移",
    /* ChainDel         */ L"删除",
    /* Preview          */ L"预览",
    /* RefreshFonts     */ L"刷新字体列表",
    /* PreviewSample    */ L"中文 English 123 — 字体预览",
    /* PreviewMissing   */ L"未找到：%s（仍可保存，系统会回退）",
    /* PreviewInherited */ L"正文未设置，跟随界面字体",

    /* RenderMermaid    */ L"Mermaid 图表",
    /* RenderKatex      */ L"KaTeX 公式",
    /* RenderHighlight  */ L"代码语法高亮",
    /* RenderHtml       */ L"内联原始 HTML",
    /* RenderLocal      */ L"本地相对图片",
    /* NetAskWhen       */ L"网络图片加载前：",
    /* NetPerDoc        */ L"每文档询问一次",
    /* NetPerHost       */ L"每主机询问一次（DO 运行期间记住）",
    /* NetNever         */ L"不询问，直接加载",
    /* OutsideImages    */ L"允许图片引用文档目录外的路径（../ 与绝对路径）",

    /* Version          */ L"版本",
    /* GiantBlock       */ L"巨型块",
    /* GiantBlockNote   */ L"超过 2000 行或 256 KiB 时按纯文本窗口化",
    /* ResidentWindow   */ L"常驻窗口",
    /* ResidentNote     */ L"视口上下各 120 块",
    /* Prewarm          */ L"启动时预热预览引擎（Opus 窗口关闭时释放）",
    /* IdleRelease      */ L"关闭预热后空闲释放",
    /* IdleUnit         */ L"分钟（0 = 不释放）",
    /* OpenLog          */ L"打开日志目录",
    /* OpenConfig       */ L"打开配置根目录",
    /* OpenSkins        */ L"打开皮肤目录",
    /* ConfigFolder     */ L"配置文件夹",
    /* ConfigFolderHelp */
    L"默认位置在 Opus 的用户数据目录，随 Opus 配置一起备份。\r\n\r\n"
    L"更换位置：在「设置 → 文件夹别名（Folder Aliases）」中新建名为 %s 的别名，"
    L"指向目标文件夹。重启 Opus 后，配置、皮肤、日志和预览缓存都会改用该文件夹。\r\n\r\n"
    L"目标文件夹必须可写，不要指向需要管理员权限的文件夹（例如 Program Files）。"
    L"\r\n\r\n当前配置文件夹：\r\n%s",
    /* CopyAlias        */ L"复制别名",
    /* Copied           */ L"已复制",
    /* Unknown          */ L"未知",
    /* RuntimeMissing   */ L"未安装或不可用",

    /* Restore          */ L"恢复默认",
    /* Apply            */ L"应用",
    /* Ok               */ L"确定",
    /* Cancel           */ L"取消",

    /* SaveFailed       */ L"写入 config.json 失败，详见日志。",
    /* NoFolder         */ L"目录路径不可用。",
    /* OpenFolderFailed */ L"打开目录失败。",
};

static_assert(static_cast<int>(Str::Skin) == 5, "字符串表顺序必须与 localize.h 的 Str 一致");

}  // namespace

const wchar_t* T(Str id) {
  const int i = static_cast<int>(id);
  if (i < 0 || i >= kCount) return L"";
  return kZh[i];
}

}  // namespace loc
