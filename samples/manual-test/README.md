# 手动验收样本（DopusMdViewer）

在 Opus 里按顺序打开，每个文件开头都写了预期。

| 文件 | 看什么 |
|---|---|
| 01-syntax.md | GFM 排版、代码高亮、任务列表、定义列表、内联 HTML、front matter 隐藏；页内锚点（`#重复标题-1` 指向第二个重名标题） |
| 02-refs-footnotes.md | 跨块引用 `[x][ref]`、脚注角标与文末脚注区、未定义项保持原文 |
| 03-images.md | 本地图（同目录/子目录）、`../` 与绝对路径默认被拒、data URI、网络图页内确认 |
| 04-math-mermaid.md | KaTeX 行内/独立公式、Mermaid 两种图、未知图降级 |
| 05/06/07-encoding-*.md | GB18030 / UTF-16LE / UTF-8 BOM 三种编码同一内容 |
| gen-enc.py → `%TEMP%\encsamples\` | 多编码判定样本（日/韩/繁/俄/西欧、无 BOM UTF-16、UTF-32 BOM、8 MB 探测边界）；用 `bin\encdetect.exe` 批跑。不入库，`very-late-sjis.md` 约 10 MB |
| 08-outside-images.md | 设置开关前后的目录外图片（`../` 与绝对路径） |
| big-mixed-3000.md | 普通块与 3000 行围栏混排，进出巨型块的滚动衔接 |
| big-fence-120000.md | 12 万行单围栏：窗口化滚动、第 10 万行处的分段接缝 |
| big-table-3000.md | 已知取舍：巨型表格按纯文本显示 |

网络图确认受 `config.json` 的 `netImageConfirm` 影响；测完之后可在设置里切回默认。

交互：`Ctrl+A` 全选（视觉上高亮已载入的块），`Ctrl+C` 复制渲染后的纯文本（1 MB 级文档完整复制，更大的文档只复制前缀并在状态栏提示“文档过大”）；普通选区仍按浏览器原生方式复制。拖选跨块时选区不塌陷，拖到视口上下边缘会自动滚动延伸选区。打印（查看器右键/菜单，若 Opus 提供）会先把全文渲染进打印视图再弹 WebView2 打印对话框，>2 MB 的文档不支持。
