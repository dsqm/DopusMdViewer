// 网络图片确认的回归测试（构建期用，不进包）。
//
// 这一块的两档语义只差在"一篇文档里问几次"，但记忆的键与失效时机是两者的共同前提，
// 错了不会报错，只会表现为"刚允许过的网站又跳出来问"或"换一篇回来还要再问一遍"。
// 所以这里逐条比对：
//   ① 每主机档：同一篇里每个主机各问一次，允许与拒绝在本篇内都算数
//   ② 切走再切回：同一篇仍然记得；换到别的文档同一主机要重新问
//   ③ 每文档档：整篇只问一次，答复覆盖这篇里的所有主机，切回来仍有效
//   ④ 会话 id 变了（DO 重启）：整份记忆作废，重新问
//   ⑤ 页面被回收重建（localStorage 还在）：记忆恢复，不重复问
//   ⑥ 超过 64 篇：按最近使用淘汰，被淘汰的那篇重问
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

// 没有花括号的普通常量：取到行尾
function lineOf(head) {
  const at = src.indexOf(head)
  if (at < 0) throw new Error('app.js 里找不到 ' + head)
  return src.slice(at, src.indexOf('\n', at))
}

// 确认面板要画 DOM，这里用两个桩替掉：只记录"问了哪个主机"。
// 函数体内按名字调用 showNetPrompt / hideNetPrompt，所以用形参注入；
// localStorage 走 window 参数，可以跨实例共用一份（模拟页面重建后磁盘还在）。
const body = [
  lineOf('const NET_STORE_KEY'),
  lineOf('const NET_STORE_VER'),
  lineOf('const NET_MAX_DOCS'),
  grab('const NET = {'),
  grab('function netHash('),
  grab('function netStoreLoad('),
  grab('function netStoreSave('),
  grab('function netEntry('),
  grab('function netOpenDoc('),
  grab('function netAllowedNow('),
  grab('function ensureNetConsent('),
  grab('function pumpNetConsent('),
  grab('function decideNetConsent('),
  grab('function setNetMode('),
  grab('function dropNetPrompts('),
  'return { NET, netOpenDoc, ensureNetConsent, decideNetConsent, setNetMode, dropNetPrompts, netAllowedNow }',
].join('\n')

function makeStorage() {
  const map = new Map()
  return {
    getItem: (k) => (map.has(k) ? map.get(k) : null),
    setItem: (k, v) => map.set(k, String(v)),
    removeItem: (k) => map.delete(k),
  }
}

const pump = []
const make = (storage) =>
  new Function('showNetPrompt', 'hideNetPrompt', 'window', body)(
    (host) => pump.push(host), () => {}, { localStorage: storage })

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
  const M = make(makeStorage())
  M.setNetMode(1)
  M.netOpenDoc('c:\\docs\\a.md*1*2', 's1')
  M.ensureNetConsent('a.com')
  M.ensureNetConsent('b.com')
  check('每主机档：先问第一个主机', pump.length === 1 && pump[0] === 'a.com', pump)
  M.decideNetConsent(true)
  check('每主机档：答完才问第二个主机', pump.length === 2 && pump[1] === 'b.com', pump)
  M.decideNetConsent(false)
  check('每主机档：允许与拒绝各记各的主机',
    M.NET.cur.allowed.has('a.com') && M.NET.cur.denied.has('b.com'),
    [[...M.NET.cur.allowed], [...M.NET.cur.denied]])
  M.ensureNetConsent('a.com')
  M.ensureNetConsent('b.com')
  check('每主机档：已经答过的主机在本篇内不再问', pump.length === 2, pump)
}

// ---- ② 切走再切回记得；换文档重问 ----
{
  pump.length = 0
  const M = make(makeStorage())
  M.setNetMode(1)
  M.netOpenDoc('docA', 's1')
  M.ensureNetConsent('a.com')
  M.decideNetConsent(true)

  M.netOpenDoc('docB', 's1')
  M.ensureNetConsent('a.com')
  check('换到别的文档：同一主机重新问', pump.length === 2 && pump[1] === 'a.com', pump)
  M.decideNetConsent(false)

  M.netOpenDoc('docA', 's1')
  M.ensureNetConsent('a.com')
  check('切回原来的文档：不再问', pump.length === 2, pump)
  check('切回原来的文档：答复还在', M.netAllowedNow('a.com'))

  M.netOpenDoc('docB', 's1')
  M.ensureNetConsent('a.com')
  check('再切到 B：B 里的拒绝也记得', pump.length === 2, pump)
}

// ---- ③ 每文档档：整篇一票 ----
{
  pump.length = 0
  const M = make(makeStorage())
  M.setNetMode(2)
  M.netOpenDoc('docC', 's1')
  M.ensureNetConsent('a.com')
  M.decideNetConsent(true)
  const rest = M.ensureNetConsent('b.com')
  check('每文档档：同一篇里第二个主机不再问', pump.length === 1, pump)
  check('每文档档：整篇一个答复', M.NET.cur.decided === true)
  rest.then((v) => check('每文档档：答复覆盖所有来源', v === true))

  M.netOpenDoc('docD', 's1')
  M.ensureNetConsent('a.com')
  check('每文档档：换文档重新问', pump.length === 2, pump)

  M.netOpenDoc('docC', 's1')
  M.ensureNetConsent('c.com')
  check('每文档档：切回来仍有效', pump.length === 2, pump)
}

// ---- ④ 会话 id 变了：整份作废 ----
{
  pump.length = 0
  const storage = makeStorage()
  const M = make(storage)
  M.setNetMode(1)
  M.netOpenDoc('docA', 's1')
  M.ensureNetConsent('a.com')
  M.decideNetConsent(true)

  M.netOpenDoc('docA', 's2')  // DO 重启：同一个文件、新的会话
  M.ensureNetConsent('a.com')
  check('换会话：同一篇同一主机重新问', pump.length === 2, pump)
  check('换会话：旧答复没被当真', !M.netAllowedNow('a.com'))
}

// ---- ⑤ 页面重建（localStorage 还在）：记忆恢复 ----
{
  pump.length = 0
  const storage = makeStorage()
  const M1 = make(storage)
  M1.setNetMode(1)
  M1.netOpenDoc('docA', 's1')
  M1.ensureNetConsent('a.com')
  M1.decideNetConsent(true)

  const M2 = make(storage)  // 新页面：内存全没了，只剩 localStorage
  M2.setNetMode(1)
  M2.netOpenDoc('docA', 's1')
  M2.ensureNetConsent('a.com')
  check('页面重建后：同一篇不再问', pump.length === 1, pump)
  check('页面重建后：答复从磁盘恢复', M2.netAllowedNow('a.com'))
}

// ---- ⑥ 上限 64 篇：最久没用到的被淘汰 ----
{
  pump.length = 0
  const M = make(makeStorage())
  M.setNetMode(1)
  for (let i = 0; i < 65; i++) {
    M.netOpenDoc('doc' + i, 's1')
    M.ensureNetConsent('h.com')
    M.decideNetConsent(true)
  }
  check('超过 64 篇：写盘时按最近使用淘汰', M.NET.docs.size === 64, M.NET.docs.size)
  M.netOpenDoc('doc0', 's1')
  M.ensureNetConsent('h.com')
  check('最早那篇被淘汰后重新问', pump.length === 66, pump.length)
}

if (failed) {
  console.log('\n有 ' + failed + ' 条不通过')
  process.exit(1)
}
console.log('\n全部通过')
