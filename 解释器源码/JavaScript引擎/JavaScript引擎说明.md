# JavaScript 引擎（sillage.js）· 网页端

> 单文件、无依赖、ES2018，1,809 行。在浏览器里挂全局 `SILLAGE`。

---

## 1. 它是什么

| 项 | 说明 |
|---|---|
| 语言 | ES2018（不需要构建步骤，`<script>` 直接引） |
| 依赖 | 无（不依赖 DOM，可在 Node 里跑逻辑） |
| 规模 | 1,809 行 |
| 全局 | `window.SILLAGE` |
| 主要 API | `SILLAGE.create(host)` → `interp.eval(src, name)` / `interp.call(nom, args)` / `interp.globals()` |

## 2. 用在哪

1. `网页端/index.html` —— 内嵌了这个引擎，`file://` 直接打开就能在浏览器里写 SILLAGE；
2. 想让网页做「本地引擎 + 远端引擎」对照时，它就是那个本地引擎。

## 3. 最小用法

```html
<script src="sillage.js"></script>
<script>
  const interp = SILLAGE.create({
    "carnet.lister": (nom) => [],
    "horloge.maintenant": () => new Date().toISOString().slice(0, 19),
  });
  const v = interp.eval('que x vaille 2 plus 3\nrends x', '<demo>');
  console.log(v);                     // 5
</script>
```

## 4. 与另两份引擎的关系

| | JS 引擎 | C 引擎 | Python 引擎 |
|---|---|---|---|
| 运行位置 | 浏览器 | Windows 可执行 | Python 进程 |
| 宿主服务 | 注入式（传 `host` 对象） | 内置 | 注入式 |
| 零参调用 `f de ()` | 支持 | 不支持 | 支持 |
| 用途 | 网页演示与交互 | 日常运行 / RPC | 语义基准 / 对照 |

三份引擎共用 `解释器源码/对比测试/sillage.tests.json` 的期望值；
行为不一致时**以 Python 参考实现为准**，然后三份一起改。

## 5. 注意

* 数字同样是定点：内部按 `1e-9` 单位整数处理，`0.1 plus 0.2` 得 `0.3`；
* 文本按字素簇计长（用 `Intl.Segmenter` 可用时启用，否则退化到码点近似）；
* 不提供 `tokens` / `ast` 命令，也不做文件读写——它是浏览器里的库，不是 CLI。
