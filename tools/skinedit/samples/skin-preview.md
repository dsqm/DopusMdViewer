<!-- 皮肤编辑器的预览样例。base.css 里受皮肤控制的元素在这里各出现一次：
     点了表单里任何一个颜色，右边都该有东西跟着变。改 base.css 的选择器时顺手改
     这里，否则会冒出一批"改了没反应"的变量。

     不放的东西：图片（渲染时会被改写成 __asset 地址，预览侧没有那条路由）、
     Mermaid（异步渲染，且不吃 --md-* 变量）。 -->

# 一、标题与正文

## 二级标题 H2

### 三级标题 H3

#### 四级标题 H4

##### 五级标题 H5

###### 六级标题 H6

正文里的**加粗**、*倾斜*、~~删除线~~、`行内代码`、==高亮标记==、H~2~O、x^2^，
以及一个[链接](https://example.com)、一个自动链接 <https://example.com>、
一键"搜索命中"的两种状态：<mark class="hit">普通命中</mark>
与 <mark class="hit hit--current">当前命中</mark>。

行内代码带特殊符号 `if (a < b && c > d) {}`，还有一个变量名 `--md-bg`。

> 引用块第一行，底色是 --md-quote-bg，左边一条 --md-quote-bar。
>
> 引用块第二段，里面也有 `行内代码` 和**加粗**。
>
> > 嵌套引用，颜色再退一层。

## 二、列表

- 无序项一
- 无序项二
  - 嵌套项 A
  - 嵌套项 B

1. 有序项一
2. 有序项二
3. 有序项三

3. 从三开始的有序列表
4. 接着是四

- [ ] 未完成任务
- [x] 已完成任务
- [ ] 带嵌套的未完成
  - [x] 嵌套完成

- 松散项一

- 松散项二

术语 A
: 定义一，用来确认 dl/dt/dd 的间距。

术语 B
: 定义二。

## 三、表格

| 左对齐 | 居中 | 右对齐 |
|:-------|:----:|-------:|
| 奇行 | `code` | 123 |
| 偶行 | **粗** | 456 |
| 奇行，长内容单元格会换行，用来看边框与奇偶底色 | 中间 | 789 |
| 偶行 | --md-table-row-hover | 0 |

## 四、代码

```python
# 注释：--md-syn-comment；行号用 --md-code-ln-fg
import math


class Circle:                                   # --md-syn-title
    """类型名走 --md-syn-type。"""

    def area(self, radius: float) -> float:     # --md-syn-builtin
        return math.pi * radius ** 2

    NAME = "字符串走 --md-syn-string"
    SIDES = 0                                   # 数字走 --md-syn-number
```

```javascript
const re = /--md-syn-[a-z-]+/g
function pick(list, key = 'name') {
  return list.filter((x) => x[key] && x[key].length > 0)
}
```

```css
/* 选择器走 --md-syn-selector，属性名走 --md-syn-name */
.mdv-cb > button:hover,
.markdown-alert-tip {
  color: #0969da;
  background: var(--md-icon-bg);
}
```

```diff
- 删除行走 --md-syn-deletion / -bg
+ 新增行走 --md-syn-addition / -bg
  未变的上下文行
```

```c
#include <stdio.h>              /* 预处理行与注释走 --md-syn-meta / -comment */

struct Node {                   /* 类型名走 --md-syn-type */
  int value;
};
```

```html
<div class="note" data-kind="tip">标签名走 --md-syn-name</div>
```

```bash
echo "${HOME}/bin"              # 变量走 --md-syn-variable
```

    缩进式代码块（没有语言标注）

## 五、提示框

> [!NOTE]
> 说明框：左边框 --md-alert-note，底 --md-alert-note-bg。

> [!TIP]
> 提示框：tip。

> [!IMPORTANT]
> 重要框：important。

> [!WARNING]
> 警告框：warning。

> [!CAUTION]
> 危险框：caution。

## 六、脚注与数学

这里有一个脚注引用[^1]，还有一个更长的[^long]。

[^1]: 脚注一的内容，颜色走 --md-footnote-fg。
[^long]: 脚注二的
    内容可以换行。

行内公式 $E = mc^2$ 与块级公式：

$$
\int_{-\infty}^{\infty} e^{-x^2}\,dx = \sqrt{\pi}
$$

---

## 七、原始 HTML

<details>
<summary>折叠块标题</summary>

折叠块里的正文。

</details>

<div class="custom">原始 HTML 里的 div</div>

~~这一行被删除~~

最后一段正文，用来确认块间距（--md-space-block）与末尾留白。
