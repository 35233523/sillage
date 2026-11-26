# Python 引擎（sillage.py）· 参考实现

> 单文件，仅用标准库，1,726 行。**它是语义基准**：C 引擎与它逐字段对齐，
> 遇到行为分歧以本文件为准（先改这里，再改 C 与 JS）。

---

## 1. 它是什么

```
$ python -X utf8 解释器源码\Python引擎\sillage.py test
sillage: 374 passed, 0 failed
```

| 项 | 说明 |
|---|---|
| 语言 | Python 3.9+（无第三方包） |
| 规模 | 1,726 行 |
| 依赖 | 无（只 import 标准库） |
| 对外 API | `create(host)` → `Interpreter`：`eval(source, name)` / `call(nom, args)` / `globals()` |
| 命令行 | `test` / `run <文件> [JSON]` / `ast <文件>` / `tokens <文件>` / `repl` |

## 2. 命令行

```powershell
cd 解释器源码\Python引擎
python -X utf8 sillage.py test                  # 374 条断言
python -X utf8 sillage.py run  ..\..\示例程序\入门\01-hello.sil
python -X utf8 sillage.py ast  ..\..\示例程序\入门\02-arithmetique.sil
python -X utf8 sillage.py tokens ..\..\示例程序\入门\02-arithmetique.sil
python -X utf8 sillage.py repl
```

`tokens` 是 Python 引擎独有的（C 引擎只有 `ast`）；`ast` 与 `tokens` 输出的是
**内部表示**——数字已经化成 `1e-9` 单位的整数，所以 `2` 显示成 `2000000000`，这是正常的。

## 3. 与 C 引擎的关键差异

| | Python 引擎 | C 引擎 |
|---|---|---|
| 宿主服务（`carnet.*` / `horloge` / `atelier`） | **由调用方注入**（字典 `host`），CLI 下没有宿主 | **内置在引擎里** |
| 零参调用 `f de ()` | 支持 | 不支持（语法错） |
| `tokens` 子命令 | 有 | 无 |
| 性能 | 慢 | 快约 10–30 倍 |
| 用途 | 读代码、对语义、做对照测试 | 日常运行、浏览器、RPC |

### 3.1 CLI 下为什么 `carnet.*` 会报错

```text
$ python sillage.py run x.sil        # 文件里写了 carnet.lister "posts"
{"défaut": {"étape": "nom", "ligne": null, "note": "nom inconnu: carnet.lister"}}
```

这是**预期行为**：Python 引擎的宿主是注入式的，裸 CLI 没有注入任何宿主。
要跑带宿主的程序，用 `解释器源码/对比测试/` 里的脚本，或直接用 C 引擎。

## 4. 作为库使用

```python
import importlib.util
spec = importlib.util.spec_from_file_location("sillage", r"解释器源码\Python引擎\sillage.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

carnet = {"posts": []}
host = {
    "carnet.lister": lambda n: carnet.get(n, []),
    "carnet.écrire": lambda n, v: (carnet.setdefault(n, []).append(v), v)[1],
    "carnet.compter": lambda n: len(carnet.get(n, [])),
    "horloge.maintenant": lambda: "2026-11-26T00:00:00",
}
interp = mod.create(host)
print(interp.eval('que x vaille 2 plus 3\nrends x'))     # 内部值
print(mod._sortir(interp.eval('fige de ⟬a ⟼ 1⟭')))       # 转外部表示
```

> 注意 `eval()` 返回的是**内部值**：数字是 `1e-9` 单位的整数。对外传递前要过
> `mod._sortir(v)`（内部 → 外部）或 `mod._entrer(v)`（外部 → 内部）。

## 5. 它的定位

* **不要**把它当「C 引擎的替代品」——正式运行请用 C 引擎；
* 它存在的意义是：语义可读、可改、可对照，遇到歧义时它是裁判；
* 三份引擎的用例都从 `对比测试/sillage.tests.json` 来，同一批源码 → 同一批期望值。
