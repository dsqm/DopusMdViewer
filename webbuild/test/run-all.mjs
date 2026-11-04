// 前端回归测试的统一入口（构建期用，不进包）。
//
// 目录下每个 *.mjs 都是独立脚本（自带 check 与退出码），本入口逐个拉起、
// 汇总结果，省得一个个跑。新增测试脚本后把文件名加进 TESTS 即可。
//
// 跑：node test/run-all.mjs
// （本机没有 node 时用 Qoder 的 Electron 当 Node：
//   ELECTRON_RUN_AS_NODE=1 "$LOCALAPPDATA/Programs/Qoder CN/Qoder CN.exe" test/run-all.mjs）
// C++ 侧的单元测试是另一套：python tools/build_unittests.py run

import { spawnSync } from 'node:child_process'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))

const TESTS = [
  'table-cols.mjs',
  'whitespace-mark.mjs',
  'highlight-split.mjs',
  'net-consent.mjs',
  'img-zoom.mjs',
  'link-open.mjs',
]

let failed = 0
for (const name of TESTS) {
  console.log(`==== ${name} ====`)
  const r = spawnSync(process.execPath, [path.join(HERE, name)], {
    stdio: 'inherit',
    env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' },
  })
  if (r.status !== 0) {
    failed++
    console.log(`==== ${name} 失败（exit=${r.status ?? r.error?.code}）====`)
  }
}

console.log('------------------------------------------------------------')
console.log(`${TESTS.length} 个脚本，${failed} 个失败`)
process.exit(failed ? 1 : 0)
