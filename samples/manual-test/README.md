# 手动验收样本（DopusMdViewer）

在 DOpus 里按顺序打开，每个文件开头都写了预期。

| 文件 | 看什么 |
|---|---|
| 01-syntax.md | GFM 排版、代码高亮、任务列表、定义列表、内联 HTML、front matter 隐藏；页内锚点（`#重复标题-1` 指向第二个重名标题） |
| 02-refs-footnotes.md | 跨块引用 `[x][ref]`、脚注角标与文末脚注区、未定义项保持原文 |
| 03-images.md | 本地图（同目录/子目录）、`../` 与绝对路径默认被拒、data URI、网络图页内确认 |
| 04-math-mermaid.md | KaTeX 行内/独立公式、Mermaid 两种图、未知图降级 |
| 05/06/07-encoding-*.md | GB18030 / UTF-16LE / UTF-8 BOM 三种编码同一内容 |
| gen-enc.py → `%TEMP%\encsamples\` | 多编码判定样本（日/韩/繁/俄/西欧、无 BOM UTF-16、UTF-32 BOM、8 MB 探测边界）；用 `build\tools\encdetect.exe` 批跑。不入库，`very-late-sjis.md` 约 10 MB |
| 10-search-regex.md | 正则：日期 10 处、二级标题 4 处、`Error` 大小写 4/1 处、非法正则的提示 |
| 11-search-word.md | 整词与折叠：`cat` 7→3 处、`CAFÉ` 3/1 处、`中文` 3→2 处 |
| gen-search.py → `%TEMP%\mdvsearch\` | 巨型块命中定位（`big-fence-hits.md`，命中在围栏内第 1001/90001 行与末行）与多块拖选（`mid-blocks.md`）。不入库 |
| CHECK.md | 装机验收清单：每一项的样本、查询词、开关与精确预期 |
| 08-outside-images.md | 设置开关前后的目录外图片（`../` 与绝对路径） |
| 12-table-cells.md | 表格单元格与跨块表尾（CHECK.md 第九节） |
| 14-table-width.md | 表格宽度：短表收窄、长 URL 折行、12 列横向滚动、引用/列表/提示框嵌套、`<br>` 换行 |
| 15-image-zoom.md | 图片左键放大遮罩、右键图片动作区（CHECK.md 第十六节） |
| 16-open-in-dopus-viewer.md | 图片「打开方式」改走 DOpus 查看器（CHECK.md 第十七节） |
| big-mixed-3000.md | 普通块与 3000 行围栏混排，进出巨型块的滚动衔接 |
| big-fence-120000.md | 12 万行单围栏：窗口化滚动、第 10 万行处的分段接缝 |
| big-table-3000.md | 巨型表格按真表格显示（2 万行以内） |

样本编号不连续：`09` 没有对应文件；`13-nested-tables.md` 是本地临时样本，不在验收清单内。
`big-*` 由 `gen.py` 生成，不入库。

网络图确认受 `config.json` 的 `netImageConfirm` 影响；测完之后可在设置里切回默认。

交互：`Ctrl+A` 全选（视觉上高亮已载入的块），`Ctrl+C` 复制渲染后的纯文本（1 MB 级文档完整复制，更大的文档只复制前缀并在状态栏提示“文档过大”）；普通选区仍按浏览器原生方式复制。拖选跨块时选区不塌陷，拖到视口上下边缘会自动滚动延伸选区。打印（查看器右键/菜单，若 DOpus 提供）会先把全文渲染进打印视图再弹 WebView2 打印对话框，>2 MB 的文档不支持。
