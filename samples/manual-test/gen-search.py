# -*- coding: utf-8 -*-
"""生成搜索验收用的大样本（票 16 巨型块定位、跨块拖选）。

输出到 %TEMP%\\mdvsearch\\，不入库：big-fence-hits.md 约 2 MB、20 万行级。

    python samples/manual-test/gen-search.py
    bin\\indextest.exe literal %TEMP%\\mdvsearch\\big-fence-hits.md zqneedle

命中位置由脚本写死并打印出来，改脚本时同步改 CHECK.md 里的预期。
"""
import os

OUT = os.path.join(os.environ["TEMP"], "mdvsearch")

FENCE_LINES = 120000
NEEDLES = {1001: "zqneedle", 90001: "zqneedle", FENCE_LINES: "zqtail"}


def write_big_fence():
    head = [
        "# 巨型块命中定位",
        "",
        "普通块里的命中：zqneedle",
        "",
        "```text",
    ]
    body = []
    for i in range(1, FENCE_LINES + 1):
        needle = NEEDLES.get(i)
        if needle:
            body.append("%06d %s 围栏内第 %d 行" % (i, needle, i))
        else:
            body.append("%06d 普通围栏行 filler" % i)
    data = "\n".join(head + body + ["```", ""])
    with open(os.path.join(OUT, "big-fence-hits.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(data)
    return len(head)


def write_mid_blocks(count=300):
    """大量小块：拖选跨块时视口上下边缘要自动滚动延伸选区。"""
    chunks = ["# 多块拖选验收", ""]
    for i in range(1, count + 1):
        chunks.append("第 %03d 块第一行" % i)
        chunks.append("第 %03d 块第二行，用来拖选" % i)
        chunks.append("")
    with open(os.path.join(OUT, "mid-blocks.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(chunks))


def main():
    os.makedirs(OUT, exist_ok=True)
    head_lines = write_big_fence()
    write_mid_blocks()

    for name in sorted(os.listdir(OUT)):
        path = os.path.join(OUT, name)
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            lines = f.read().count(b"\n")
        print("%-20s %8d B  %7d 行" % (name, size, lines))

    print("\nbig-fence-hits.md 的命中位置（整篇绝对行号）：")
    for rel in sorted(NEEDLES):
        print("  围栏内第 %6d 行 → 整篇第 %6d 行  %s" % (rel, head_lines + rel, NEEDLES[rel]))
    print("  围栏结束在整篇第 %d 行" % (head_lines + FENCE_LINES + 1))


if __name__ == "__main__":
    main()
