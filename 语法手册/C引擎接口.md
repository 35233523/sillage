# SILLAGE C 引擎 · 接口与实现说明

> 本文是 **C 引擎（`sillage_c.c` → `可执行程序/sillage_c.exe`）** 的接口契约：
> 命令行、REPL、JSON-RPC、内置服务签名、语义对齐要求。
> 想看「它为什么是独立运行、没有宿主注入」，请看 `解释器源码/C引擎/C引擎说明.md`。

---

## 1. 文件与构建

```
解释器源码/C引擎/sillage_c.c     C 引擎（词法/语法/求值/内置/原生服务/JSON/RPC/CLI），单文件
编译.ps1 / 编译.sh               构建脚本（Zig → MSVC → gcc/clang）
可执行程序/sillage_c.exe         产物（261 KB）
```

```powershell
.\编译.ps1 -Rebuild
# 等价于：
zig cc -std=c11 -O2 -o 可执行程序\sillage_c.exe 解释器源码\C引擎\sillage_c.c
```

编译器只要支持 C11；源码只 include C 标准库与 `<windows.h>`，不链接任何第三方库。

---

## 2. 命令行

```
sillage_c.exe                            交互式解释器（双击等价，无参数）
sillage_c.exe repl                       同上（显式）
sillage_c.exe test                       内置自测，打印 "sillage_c: N passed, M failed"
sillage_c.exe run <文件.sil> [JSON]      先打印 affiche，再打印模块结果 JSON
sillage_c.exe call <文件.sil> <函数> [JSON]   调函数，打印返回值 JSON
sillage_c.exe ast <文件.sil>             语法树 JSON（一行）
sillage_c.exe --server [--store <JSON>]  RPC 服务模式（stdin/stdout 逐行 JSON）
```

`--store` 缺省：

| 模式 | 默认存储 | 由此推出的「项目根」 |
|---|---|---|
| `--server` | `<exe目录>/../miku/zici.json` | `<exe目录>/..` 的上一级 |
| CLI / REPL | `<exe目录>/数据/sillage.json` | `<exe目录>` |

`atelier.lancer` 按「项目根」解析文件名，所以跑跨程序示例时建议统一传
`--store 数据\sillage.json`（相对分发包根），让项目根 = 分发包根。

---

## 3. REPL

命令：`:help`、`:reset`、`:load <文件.sil>`、`:quit`（`:q` 同义）。

* 块未闭合时提示 `...` 续行；
* 错误只打印一行 `{"défaut": …}` 后回到提示符，**不退出**；
* 块括号可用键盘 `{` `}` 代替 `⟦` `⟧`（**仅 REPL**，文件里不行）；
* 控制台输入用 `ReadConsoleW` 读 UTF-16 再转 UTF-8，中文不乱码。

---

## 4. JSON-RPC（逐行 JSON，UTF-8，不换行）

请求：

```json
{"id":1,"cmd":"ping"}
{"id":2,"cmd":"eval","source":"que x vaille 1\nx","name":"<module>"}
{"id":3,"cmd":"call","file":"D:/…/示例程序/程序/calc.sil","func":"handle","arg":{}}
{"id":4,"cmd":"names","file":"D:/…/示例程序/程序/demo.sil"}
{"id":5,"cmd":"store"}
{"id":6,"cmd":"quit"}
```

响应：

```json
{"id":2,"ok":true,"value":2}
{"id":3,"ok":false,"error":{"étape":"nom","ligne":null,"note":"geste inconnu: x"}}
```

细节：

* 数值/文本/序列/记录/布尔/空 与 Python 版的外部表示完全一致；中文不转义；
* **服务模式不打印 `affiche`**（内部置 `g_muet=1`），保证 stdout 只有 JSON；
* 词法/语法阶段失败时回包的 `id` 是 `null`（还没读到 id 就出错了）；调用方应按
  「串行请求 + `id` 为 null 且 `ok:false`」处理（`运行支撑/engine.py` 已如此实现）。

---

## 5. 原生服务（引擎自己实现，不是宿主注入）

| 名字 | 签名 | 语义 |
|---|---|---|
| `carnet.lister` | (名) → 序列 | 读集合，缺省 `‹›` |
| `carnet.lire` | (名) → 任意 | 读集合原值，缺省 `VIDE` |
| `carnet.écrire` | (名, 值) → 值 | 覆盖并落盘 |
| `carnet.ajouter` | (名, 记录) → 记录 | 追加并落盘 |
| `carnet.compter` | (名) → 数字 | 条数 |
| `carnet.vider` | (名) → 0 | 清空 |
| `carnet.retirer` | (名, id) → 数字 | 按 id 删除，返回删掉条数 |
| `carnet.fusionner` | (名, id, 字段记录) → 记录 | 按 id 合并字段 |
| `horloge.maintenant` | → 文本 | 本地时间 `YYYY-MM-DDTHH:MM:SS` |
| `voix.dire` / `voix.signaler` | (文本) → 文本 | 输出/日志（RPC 下静默） |
| `atelier.lancer` | (文件, 函数, 参数) → 任意 | 载入另一个 `.sil` 并调函数 |
| `atelier.évaluer` | (源码) → 任意 | 把源码当模块求值 |
| `atelier.noms` | → 序列 | 列出可调用的程序名 |

`atelier.lancer` 的文件名解析：含 `/` 或 `\` 时按**项目根**相对路径；否则在
`juanzong/`、`huapu/`、`程序/`、`示例/` 下递归找同名 `.sil`，最后再试项目根。
解释器按「路径 + 修改时间」缓存。

JSON 存储：单文件；写操作原子替换（临时文件 + rename）；进程内常驻内存。

---

## 6. 语义对齐要求

以 Python 参考实现（`解释器源码/Python引擎/sillage.py`）为准，重点：

- 定点数（`1e-9` 单位、最多 9 位小数、`|x| ≤ 1e15`、整数不带小数点）；
- 文本按**字素簇**、下标 1 起；`bribe` 1 起闭区间、越界夹紧；
- `Suite` **不可变**；`Fiche` 保持**插入序**；
- 关键字别名与运算符符号别名全覆盖；
- 语句/块/作用域/`rends`/模块结果三条规则/`silence`；
- 错误 `Défaut{étape,ligne,note}`，étape ∈ `lexique|syntaxe|forme|nom|calcul|hôte`；
- 循环 200 万次、递归 400 层护栏；
- 18 个内置：`mesure` `en-texte` `en-nombre` `égalise` `abaissé` `haussé` `découpe`
  `recoud` `renferme` `clefs` `teneurs` `ajoute-fin` `bribe` `ordonne` `total` `tordu` `fige` `absolu`。

对齐验证：

```powershell
.\可执行程序\sillage_c.exe test                              # 360 passed, 0 failed
python -X utf8 解释器源码\Python引擎\sillage.py test          # 374 passed, 0 failed
python -X utf8 解释器源码\对比测试\双引擎对比.py              # 374 / 374 一致
```

---

## 7. 已知实现差异

| 项 | C 引擎 | Python / JS |
|---|---|---|
| 零参调用 `f de ()` | **不支持**（定义可以，调用是语法错） | 支持 |
| `tokens` 子命令 | 无（只有 `ast`） | 有 |
| 错误回包的 `id` | 词法/语法阶段为 `null` | 不涉及 |
| REPL 平台 | 仅 Windows（`ReadConsoleW`） | 跨平台 |
