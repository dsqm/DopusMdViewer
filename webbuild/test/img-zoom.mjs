// 图片点击放大的回归测试（构建期用，不进包）。
//
// 路由与几何都抽成了 src/app.js 的顶层纯函数，这里抠真实实现逐组断言：
//   路由 imgAction —— 输入图片属性快照 + 键位 → 输出动作/帧方向
//     ① 本地图左键 zoom、右键 imgctx（帧带 local 标记）
//     ② 链接内图片左键让位给链接，右键仍给图片动作（放大走右键）
//     ③ 远程图只有「确认后真正加载」（src === mdvSrc）可动手，帧带 remote
//     ④ 未确认（透明占位）与被拒（无 src）的远程图两键都不可动手
//     ⑤ data:/blob: 算本地图（宿主端解不出磁盘路径时只出全局菜单，页面不管）
//   几何 zoomFitScale / zoomScaleBounds / zoomClampOffset / zoomWheelZoom /
//   zoomToggleZoom / zoomInitialState —— 输入原图尺寸/视口/光标/倍率 → 变换参数
//     ⑥ contain：完整显示且宽高比不变；退化输入不炸
//     ⑦ 允许倍率范围必须同时覆盖 fit 与 1:1
//     ⑧ 光标中心缩放：光标下的图像点不动；倍率钳在上下限内
//     ⑨ 平移边界：小轴固定居中，大轴图像边缘不出视口（拖动边界同源）
//     ⑩ 双击在 fit ↔ 1:1 之间切换，切换点仍锚在光标下
//
// 跑：node test/img-zoom.mjs（统一入口 test/run-all.mjs）

import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const APP = path.resolve(HERE, '..', 'src', 'app.js')

const src = fs.readFileSync(APP, 'utf8')

// 按花括号配对抠出一个顶层函数源码（与 table-cols.mjs 同一套路）
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

// 这组函数互相调用（zoomWheelZoom 用到 zoomScaleBounds/zoomClampOffset），
// 必须放进同一个作用域里一起求值
const NAMES = ['imgAction', 'zoomFitScale', 'zoomScaleBounds', 'zoomClampOffset',
               'zoomWheelZoom', 'zoomToggleZoom', 'zoomInitialState']
const api = new Function(NAMES.map(grab).join('\n') +
  '; return {' + NAMES.join(',') + '}')()

let failed = 0

function check(name, cond, detail) {
  if (cond) {
    console.log('  v ' + name)
  } else {
    failed++
    console.log('  x ' + name + (detail ? '  → ' + detail : ''))
  }
}

const close = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps

console.log('图片路由 imgAction')

const LOCAL_ASSET = 'https://mdblocks.localhost/__asset?p=pic.png'
const REMOTE = 'https://example.com/a/cat.jpg'

// ---- ① 本地图 ----
{
  const attrs = { inLink: false, src: LOCAL_ASSET, mdvSrc: '', mdvNet: false }
  const left = api.imgAction(attrs, 'left')
  check('本地图左键开遮罩', left.act === 'zoom' && left.kind === 'local' && left.url === LOCAL_ASSET,
        JSON.stringify(left))
  const right = api.imgAction(attrs, 'right')
  check('本地图右键发 imgctx 且标记 local',
        right.act === 'imgctx' && right.kind === 'local' && right.url === LOCAL_ASSET,
        JSON.stringify(right))
}

// ---- ② 链接内图片 ----
{
  const attrs = { inLink: true, src: LOCAL_ASSET, mdvSrc: '', mdvNet: false }
  check('链接内左键让位给链接', api.imgAction(attrs, 'left').act === null)
  const right = api.imgAction(attrs, 'right')
  check('链接内右键仍有图片动作', right.act === 'imgctx' && right.kind === 'local', JSON.stringify(right))
}

// ---- ③ 远程图（已确认加载） ----
{
  const attrs = { inLink: false, src: REMOTE, mdvSrc: REMOTE, mdvNet: true }
  const left = api.imgAction(attrs, 'left')
  check('已确认远程图左键可放大', left.act === 'zoom' && left.kind === 'remote' && left.url === REMOTE,
        JSON.stringify(left))
  const right = api.imgAction(attrs, 'right')
  check('已确认远程图右键标记 remote', right.act === 'imgctx' && right.kind === 'remote',
        JSON.stringify(right))
}

// ---- ④ 远程图（未确认 / 被拒） ----
{
  // 等待确认时 src 是 1x1 透明占位，不是远程真身
  const pending = { inLink: false, src: 'data:image/gif;base64,R0', mdvSrc: REMOTE, mdvNet: true }
  check('未确认远程图左键不可动手', api.imgAction(pending, 'left').act === null)
  check('未确认远程图右键不可动手', api.imgAction(pending, 'right').act === null)
  // 被拒时 src 被整个移除
  const denied = { inLink: false, src: '', mdvSrc: REMOTE, mdvNet: true }
  check('被拒远程图两键都不可动手',
        api.imgAction(denied, 'left').act === null && api.imgAction(denied, 'right').act === null)
  // data-mdv-src 缺失（不该发生）：不能把远程 src 误判成可打开的本地图
  const noMark = { inLink: false, src: REMOTE, mdvSrc: '', mdvNet: true }
  check('缺 mdvSrc 标记的远程图不可动手', api.imgAction(noMark, 'right').act === null)
}

// ---- ⑤ data:/blob: 与无 src ----
{
  const data = { inLink: false, src: 'data:image/png;base64,AAAA', mdvSrc: '', mdvNet: false }
  check('data: 图左键可放大', api.imgAction(data, 'left').act === 'zoom')
  check('data: 图右键发 imgctx', api.imgAction(data, 'right').act === 'imgctx')
  check('无 src（本地图片关闭）不可动手',
        api.imgAction({ inLink: false, src: '', mdvSrc: '', mdvNet: false }, 'left').act === null)
}

console.log('遮罩几何')

// ---- ⑥ contain ----
{
  const fit = api.zoomFitScale(2000, 1000, 1000, 500)
  check('大图缩到恰好塞满（宽先顶满）', close(fit, 0.5), String(fit))
  const fit2 = api.zoomFitScale(2000, 1000, 400, 500)
  check('宽图受宽度约束', close(fit2, 0.2), String(fit2))
  const fit3 = api.zoomFitScale(1000, 2000, 500, 400)
  check('高图受高度约束', close(fit3, 0.2), String(fit3))
  const fit4 = api.zoomFitScale(100, 50, 1000, 500)
  check('小图放大塞满（contain 口径）', close(fit4, 10), String(fit4))
  check('退化尺寸回 1 不炸', api.zoomFitScale(0, 50, 1000, 500) === 1 &&
        api.zoomFitScale(100, 50, 0, 500) === 1)
  const st = api.zoomInitialState(2000, 1000, 1000, 500)
  check('初始态 scale=fit 且居中',
        close(st.scale, st.fit) && close(st.x, 0) && close(st.y, 0), JSON.stringify(st))
}

// ---- ⑦ 倍率范围 ----
{
  const b1 = api.zoomScaleBounds(0.01)
  check('超大图范围覆盖 fit 与 1:1', b1.min <= 0.01 && b1.max >= 1, JSON.stringify(b1))
  const b2 = api.zoomScaleBounds(56)
  check('小图放大后范围仍含 1:1', b2.min <= 1 && b2.max >= 56, JSON.stringify(b2))
  const b3 = api.zoomScaleBounds(1)
  check('退化 fit 落在范围内', b3.min <= 1 && b3.max >= 1, JSON.stringify(b3))
}

// ---- ⑧ 光标中心缩放 + 倍率钳制 ----
{
  // 2000×1000 图，视口 1000×500，fit=0.5 初始态；放大 2 倍锚在 (700, 300)
  const base = { ...api.zoomInitialState(2000, 1000, 1000, 500),
                 natW: 2000, natH: 1000, vpW: 1000, vpH: 500 }
  const cur = { ...base, x: -200, y: -100, scale: 0.6 }
  const cursor = { x: 700, y: 300 }
  const st = api.zoomWheelZoom(cur, cursor, 2)
  check('倍率翻倍', close(st.scale, 1.2), JSON.stringify(st))
  // 光标下的图像点：旧 (700+200)/0.6=1500, (300+100)/0.6≈666.7；新位置 700-1500*1.2, 300-666.67*1.2
  check('光标下的图像点不动', close(700 - st.x, ((cursor.x - cur.x) / cur.scale) * st.scale) &&
        close(300 - st.y, ((cursor.y - cur.y) / cur.scale) * st.scale))
  // 倍率钳制：往上滚 100 次、往下滚 100 次，都不能出界。
  // zoomWheelZoom 只回 { scale, x, y }，视口与 fit 每次都从 base 补（DOM 侧 zoomCur() 同款）
  const b = api.zoomScaleBounds(base.fit)
  let up = base
  for (let i = 0; i < 100; i++) up = api.zoomWheelZoom({ ...base, ...up, fit: base.fit }, cursor, 2)
  check('放大钳在上限', close(up.scale, b.max), String(up.scale))
  let down = base
  for (let i = 0; i < 100; i++) down = api.zoomWheelZoom({ ...base, ...down, fit: base.fit }, cursor, 0.5)
  check('缩小钳在下限', close(down.scale, b.min), String(down.scale))
}

// ---- ⑨ 平移边界 ----
{
  // 放大后 2000×1000 的图在 1000×500 视口里拖动：上下左右都不露缝
  const c1 = api.zoomClampOffset(-9999, -9999, 2000, 1000, 1000, 500)
  check('越界拖回左上角', close(c1.x, -1000) && close(c1.y, -500), JSON.stringify(c1))
  const c2 = api.zoomClampOffset(9999, 9999, 2000, 1000, 1000, 500)
  check('反方向拖到 0', close(c2.x, 0) && close(c2.y, 0), JSON.stringify(c2))
  // 图比视口小的轴固定居中，拖了也不动
  const c3 = api.zoomClampOffset(-300, 0, 800, 400, 1000, 500)
  check('小轴固定居中', close(c3.x, 100) && close(c3.y, 50), JSON.stringify(c3))
}

// ---- ⑩ 双击切换 ----
{
  const base = { ...api.zoomInitialState(2000, 1000, 1000, 500),
                 natW: 2000, natH: 1000, vpW: 1000, vpH: 500 }
  const cursor = { x: 700, y: 300 }
  const toOne = api.zoomToggleZoom(base, cursor)
  check('fit 状态双击回 1:1', close(toOne.scale, 1), JSON.stringify(toOne))
  check('1:1 点仍锚在光标下', close(700 - toOne.x, (700 - base.x) / base.scale) &&
        close(300 - toOne.y, (300 - base.y) / base.scale))
  const back = api.zoomToggleZoom({ ...base, ...toOne, fit: base.fit }, cursor)
  check('1:1 状态双击回 fit', close(back.scale, base.fit), JSON.stringify(back))
  check('回 fit 后不越界', back.x <= 0 && back.x >= -2000 * back.scale + 1000 &&
        back.y <= 0 && back.y >= -1000 * back.scale + 500)
}

process.exit(failed ? 1 : 0)
