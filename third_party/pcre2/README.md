# PCRE2 10.45（vendored）

- 来源：`https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.45/pcre2-10.45.tar.gz`
  （GitHub 的源码 zip 不含 `deps/sljit` 子模块，JIT 编不过，必须用 release tarball）。
- 只保留库源码：CLI（pcre2test/pcre2grep/pcre2posix）、测试与 dftables 已删；
  `deps/sljit/sljit_src` 原样保留（`pcre2_jit_compile.c` 用相对路径包含它）。
- `include/` 是生成物：
  - `pcre2.h` ← `src/pcre2.h.generic`
  - `config.h` ← `src/config.h.generic` + `SUPPORT_UNICODE 1`、`SUPPORT_JIT 1`
- 构建：`build-pcre2.bat`（8 位与 16 位各一套，`/MT` 静态，对象缓存在 `bin\obj-pcre2\`）。
  改了这里的源码或配置后删掉 `bin\obj-pcre2\` 再编。
- 许可证：`LICENCE.md`（BSD-3-Clause）。
