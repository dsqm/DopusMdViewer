# -*- coding: utf-8 -*-
"""生成多编码判定样本（票 19）。

输出到 %TEMP%\\encsamples\\，不入库：其中 very-late-sjis.md 约 10 MB。

    python samples/manual-test/gen-enc.py
    bin\\encdetect.exe %TEMP%\\encsamples\\*.md

每个样本的预期写在文件名和下面的注释里；已知判不出来的两例也在末尾标注。
"""
import os

OUT = os.path.join(os.environ["TEMP"], "encsamples")

zh = "# 中文测试\n\n这是一个 Markdown 文件，用来验证编码判定。\n包含标点：，。！？——以及英文 mixed English。\n"
zht = "# 繁體中文測試\n\n這是一個 Markdown 檔案，用來驗證編碼判定。\n"
ja = "# 日本語テスト\n\nこれは Markdown ファイルです。エンコーディング判定を確認します。\n"
ko = "# 한국어 테스트\n\n이것은 마크다운 파일입니다. 인코딩 판정을 확인합니다.\n"
ru = "# Русский тест\n\nЭто файл Markdown для проверки определения кодировки.\n"
fr = "# Test français\n\nCeci est un fichier Markdown pour vérifier la détection d'encodage : été, naïve, garçon.\n"


def pad(text, times=400):
    """重复到 40 KB 量级。太短的样本 CED 自己也拿不准。"""
    return (text * times).encode("utf-8")


def write(name, data):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)


def main():
    os.makedirs(OUT, exist_ok=True)

    write("ascii.md", b"# plain ascii\n\nnothing but ascii here, no accents at all.\n" * 400)
    write("utf8.md", pad(zh))
    write("utf8-bom.md", b"\xef\xbb\xbf" + pad(zh))
    write("gb18030.md", pad(zh, 300).decode("utf-8").encode("gb18030"))
    write("big5.md", pad(zht, 300).decode("utf-8").encode("big5"))
    write("sjis.md", pad(ja, 300).decode("utf-8").encode("shift_jis"))
    write("eucjp.md", pad(ja, 300).decode("utf-8").encode("euc_jp"))
    write("jis.md", pad(ja, 200).decode("utf-8").encode("iso2022_jp"))
    write("euckr.md", pad(ko, 300).decode("utf-8").encode("euc_kr"))
    write("koi8r.md", pad(ru, 300).decode("utf-8").encode("koi8_r"))
    write("cp1251.md", pad(ru, 300).decode("utf-8").encode("cp1251"))
    write("cp1252.md", pad(fr, 300).decode("utf-8").encode("cp1252"))

    # 无 BOM 的 UTF-16：0x00 分布足够明显时能判出来
    write("u16le-nobom.md", pad(zh).decode("utf-8").encode("utf-16-le"))
    write("u16be-nobom.md", pad(zh).decode("utf-8").encode("utf-16-be"))
    # 已知判不出来：纯 CJK 的 UTF-16 没有任何 0x00，且 CED 会自信地判成 UTF-8
    write("u16le-cjk-nobom.md", ("中文测试" * 2000).encode("utf-16-le"))
    write("u32le-bom.md", b"\xff\xfe\x00\x00" + pad(zh, 100).decode("utf-8").encode("utf-32-le"))

    tail = pad(ja, 300).decode("utf-8").encode("shift_jis")
    # 非 ASCII 在 1 MB 之后但在 8 MB 探测窗口内：跳过前导 ASCII + 8 MB 窗口应当判对
    prefix = ("# long english section\n\n" + "plain ascii line for padding. " * 40 + "\n\n") * 1400
    write("ascii-then-sjis.md", prefix.encode("utf-8") + tail)
    # 非 ASCII 在 8 MB 之后：超出探测窗口，判成 ASCII 是预期结果
    prefix2 = ("# long english section\n\n" + "plain ascii line for padding. " * 40 + "\n\n") * 8000
    write("very-late-sjis.md", prefix2.encode("utf-8") + tail)

    for name in sorted(os.listdir(OUT)):
        print("%-24s %8d" % (name, os.path.getsize(os.path.join(OUT, name))))


if __name__ == "__main__":
    main()
