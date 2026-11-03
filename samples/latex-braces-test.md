# 双花括号公式与相对图片

模板引擎转义后残留的成对双花括号（KaTeX 本来不认，渲染报错后折叠一层重试）：

$$
A = \begin{{bmatrix}}
a_{{11}} & a_{{12}} \\
a_{{21}} & a_{{22}}
\end{{bmatrix}}
$$

行内公式：$a_{{11}} + b_{{22}}$

同目录图片：

![同目录](manual-test/pic.png)

子目录图片：

![子目录](manual-test/sub/pic2.png)
