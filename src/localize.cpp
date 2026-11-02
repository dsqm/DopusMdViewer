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
    /* MeasureUnit      */ L"em（8–200）",
    /* Unlimited        */ L"不限宽",
    /* MeasureHint      */ L"行高与字号改变后，预览按当前锚点恢复位置。",

    /* Slot             */ L"槽位",
    /* SlotUi           */ L"界面",
    /* SlotBody         */ L"正文",
    /* SlotMono         */ L"等宽",
    /* Family           */ L"字体",
    /* FamilyAdd        */ L"添加",
    /* FontAdd          */ L"添加字体…",
    /* FontFilterCue    */ L"输入以筛选",
    /* FontUseInput     */ L"使用「%s」",
    /* Chain            */ L"回退链",
    /* ChainUp          */ L"上移",
    /* ChainDown        */ L"下移",
    /* ChainDel         */ L"删除",
    /* Preview          */ L"预览",
    /* RefreshFonts     */ L"刷新字体列表",
    /* PreviewSample    */ L"中文 English 123 — 字体预览",
    /* PreviewMissing   */ L"未找到：%s（仍可保存，系统会回退）",
    /* PreviewInherited */ L"正文未设置，跟随界面字体",

    /* RenderGroupContent */ L"内容渲染",
    /* RenderGroupUi    */ L"界面控件",
    /* RenderGroupImages */ L"外部图片",
    /* RenderMermaid    */ L"Mermaid 图表",
    /* RenderKatex      */ L"KaTeX 公式",
    /* RenderHighlight  */ L"代码语法高亮",
    /* RenderHtml       */ L"内联原始 HTML",
    /* RenderLocal      */ L"本地相对图片",
    /* RenderCopyCode   */ L"代码块复制按钮",
    /* RenderCopyTable  */ L"表格复制按钮",
    /* RenderCopyFormat */ L"表格复制格式",
    /* RenderFindButton */ L"显示搜索按钮",
    /* RenderOutlineButton */ L"显示大纲按钮",
    /* RenderLineNumbers */ L"代码块行号",
    /* CodeBlockGroup   */ L"代码块",
    /* RenderCodeWhitespace */ L"显示空白符",
    /* CodeWsBoundary   */ L"边界",
    /* CodeWsAll        */ L"全部",
    /* CodeWsSelected   */ L"仅选中",
    /* RenderIndentedBlocks */ L"扩展至缩进式代码块",
    /* CopyFormatTsv    */ L"TSV 纯文本",
    /* CopyFormatMarkdown */ L"Markdown 源码",
    /* NetAskWhen       */ L"网络图片加载前：",
    /* NetPerDoc        */ L"每文档询问一次",
    /* NetPerHost       */ L"每主机询问一次（DO 运行期间记住）",
    /* NetNever         */ L"不询问，直接加载",
    /* OutsideImages    */ L"允许图片引用文档目录外的路径（../ 与绝对路径）",

    /* Version          */ L"版本",
    /* Prewarm          */ L"启动时预热预览引擎（Opus 窗口关闭时释放）",
    /* IdleRelease      */ L"关闭预热后空闲释放",
    /* IdleUnit         */ L"分钟（0 = 不释放）",
    /* LogEnabled       */ L"记录日志",
    /* OpenLog          */ L"打开日志目录",
    /* OpenConfig       */ L"打开配置目录",
    /* OpenSkins        */ L"打开皮肤目录",
    /* ConfigFolder     */ L"配置目录",
    /* ConfigFolderHelp */
    L"默认位置在 Opus 的用户数据目录，随 Opus 配置一起备份。\r\n\r\n"
    L"更换位置：在「设置 → 文件夹别名（Folder Aliases）」中新建名为 %s 的别名，"
    L"指向目标文件夹。重启 Opus 后，配置、皮肤和日志都会改用该文件夹。\r\n\r\n"
    L"目标文件夹必须可写，不要指向需要管理员权限的文件夹（例如 Program Files）。"
    L"\r\n\r\n当前配置文件夹：\r\n%s",
    /* WebViewCache     */ L"WebView2 缓存目录",
    /* WebViewBrowse    */ L"浏览…",
    /* WebViewOpen      */ L"打开缓存目录",
    /* WebViewDefault   */ L"恢复默认目录",
    /* WebViewClear     */ L"清理缓存",
    /* WebViewCacheInfo */ L"缓存 %s；超过 %s 自动清理；修改后重启 Opus 生效",
    /* WebViewDirHelp */
    L"留空时使用默认位置：\r\n%s\r\n\r\n"
    L"实际 WebView2 目录 = 所选位置 + \\WebView2：\r\n%s\r\n\r\n"
    L"缓存可再生，超过 %s 会在下次创建预览引擎前自动清理；也可以点「清理缓存」手动删除。\r\n"
    L"修改位置不会搬移或删除旧位置的缓存。\r\n"
    L"不要选择同步盘、网络盘或备份目录。修改后重启 Opus 生效。",
    /* WebViewClearConfirm   */ L"将删除 WebView2 缓存，下次预览会重新生成。继续？",
    /* WebViewClearCloseFirst */ L"预览引擎正在使用缓存，请先关闭所有预览窗口，再清理缓存。",
    /* WebViewClearFailed    */ L"缓存清理失败：目录仍被占用，请关闭所有预览窗口后重试。",
    /* WebViewPathInvalid    */ L"缓存位置必须是本地绝对路径，且目录可写。",
    /* Copy             */ L"复制",
    /* CopyAlias        */ L"复制别名",
    /* Copied           */ L"已复制",
    /* CopyPath         */ L"复制路径",
    /* Unknown          */ L"未知",
    /* RuntimeMissing   */ L"未安装或不可用",

    /* CopyVersionInfo  */ L"复制版本信息",
    /* PrewarmHelp */
    L"作用：Opus 启动时就在后台把预览引擎和页面拉起来。之后打开第一个 Markdown 文件不用等"
    L"引擎初始化，首屏一般能快 0.3～1 秒。\r\n\r\n"
    L"代价：常驻一个预览进程（约 30～60 MB），即使从不看 Markdown 它也在后台待着。\r\n\r\n"
    L"适合：经常用预览窗格看文档、机器内存充裕。\r\n"
    L"不适合：很少看 Markdown，或在内存吃紧的机器上。关掉后由下面「空闲释放」在不用时回收。",
    /* ResetPage        */ L"重置本页",
    /* ResetAllTitle    */ L"重置全部设置",
    /* ResetAllText */
    L"所有页签的设置都会恢复默认值。\r\n\r\n"
    L"确定后仍需点「应用」或「确定」才会写入配置文件。",
    /* MeasureHintUnlimited */ L"不限宽：正文宽度随窗口自适应，不再按 em 限制。",
    /* RenderHtmlHint   */ L"关闭后，正文里的 HTML 标签按原文显示，不会被解释成元素。",
    /* Restore          */ L"重置全部",
    /* Apply            */ L"应用",
    /* Ok               */ L"确定",
    /* Cancel           */ L"取消",

    /* SaveFailed       */ L"无法保存设置。",
    /* SaveFailedPath   */ L"路径：%s",
    /* SaveFailedError  */ L"错误：%lu - %s",
    /* SaveFailedNoPath */ L"路径：不可用（Opus 的用户数据目录没解析出来）",
    /* SaveAdviceMissing */
    L"建议：配置目录建不出来，可能被清理软件删掉了上级目录，或被 Folder Alias 指到了不存在的盘。"
    L"请检查「高级」页里的目录位置。",
    /* SaveAdviceDenied */
    L"建议：没有写入权限，或被安全软件拦住。检查该目录的属性，或在「高级」页把数据目录换到别处。",
    /* SaveAdviceBusy */
    L"建议：文件正被其他程序占用（常见于杀毒软件扫描、网盘同步）。稍后重试。",
    /* SaveAdviceDisk   */ L"建议：清理磁盘空间后重试。",
    /* SaveAdviceOther  */ L"建议：请检查该目录是否可写；若反复出现，把这段信息截图发给作者。",
    /* NoFolder         */ L"目录路径不可用。",
    /* OpenFolderFailed */ L"打开目录失败。",
};

static_assert(static_cast<int>(Str::Skin) == 5, "字符串表顺序必须与 localize.h 的 Str 一致");
static_assert(sizeof(kZh) / sizeof(kZh[0]) == kCount,
              "kZh 少了条目：漏配的 key 会显示成空串");

}  // namespace

const wchar_t* T(Str id) {
  const int i = static_cast<int>(id);
  if (i < 0 || i >= kCount) return L"";
  return kZh[i];
}

}  // namespace loc
