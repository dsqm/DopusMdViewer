// 网络图片确认的回归测试（构建期用，不进包）。
//
// 这一块的两档语义只差在"一篇文档里问几次"，但换文件时的清空时机是两者的共同前提，
// 错了不会报错，只会表现为"刚允许过的网站又跳出来问"或"拒绝过一次就再也没图"。
// 所以这里逐条比对：
//   ① 每主机档：一篇文档里每个主机各问一次，允许与拒绝都只在本篇有效
//   ② 换文件（resetNetConsent）时两个集合都清空——下一篇文档里同一主机重新问
//   ③ 每文档档：整篇只问一次，答复覆盖这篇里的所有主机，换文件作废
//
// 直接读 src/app.js 抠真实实现，不复制一份逻辑来测，否则测的是另一份代码。
// 跑：node test/net-consent.mjs

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const APP = path.join(HERE, '..', 'src', 'app.js')
const src = fs.readFileSync(APP, 'utf8')

// 按花括号配对抠出一个顶层函数 / 顶层对象字面量的源码
function grab(head) {
  const at = src.indexOf(head)
  if (at < 0) throw new Error('app.js 里找不到 ' + head)
  let depth = 0
  for (let i = src.indexOf('{', at); i < src.length; i++) {
    if (src[i] === '{') depth++
    else if (src[i] === '}') {
      depth--
      if (depth === 0) return src.slice(at, i + 1)
    }
  }
  throw new Error(head + '的花括号不配对')
}

// 确认面板要画 DOM，这里用两个桩替掉：只记录"问了哪个主机"。
// 函数体内按名字调用 showNetPrompt / hideNetPrompt，所以用形参注入。
const body = [
  grab('const NET = {'),
  grab('function ensureNetConsent('),
  grab('function pumpNetConsent('),
  grab('function decideNetConsent('),
  grab('function setNetMode('),
  grab('function resetNetConsent('),
  'return { NET, ensureNetConsent, decideNetConsent, resetNetConsent, setNetMode }',
].join('\n')

const pump = []
const make = () =>
  new Function('showNetPrompt', 'hideNetPrompt', body)((host) => pump.push(host), () => {})

let failed = 0
function check(name, cond, detail) {
  if (cond) {
    console.log('  v ' + name)
  } else {
    failed++
    console.log('  x ' + name + (detail === undefined ? '' : '   ' + JSON.stringify(detail)))
  }
}

console.log('网络图片确认')

// ---- ① 每主机档：一篇里按主机逐个问 ----
{
  pump.length = 0
  const M = make()
  M.setNetMode(1)
  M.ensureNetConsent('a.com')
  M.ensureNetConsent('b.com')
  check('每主机档：先问第一个主机', pump.length === 1 && pump[0] === 'a.com', pump)
  M.decideNetConsent(true)
  check('每主机档：答完才问第二个主机', pump.length === 2 && pump[1] === 'b.com', pump)
  M.decideNetConsent(false)
  check('每主机档：允许与拒绝各记各的主机',
    M.NET.allowed.has('a.com') && M.NET.denied.has('b.com'),
    [[...M.NET.allowed], [...M.NET.denied]])
  check('每主机档：已经答过的主机不再问', pump.length === 2, pump)
}

// ---- ② 换文件时两个集合一起清（A 文档允许 b.com，B 文档仍要问）----
{
  pump.length = 0
  const M = make()
  M.setNetMode(1)
  M.ensureNetConsent('a.com')
  M.decideNetConsent(true)
  M.ensureNetConsent('b.com')
  M.decideNetConsent(false)
  M.resetNetConsent()
  check('换文件后允许记录清空', M.NET.allowed.size === 0, [...M.NET.allowed])
  check('换文件后拒绝记录清空', M.NET.denied.size === 0, [...M.NET.denied])
  M.ensureNetConsent('a.com')
  check('换文件后同一主机重新问', pump.length === 3 && pump[2] === 'a.com', pump)
}

// ---- ③ 每文档档：整篇一票 ----
{
  pump.length = 0
  const M = make()
  M.setNetMode(2)
  M.ensureNetConsent('a.com')
  M.decideNetConsent(true)
  const rest = M.ensureNetConsent('b.com')
  check('每文档档：同一篇里第二个主机不再问', pump.length === 1, pump)
  check('每文档档：整篇一个答复', M.NET.docDecided === true)
  rest.then((v) => check('每文档档：答复覆盖所有来源', v === true))
  M.resetNetConsent()
  check('每文档档：换文件后答复作废', M.NET.docDecided === null)
  M.ensureNetConsent('a.com')
  check('每文档档：换文件后重新问', pump.length === 2, pump)
}

if (failed) {
  console.log('\n有 ' + failed + ' 条不通过')
  process.exit(1)
}
console.log('\n全部通过')
