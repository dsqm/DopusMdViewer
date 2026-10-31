// doc_index.h - 大文件块索引（票 04 / 05）。
//
// 索引只存源文件的字节偏移，不存解码后的字符位置；UTF-8 与 GB18030 共用同一条
// 字节扫描路径（0x0A 不会出现在这两个编码的多字节序列里），UTF-16 走 wchar_t。
// 块坐标相对 4 KB 页：offInPage / len16 各 2 B，浮出 64 KB 的块另存大块表。
//
// 与票 04 的一处实现偏差：kind 有 14 种，4 bit 的 kindNib 已占满，装不下
// loose / hidden / 脚注定义三个标志，因此块属性直接用 1 B/块（1 GB 多约 2 MB）。

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace doc {

enum class Kind : uint8_t {
  None = 0,
  Para,
  H1,
  H2,
  H3,
  H4,
  H5,
  H6,
  Fence,
  Table,
  ListItem,
  Quote,
  Html,
  Hr,
  FrontMatter,
};

constexpr uint8_t kKindMask = 0x0F;
constexpr uint8_t kFlagLoose = 0x10;        // 仅列表项：项内含空行，或空行后接同级项
constexpr uint8_t kFlagHidden = 0x20;       // 定义块 / front matter：渲染时产出空串
constexpr uint8_t kFlagFootnoteDef = 0x40;  // hidden 的脚注定义（[^x]: …）

// 票 05 第 4 节：超过这个行数的块走块内行窗口化。只记行数，不记字节。
constexpr uint32_t kBigBlockLines = 2000;

enum class Encoding : uint8_t { Utf8 = 0, Gb18030, Utf16LE, Utf16BE };

struct EncodingInfo {
  Encoding encoding = Encoding::Utf8;
  uint32_t bomBytes = 0;
};

// 只凭前缀判定。UTF-16 只认 BOM（无 BOM 的 UTF-16 仍是雾区）；
// 其余按「前 N 字节是否合法 UTF-8」区分 UTF-8 与 GB18030。
EncodingInfo DetectEncoding(const void* data, size_t bytes);

constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPageSize = 1u << kPageShift;
constexpr uint32_t kBigLenMark = 0xFFFF;

class BlockIndex {
 public:
  static constexpr uint32_t kInvalidBlock = 0xFFFFFFFFu;

  uint32_t BlockCount() const { return static_cast<uint32_t>(offInPage_.size()); }
  bool Empty() const { return offInPage_.empty(); }

  uint64_t Start(uint32_t i) const;
  uint64_t End(uint32_t i) const { return Start(i) + Len(i); }
  uint32_t Len(uint32_t i) const;
  uint8_t KindFlags(uint32_t i) const;
  uint32_t PageOf(uint32_t i) const;  // 块号 → 起始页
  const std::vector<uint32_t>& pageBase() const { return pageBase_; }
  Kind KindOf(uint32_t i) const {
    return static_cast<Kind>(KindFlags(i) & kKindMask);
  }
  bool IsLoose(uint32_t i) const { return (KindFlags(i) & kFlagLoose) != 0; }
  bool IsHidden(uint32_t i) const { return (KindFlags(i) & kFlagHidden) != 0; }
  bool IsFootnoteDef(uint32_t i) const {
    return (KindFlags(i) & kFlagFootnoteDef) != 0;
  }

  // 巨型块（> kBigBlockLines 行）的行数；不是巨型块或未知时返回 0。
  // 稀疏存放：只有极少数块会超过阈值，1 GB 文档里通常是个位数。
  uint32_t BigLines(uint32_t i) const;
  const std::vector<uint32_t>& bigBlockIndex() const { return bigBlockIndex_; }
  const std::vector<uint32_t>& bigBlockLines() const { return bigBlockLines_; }

  // 偏移落在哪个块：先取该页第一个块与下一页第一个块之间的区间二分；
  // 页内没有块起点时返回该页之前最后一个块（超长块与空行区间的正确解）。
  uint32_t BlockAtOffset(uint64_t off) const;

  // 索引占用（字节），只用于日志与验收。
  size_t MemoryBytes() const;

  // 构建期接口（IndexBuilder 使用；SealPages 之前 Start/BlockAtOffset 不可用）。
  // reserveBytes 是预计要扫的字节数（前缀扫描不必按全文预留块数组）。
  void Reserve(uint64_t fileSize, uint64_t reserveBytes);
  void Clear();
  void PushBlock(uint64_t start, uint64_t len, uint8_t kindFlags, uint32_t lines);
  void SealPages();

 private:

  std::vector<uint32_t> pageBase_;
  std::vector<uint16_t> offInPage_;
  std::vector<uint16_t> len16_;
  std::vector<uint32_t> bigLen_;
  std::vector<uint32_t> bigBlock_;
  std::vector<uint8_t> kindFlags_;
  std::vector<uint32_t> bigBlockIndex_;  // 稀疏：巨型块的块号（递增）
  std::vector<uint32_t> bigBlockLines_;  // 与 bigBlockIndex_ 一一对应
  uint64_t fileSize_ = 0;
  uint32_t pageCount_ = 0;
  bool sealed_ = false;
};

// 大纲节点固定 16 B：标题行起点与行内文本区间都在源字节坐标系里。
struct OutlineNode {
  uint64_t start;      // 标题行首字节
  uint16_t textOff;    // 文本相对 start 的偏移（前缀内）
  uint16_t textLen;    // 文本长度，超长截到前缀
  uint8_t level;       // 1..6
  uint8_t pad[3];
};

constexpr size_t kOutlineNodeBytes = sizeof(OutlineNode);

// 增量索引：Begin → 若干次 Feed → Finish。Feed 的字节必须按文件顺序给，
// 可以是任意切块（4 KB 与 4 MB 的结果必须一致，这条是 M3 的验收项）。
class IndexBuilder {
 public:
  IndexBuilder();
  ~IndexBuilder();

  // fileSize 用于页表规模与预留；limitBytes > 0 时只接受前 limitBytes 字节。
  // 返回 false 表示文件超过 4 GB 的分段上限（票 04：按 4 GB 分段各建一套）。
  bool Begin(EncodingInfo enc, uint64_t fileSize, uint64_t limitBytes);
  void Feed(const void* data, size_t bytes);
  void Finish();

  bool finished() const;
  bool CanFeedMore() const;
  uint64_t LimitBytes() const;
  uint64_t BytesScanned() const;
  uint64_t UnitsScanned() const;

  const BlockIndex& index() const;
  const std::vector<OutlineNode>& outline() const;
  EncodingInfo encoding() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 便捷入口：打开文件、扫到 limitBytes（0 = 全文件）。
// readMs / scanMs 分开计时，验收要区分 IO 与状态机。
bool ScanFile(const std::wstring& path, uint64_t limitBytes, IndexBuilder* out,
              double* readMs, double* scanMs, std::wstring* error,
              const std::atomic<bool>* cancel = nullptr);

}  // namespace doc
