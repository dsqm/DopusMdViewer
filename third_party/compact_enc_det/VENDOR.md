# compact_enc_det（vendored）

- 上游：https://github.com/google/compact_enc_det
- 提交：`d127078cedef9c6642cbe592dacdd2292b50bb19`（2024-02-12）
- 许可证：**Apache-2.0**，见本目录 `LICENSE`。`CMakeLists.txt` 里 "BSD-style" 那句是上游过期注释，以 `LICENSE` 为准。
- 不改上游源码。构建走工程根的 `build-ced.bat`（不经 cmake）。

## 相对上游删掉的

`compact_enc_det_unittest.cc`（376 KB）、`compact_enc_det_fuzz_test.cc`、
`util/encodings/encodings_unittest.cc`、`autogen.sh`、`.gitignore`。
删的都是单测与构建脚手架，本项目不跑上游单测。其余文件与上游逐字节一致。

## 使用约束

1. **只有 `src/encoding_detect.cpp` 可以 include 本目录的头文件**。原因：
   `util/encodings/encodings.h` 第 30 行有 `#undef UNICODE`（为了让名为 `UNICODE` 的枚举值编得过），
   一旦被别的 TU 间接包含，之后所有 `UNICODE` 版 Win32 宏都会失效。
2. `compact_enc_det.cc` 吞两个生成表头（632 KB + 88 KB），单 TU 目标文件很大，编译要带 `/bigobj`。
3. 本库**只覆盖 7/8 位编码**，枚举族里没有 UTF-16/UTF-32。无 BOM 的 UTF-16 由我们自己判定。
