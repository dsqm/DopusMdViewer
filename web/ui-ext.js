// 手写 UI 扩展（不属于 app.js bundle）：
//   docerror|<文本>：打开失败，清空旧页面并居中显示原因。
//   右键：WebView2 默认菜单已关，右键事件转给宿主，让 DOpus 显示查看器菜单
//        （其中有插件加的「编码」子菜单）。
(function () {
  var host = window.chrome && window.chrome.webview
  if (!host) return

  host.addEventListener('message', function (event) {
    var raw = event.data
    if (typeof raw !== 'string') return
    var pos = raw.indexOf('|')
    var type = pos < 0 ? raw : raw.slice(0, pos)
    if (type !== 'docerror') return
    var text = pos < 0 ? '' : raw.slice(pos + 1)

    var doc = document.getElementById('doc')
    if (!doc) return
    document.body.classList.add('virtual')
    document.body.classList.remove('dev')
    doc.textContent = ''
    doc.style.transform = ''
    var box = document.createElement('div')
    box.className = 'mdv-error'
    box.textContent = text
    doc.appendChild(box)

    var hide = ['tools', 'sb', 'find', 'outline']
    for (var i = 0; i < hide.length; i++) {
      var el = document.getElementById(hide[i])
      if (el) el.hidden = true
    }
  })

  document.addEventListener('contextmenu', function (e) {
    e.preventDefault()
    host.postMessage('ctxmenu|')
  })
})()
