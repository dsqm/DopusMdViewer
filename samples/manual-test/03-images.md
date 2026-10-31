# 图片

> 预期：
> - 同目录 / 子目录图片显示；块高会随图片加载后回填。
> - 上级目录与绝对路径不显示（只有 alt 文字）。
> - data URI 显示。
> - 网络图默认弹原生确认框；选“是”后加载（设置里可切三档）。

## 同目录

![同目录图片](pic.png)

## 子目录

![子目录图片](sub/pic2.png)

## 上级目录（预期不显示）

![上级目录图片](../up.png)

## 绝对路径（预期不显示）

![绝对路径图片](C:/Users/momo/Documents/ai/Directory Opus plugins/DopusMdViewer/samples/manual-test/pic.png)

## data URI（预期显示）

![data URI](data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAYCAIAAADF1mwTAAABZElEQVR4nGP8//8/w1AGTAxDHDAxDHHAAmd1B1xiGDqgdIPeMIkBJoaR7IG8ZdpYxc2DxRiGdAyYB4sy0D8TEwm4BVjcs2XYeZg/PP8FERGR43DLlGbnZr68592ZTW+sI8XZOJhCG5T2z3uGLM4wSGLAIVHyxpEPyyvv3jn5kYUVpN3QS/jQ4hfLq+6aBYIC/ujyl79+/FvdcA9NfLB4QFaH5+axjwwMDHfPfP73D1SLH1z4XEiG3TxYjI2TGVklLvEB9gAzCyOEwcgIQgwMDH5l8gwMDOe2vEFrlOASH2APPL3xTcWMn4GBQdWCn4EB5AMJFa6bRz6ysDEys8L9xsjIiEV8UGTi/fOeeeXLGnkLP73x7e+ffwwMDBe2v4nuVH714MfPr3+ZWRn//v7/5NrXoGoFTHFaeIAR3hodbUoMDGBiGOKAiWGIAyaGIQ6YGIY4YGIY4oBxtFM/wIBpoB1AKQAAkJmHdRg4Qs8AAAAASUVORK5CYII=)

## 网络图（默认弹确认框）

![网络图片](https://httpbin.org/image/png)
