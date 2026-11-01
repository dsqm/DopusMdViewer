// 手写 UI 扩展（不属于 app.js bundle）：
//   docerror|<文本>  打开失败：清空旧页面并居中显示原因。
//   enc|<代码页>     宿主回填当前选择的编码（0 = 自动）。
//   编码下拉框       选择后发 encode|<代码页>，宿主按该编码重开本文档。
(function () {
  var host = window.chrome && window.chrome.webview
  if (!host) return

  var select = document.getElementById('enc-select')

  function showError(text) {
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

    // 错误页保留编码下拉框：判错时还能改；搜索/大纲没有意义，藏起来。
    var tools = document.getElementById('tools')
    if (tools) tools.hidden = false
    var f = document.querySelector('#tools [data-act="find"]')
    var o = document.querySelector('#tools [data-act="outline"]')
    if (f) f.hidden = true
    if (o) o.hidden = true

    var hide = ['sb', 'find', 'outline']
    for (var i = 0; i < hide.length; i++) {
      var el = document.getElementById(hide[i])
      if (el) el.hidden = true
    }
  }

  host.addEventListener('message', function (event) {
    var raw = event.data
    if (typeof raw !== 'string') return
    var pos = raw.indexOf('|')
    var type = pos < 0 ? raw : raw.slice(0, pos)
    var rest = pos < 0 ? '' : raw.slice(pos + 1)

    if (type === 'docerror') {
      showError(rest)
      return
    }

    if (type === 'enc') {
      if (select) select.value = String(parseInt(rest, 10) || 0)
      return
    }

    if (type === 'index') {
      // 上一次失败把工具条按钮藏了，成功打开后恢复。
      var f = document.querySelector('#tools [data-act="find"]')
      var o = document.querySelector('#tools [data-act="outline"]')
      if (f) f.hidden = false
      if (o) o.hidden = false
    }
  })

  if (select) {
    select.addEventListener('change', function () {
      host.postMessage('encode|' + select.value)
    })
  }
})()
