/* 外壳占位页：只做一件事——把宿主推来的 hello 显示出来，证明通道与映射都通。 */

const el = (id) => document.getElementById(id)

function show(msg) {
  if (!msg || msg.type !== 'hello') return
  el('exe').textContent = msg.exe
  el('web').textContent = msg.web
  el('skins').textContent = msg.skins
  el('ms').textContent = Math.round(msg.ms) + ' ms'
  // 页面侧也回一条，宿主的日志里能看到往返
  post({ type: 'ready', ms: msg.ms })
}

function post(obj) {
  if (window.chrome && window.chrome.webview) window.chrome.webview.postMessage(obj)
}

if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener('message', (e) => show(e.data))
  // 宿主可能在脚本挂上之前就推过；要一次当前状态也无妨，重复推不会出事
  post({ type: 'hello?', href: location.href })
}
