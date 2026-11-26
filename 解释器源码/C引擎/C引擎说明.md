# C 引擎（sillage_c.c）· 独立运行，无任何宿主注入

> 单文件 C11（5,585 行），编译产物 `可执行程序/sillage_c.exe`（261 KB）。
> **它不调用 Python、不链接任何第三方库、不加载任何脚本运行时。**

---

## 1. 独立性怎么证明

三条硬证据，都能自己复核。

### 1.1 源码里没有宿主接口

```powershell
cd 解释器源码\C引擎
Select-String -Path sillage_c.c -Pattern 'Python.h|Py_|libpython|LoadLibrary|GetProcAddress|dlopen'
```

**输出为空**。`#include` 只有 C 标准库与 Windows 系统头：

```
<stdio.h> <stdlib.h> <string.h> <stdarg.h> <setjmp.h> <time.h>
<math.h> <wchar.h> <locale.h> <io.h> <direct.h> <sys/stat.h> <windows.h>
```

### 1.2 可执行文件只依赖系统 DLL

`可执行程序/sillage_c.exe` 的 PE 导入表（共 10 个 DLL）：

```
KERNEL32.dll
api-ms-win-crt-runtime-l1-1-0.dll      api-ms-win-crt-stdio-l1-1-0.dll
api-ms-win-crt-string-l1-1-0.dll       api-ms-win-crt-math-l1-1-0.dll
api-ms-win-crt-heap-l1-1-0.dll         api-ms-win-crt-filesystem-l1-1-0.dll
api-ms-win-crt-time-l1-1-0.dll         api-ms-win-crt-environment-l1-1-0.dll
api-ms-win-crt-private-l1-1-0.dll
```

全是 Windows 自带的 UCRT + Kernel32。**没有 pythonXX.dll，没有 vcruntime/msvcp，
没有任何 GCC 运行库**；导入函数里没有 `Py_*`，也没有 `LoadLibrary` / `GetProcAddress`。

### 1.3 把 Python 引擎整个移走，C 引擎照样全功能

实测：把 `解释器源码/Python引擎/` 改名后——

| 检查 | 结果 |
|---|---|
| `sillage_c.exe test` | 360 passed, 0 failed |
| 12 个入门示例 `run` | 12 / 12 通过 |
| 四个程序 `handle` | 4 / 4 返回 200 |
| `--server` RPC 6 条命令 | 全部正确 |
| 浏览器 `validate.py`（C 引擎） | 56 / 56 通过 |
| 浏览器 10 个页面真实窗口 | 10 / 10 渲染成功 |

（恢复文件夹后同一套检查仍然全通过。）

---

## 2. 那「宿主服务」是什么

页面和程序里能用的 `carnet.*`、`horloge.*`、`voix.*`、`atelier.*` **不是**宿主注入，
而是 C 引擎内部一张函数表（`HOTES[]`），全部用 C 实现：

| 名字 | C 函数 | 干什么 | 实现位置 |
|---|---|---|---|
| `carnet.lister` | `b_carnet_lister` | 读集合 | 自带 JSON 存储（单文件，原子替换） |
| `carnet.lire` | `b_carnet_lire` | 读集合原值 | 同上 |
| `carnet.écrire` | `b_carnet_ecrire` | 覆盖写 | 同上 |
| `carnet.ajouter` | `b_carnet_ajouter` | 追加 | 同上 |
| `carnet.compter` | `b_carnet_compter` | 计数 | 同上 |
| `carnet.vider` | `b_carnet_vider` | 清空 | 同上 |
| `carnet.retirer` | `b_carnet_retirer` | 按 id 删 | 同上 |
| `carnet.fusionner` | `b_carnet_fusionner` | 按 id 合并 | 同上 |
| `horloge.maintenant` | `b_horloge` | 本地时间 | `time()` + `localtime_s` |
| `voix.dire` / `voix.signaler` | `b_voix` | 输出文本 | 直接返回文本（不打印，RPC 模式保证 stdout 只有 JSON） |
| `atelier.lancer` | `b_atelier_lancer` | 调另一个 `.sil` 的函数 | 自建解释器缓存（路径 + mtime） |
| `atelier.évaluer` | `b_atelier_evaluer` | 求值一段源码 | 复用自身分析器 + 求值器 |
| `atelier.noms` | `b_atelier_noms` | 列出程序名 | 扫目录 |

也就是说：**语言运行时里没有「宿主」这个概念**——需要的能力都由引擎自己实现。

---

## 3. 与 Python 引擎的关系

| | C 引擎 | Python 引擎 |
|---|---|---|
| 作用 | **运行引擎**（日常使用、浏览器、RPC 服务） | **参考实现**（语义基准，便于阅读与对照） |
| 依赖 | 无（只要 Windows 系统 DLL） | Python 3.9+ 标准库 |
| 存储 | 内置（`数据/sillage.json`，原子写） | 由脚本注入宿主函数实现 |
| 时间 / 跨程序调用 | 内置 | 由脚本注入宿主函数实现 |
| 对外接口 | CLI / REPL / JSON-RPC | CLI（`test/run/ast/tokens/repl`） |
| 用例数 | 360 条内置断言 | 374 条（多覆盖参考实现特有路径） |

两者对同一段程序输出**逐字段相等**，用 `解释器源码/对比测试/双引擎对比.py` 可以逐条核对：

```powershell
cd 解释器源码\对比测试
python -X utf8 双引擎对比.py
```

---

## 4. 自己编译

```powershell
# 在分发包根目录
.\编译.ps1 -Rebuild
# 或者直接：
zig cc -std=c11 -O2 -o 可执行程序\sillage_c.exe 解释器源码\C引擎\sillage_c.c
```

`编译.ps1` 会依次找 Zig → MSVC → gcc/clang；也可以用 `编译.sh`（Linux/macOS/WSL）。
因为只有一个 `.c` 文件、只 include 标准库与 `windows.h`，任何支持 C11 的编译器都能过。

---

## 5. 已知的 C 引擎限制

1. **不支持零参调用** `f de ()`（Python / JS 引擎支持）——定义可以，调用不行；
   工程里统一用占位参数 `f de 0`；
2. 没有 `tokens` 子命令（只有 `ast`）；
3. 词法/语法阶段失败时，RPC 回包的 `id` 是 `null`（还没读到 id 就出错了）；
4. 只能用 `ReadConsoleW` 读控制台 → REPL 仅 Windows（`run`/`call`/`ast`/`--server` 在其它平台需自己重编）。
