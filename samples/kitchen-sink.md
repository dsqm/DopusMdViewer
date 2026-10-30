---
title: kitchen sink
tags: [gfm, 语法边界]
---

# 语法边界清单

覆盖 GFM 核心 + 脚注 + 定义列表 + front matter + Mermaid + 内联原始 HTML。

## 强调与标记

**加粗**、*倾斜*、~~删除~~、`行内代码`、==高亮==、H~2~O、x^2^。

## 任务列表

- [ ] 未完成
- [x] 已完成
  - [x] 嵌套完成
  - [ ] 嵌套未完成

## 有序列表起始值与松散列表

3. 从三开始
4. 接着四

- 松散项一

- 松散项二

## 表格

| 左对齐 | 居中 | 右对齐 |
| :-- | :-: | --: |
| a | b | 1 |
| 长一些的单元格 | **粗体** | 1,234.50 |

## 引用嵌套

> 第一层
> > 第二层
>
> 回到第一层

## 脚注

正文里引用一次[^note]，再引用同一个[^note]，以及另一个[^other]。

[^note]: 第一条脚注，带 `代码`。
[^other]: 第二条脚注。

## 定义列表

markdown-it
: 一个 Markdown 解析器。

texmath
: 把 TeX 公式接进 markdown-it 的插件。

## 内联原始 HTML

<details>
<summary>折叠区（summary 可见，内容默认收起）</summary>

被折叠的正文。

</details>

<span style="color:#b00">一个带内联样式的 span</span>

## 硬换行与转义

第一行以两个空格结尾  
第二行。

反斜杠转义：\*不是斜体\*。

## 长行

这是一条很长的段落，用来确认容器的换行与行宽上限是否按 em 生效，同时确认在窄窗格下不会出现横向滚动条，因为预览窗格通常只有几百像素宽，行宽上限必须能让正文自己折行而不是把整页撑开。

## 代码

```python
def fenwick_add(tree, i, delta):
    while i < len(tree):
        tree[i] += delta
        i += i & -i
```

```json
{ "skin": "dark", "size": 15, "measure": 40 }
```

```
无语言标注的围栏
```

## 分隔线

---

结束。
