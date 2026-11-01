// docerror|<文本>：打开失败时清空旧页面并居中显示原因。
// app.js 只处理已知消息类型，这里单独挂一条监听；不参与它的虚拟滚动状态，
// 下一次成功打开时 applyIndex 会清掉这里插入的内容。
(function () {
  var host = window.chrome && window.chrome.webview
  if (!host) return
  host.addEventListener('message', function (event) {
    var raw = event.data
    if (typeof raw !== 'string') return
    var pos = raw.indexOf('|')
    if (pos < 0 || raw.slice(0, pos) !== 'docerror') return
    var doc = document.getElementById('doc')
    if (!doc) return
    document.body.classList.add('virtual')
    document.body.classList.remove('dev')
    doc.textContent = ''
    doc.style.transform = ''
    var box = document.createElement('div')
    box.className = 'mdv-error'
    box.textContent = raw.slice(pos + 1)
    doc.appendChild(box)
    var hide = ['sb', 'find', 'outline', 'tools']
    for (var i = 0; i < hide.length; i++) {
      var el = document.getElementById(hide[i])
      if (el) el.hidden = true
    }
  })
})()
