// 巨型表列宽决策的回归测试（构建期用，不进包）。
//
// 普通表交给浏览器的自动表格布局，巨型表因为分块 + 固定布局，列宽必须自己算。
// 这个函数一旦偏了，表现是：短表被强行拉满、长表把正文栏撑破、或者窄列被压成
// 一条细线。所以直接读 src/app.js 抠真实实现逐组断言：
//   ① 自然宽放得下时原样保留
//   ② 放不下时压缩到正文宽，每列仍不低于最小列宽
//   ③ 列数 × 最小列宽都放不下：停在最小列宽，交给包装层横向滚动
//   ④ 关闭折行：保持单行自然宽，不压缩
//   ⑤ 自然宽小于最小列宽时按最小列宽兜底
//
// 跑：node test/table-cols.mjs

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const APP = path.resolve(HERE, '..', 'src', 'app.js')

const src = fs.readFileSync(APP, 'utf8')

// 按花括号配对抠出一个顶层函数源码
function grab(name) {
  const at = src.indexOf('function ' + name + '(')
  if (at < 0) throw new Error('app.js 里找不到 ' + name)
  let depth = 0
  for (let i = src.indexOf('{', at); i < src.length; i++) {
    if (src[i] === '{') depth++
    else if (src[i] === '}') {
      depth--
      if (depth === 0) return src.slice(at, i + 1)
    }
  }
  throw new Error(name + ' 的花括号不配对')
}

const fitTableCols = new Function('return ' + grab('fitTableCols'))()

let failed = 0

function check(name, cond, detail) {
  if (cond) {
    console.log('  v ' + name)
  } else {
    failed++
    console.log('  x ' + name + (detail ? '  → ' + detail : ''))
  }
}

const sum = (a) => a.reduce((x, y) => x + y, 0)

console.log('巨型表列宽')

// ---- ① 自然宽放得下：原样保留 ----
{
  const nat = [120, 80, 200]
  const min = [56, 56, 56]
  const got = fitTableCols(nat, min, 800, true)
  check('自然宽小于正文宽时不变', String(got) === '120,80,200', String(got))
}

// ---- ② 超过正文宽：压缩到正文宽，且不低于最小列宽 ----
{
  const nat = [600, 400, 200]
  const min = [56, 56, 56]
  const avail = 600
  const got = fitTableCols(nat, min, avail, true)
  check('压缩后总宽不超过正文宽', sum(got) <= avail, String(got) + ' sum=' + sum(got))
  check('压缩后每列不低于最小列宽', got.every((w, k) => w >= min[k]), String(got))
  check('压缩后不超过自然宽', got.every((w, k) => w <= nat[k]), String(got))
}

// ---- ②b 最小列宽放得下时不应出现横向滚动 ----
{
  // 一列刚好顶在最小列宽上、其余列很宽：按比例硬缩会把窄列压到下限，
  // 总宽反而超出正文宽，默认模式平白多一条滚动条。
  const nat = [60, 163, 103, 118, 278]
  const min = [58, 59, 59, 59, 59]
  const avail = 513
  const got = fitTableCols(nat, min, avail, true)
  check('最小列宽放得下时总宽不超正文宽', sum(got) <= avail, String(got) + ' sum=' + sum(got))
  check('最小列宽放得下时每列不低于下限', got.every((w, k) => w >= min[k]), String(got))
}

// ---- ③ 最小列宽本身就放不下：停在最小列宽（包装层滚动） ----
{
  const nat = [900, 900, 900]
  const min = [100, 100, 100]
  const got = fitTableCols(nat, min, 200, true)
  check('放不下时按最小列宽兜底', String(got) === '100,100,100', String(got))
  check('兜底后总宽允许超过正文宽', sum(got) > 200, String(sum(got)))
}

// ---- ④ 关闭折行：不压缩 ----
{
  const nat = [900, 900, 900]
  const min = [100, 100, 100]
  const got = fitTableCols(nat, min, 400, false)
  check('横向滚动模式保持自然宽', String(got) === '900,900,900', String(got))
}

// ---- ⑤ 自然宽小于最小列宽 ----
{
  const nat = [20, 30]
  const min = [64, 64]
  const got = fitTableCols(nat, min, 800, true)
  check('空列/单字列抬到最小列宽', String(got) === '64,64', String(got))
}

// ---- ⑥ 小数列宽向上取整，避免半像素误差累积后表头折行 ----
{
  const got = fitTableCols([100.2, 80.9], [10, 10], 800, true)
  check('列宽取整', got.every((w) => Number.isInteger(w)), String(got))
}

process.exit(failed ? 1 : 0)
