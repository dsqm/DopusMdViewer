# 公式与图表

Mermaid 只在本文出现 ```mermaid 时才会去拉 `mermaid.js`（5.0 MiB）。

## 流程图

```mermaid
flowchart LR
  A[DOpus 查看器窗口] -->|SetParent 借用| B[隐藏宿主 HWND]
  B --> C[WebView2 Controller]
  C --> D[shell.html]
  D -->|二进制帧| E[区块正文]
  D -->|postMessage| F[皮肤 / 字体 / 状态]
```

## 时序图

```mermaid
sequenceDiagram
  participant O as DOpus
  participant V as viewer_window
  participant H as webhost
  participant J as shell.html
  O->>V: DVPLUGINMSG_LOAD
  V->>H: ensure controller
  H->>J: postMessage(doc)
  J-->>V: event(rendered)
```

## 行内公式

能量与动量：$E^2 = (pc)^2 + (m_0 c^2)^2$，当 $v \ll c$ 时退化为 $E \approx m_0c^2 + \frac{1}{2}m_0v^2$。

## 独立公式

$$
\mathrm{Fenwick}(i) = \sum_{k=i-\mathrm{lowbit}(i)+1}^{i} w_k
$$

$$
T(n) = \begin{cases} \Theta(1) & n \le 64 \\ 2T(n/2) + \Theta(n) & n > 64 \end{cases}
$$

## 未知图类型（应降级为源码块）

```mermaid
nonsenseDiagram
  X --> Y
```
