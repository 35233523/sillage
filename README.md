# SILLAGE（航迹）语法手册 · 学习版

> 版本 **v1.0（2026-11）** ｜ 适用引擎：`可执行程序/sillage_c.exe`（C 实现，独立运行）、`解释器源码/Python引擎/sillage.py`（Python 参考实现）、`解释器源码/JavaScript引擎/sillage.js`（浏览器实现）
>
> 本手册面向第一次接触 SILLAGE 的人：从双击解释器开始，按章节学完词法、值、表达式、语句、函数、
> 内置函数、宿主服务，最后用 12 课示例、24 道练习和 20 个坑收尾。
> **手册里的每个示例都在本机 C 引擎上真跑过**，输出照抄实测（逐条记录见 `数据/手册示例.json`，79 条）。

---

## 目录

**第一部分 · 认识它**
1. [这是什么语言](#1-这是什么语言)
2. [五分钟上手](#2-五分钟上手)
3. [目录结构、构建与自测](#3-目录结构构建与自测)

**第二部分 · 语法**
4. [词法：一行一句](#4-词法一行一句)
5. [值：数字、文本、序列、记录、布尔、空](#5-值数字文本序列记录布尔空)
6. [表达式与运算符](#6-表达式与运算符)
7. [语句：绑定、判断、循环、返回](#7-语句绑定判断循环返回)
8. [函数：定义、参数、递归、作用域](#8-函数定义参数递归作用域)

**第三部分 · 标准能力**
9. [内置函数 18 个](#9-内置函数-18-个)
10. [宿主服务：集合、时间、输出、跨程序调用](#10-宿主服务集合时间输出跨程序调用)
11. [错误模型与调试](#11-错误模型与调试)
12. [模块结果的三条规则](#12-模块结果的三条规则)

**第四部分 · 用它**
13. [四种交互方式与完整命令参考](#13-四种交互方式与完整命令参考)
14. [12 课示例逐段讲解（真实输出）](#14-12-课示例逐段讲解真实输出)

**第五部分 · 学会它**
15. [四周学习路线](#15-四周学习路线)
16. [24 道练习与答案要点](#16-24-道练习与答案要点)
17. [20 个常见错误与陷阱](#17-20-个常见错误与陷阱)
18. [附录：全部关键字 / 运算符 / 内置 / 宿主](#18-附录全部关键字-运算符-内置-宿主)
19. [已知限制](#19-已知限制)
20. [协议、参与与致谢](#20-协议参与与致谢)

---

# 第一部分 · 认识它

## 1. 这是什么语言

SILLAGE（法语「航迹」）是一门**面向学习者的脚本语言**，全部设计只围绕一句话：

> **程序是一段法语陈述：先说做什么，再说对什么。**

```sillage
que prix vaille 12.5
affiche prix fois 2
```

读出来是「令 prix 等于 12.5；显示 prix 乘 2」。运行结果：

```text
25
```

没有分号、没有大括号、没有 `int main`、没有 `import`、没有包管理。

### 1.1 六个设计取舍

| 维度 | SILLAGE 的选择 | 为什么这样选 |
|---|---|---|
| **值模型** | 只有一种数字 `Nombre`（十进制定点，内部按 `1e-9` 存整数，最多 9 位小数，**没有 int/float 之分**）；文本按**字素簇**计长、下标 **1 起**；序列 `Suite` **不可变**；记录 `Fiche` 保持**插入序** | 初学者不必区分整数与浮点，`0.1 plus 0.2` 正好是 `0.3`；中文一个字算一个长度 |
| **错误模型** | 只有一个异常 `Défaut{étape, ligne, note}`；`étape` 是六个英文类别之一；解析失败也带 `note`；命令行打印**一行 JSON** | 一种错误，一种看法；一行 JSON 能直接贴进 issue |
| **模块结果** | 第一条真正执行的**顶层** `rends` 立即结束模块并给出结果；否则取最后一条顶层表达式；再否则 `VIDE` | 「一个裸表达式就是一段完整程序」，REPL 与脚本行为一致 |
| **宿主命名** | **点分名 + 纯并置实参**：`carnet.lister "posts"`、`atelier.lancer "calc.sil" "handle" ‹req›` | 与语言自身的介词调用形成对照，一眼看出「这是宿主，不是我的函数」 |
| **内置语义** | 18 个**介词式**法文名：`mesure de x`、`découpe de t par sep`、`renferme de f dans k` | 参数角色写进代码：谁是容器、谁是被查项，不用查文档 |
| **句式** | 自然语序、**动词在前**：`que x vaille e`、`donne e à x`、`on définit f de a comme` | 读代码像读句子；缩进不是语法，块用 `⟦ … ⟧` 明确标出 |

### 1.2 它不是什么

* **不是通用工业语言**：没有线程、没有网络、没有包管理，不适合写大型服务；
* **不是纯玩具**：它有跨程序调用、本地集合存储、JSON-RPC 服务、专用浏览器，
  可以用来写完整的小系统（仓库内配套的国风插画平台就是 27,163 行 `.sil`）；
* **不追求性能**：解释执行，护栏是「循环 200 万次、递归 400 层」。

### 1.3 三份引擎，一套语义

| 文件 | 语言 | 用途 | 调用方式 |
|---|---|---|---|
| `解释器源码/C引擎/sillage_c.c` → `可执行程序/sillage_c.exe` | C11，单文件 | **运行引擎**（推荐） | CLI / REPL / JSON-RPC |
| `解释器源码/Python引擎/sillage.py` | Python 3.11，仅标准库 | **参考实现**（语义基准） | `python sillage.py test\|run\|ast\|tokens\|repl` |
| `解释器源码/JavaScript引擎/sillage.js` | ES2018，无依赖 | 网页端 | 浏览器里挂全局 `SILLAGE` |

三份引擎对同一段程序输出**逐字段相等**；本手册以 C 引擎为准。

---

## 2. 五分钟上手

### 2.1 双击就能学

双击 `可执行程序/sillage_c.exe`（或命令行运行 `sillage_c.exe`），出现：

```text
  SILLAGE C 引擎 · 交互式解释器（v1.0）
  输入语句回车求值；以 : 开头是命令。
  示例：que prix vaille 12.5   然后   affiche prix fois 2
  命令：:help 帮助   :reset 清空会话   :load <文件> 载入程序   :quit 退出

sillage>
```

输入一句、回车，立即看到结果：

```text
sillage> que prix vaille 12.5
sillage> affiche prix fois 2
25
sillage> 1 plus 2 fois 3
7
sillage> "你好" plus "，世界"
"你好，世界"
```

* 绑定语句（`que …`）不回显；表达式句回显 JSON；
* 块要分多行，解释器自动出现续行提示 `...`；
* 块括号可以直接打键盘上的 `{` `}`，解释器等价成 `⟦` `⟧`；
* 出错**不退出**，打印一行 `{"défaut": …}` 后回到 `sillage>`。

### 2.2 第一个脚本

新建 `hello.sil`（UTF-8）：

```sillage
que nom vaille "小明"
affiche "你好，" plus nom plus "！"
affiche 3 fois 4 plus 1
```

在分发包根目录运行：

```powershell
.\可执行程序\sillage_c.exe run hello.sil
```

实测输出：

```text
你好，小明！
13
null
```

最后一行 `null` 是**模块结果**：文件里没有 `rends`，最后一条顶层语句是 `affiche`（值 `VIDE`），
所以模块结果是 `null`。这就是 §12「模块结果三条规则」的全部内容。

### 2.3 三种最常用的跑法

| 场景 | 命令 | 说明 |
|---|---|---|
| 边写边试 | `.\可执行程序\sillage_c.exe` | 交互式 REPL（双击等价） |
| 跑一个文件 | `.\可执行程序\sillage_c.exe run 文件名.sil` | 先输出 `affiche`，最后打印模块结果 |
| 被别的程序调用 | `.\可执行程序\sillage_c.exe --server --store 数据\sillage.json` | 逐行 JSON-RPC（浏览器用这个） |

> **注意**：跨程序调用 `atelier.lancer` 需要知道「项目根」。
> 在分发包里请加 `--store 数据\sillage.json`，或从分发包根目录运行；
> 否则按项目根找不到 `示例程序/…` 里的文件，报
> `{"étape":"hôte","note":"atelier.lancer: program-not-found: …"}`。

---

## 3. 目录结构、构建与自测

### 3.1 不需要安装

`可执行程序/sillage_c.exe` 是独立程序：不依赖 Python、数据库、网络，
只需要能写文件的目录（集合数据默认存 `数据/sillage.json`）。

### 3.2 分发包目录

```text
航迹语言开源版/
  解释器源码/     C引擎/（sillage_c.c）  Python引擎/（sillage.py）
                  JavaScript引擎/（sillage.js）  对比测试/（tests.json + 双引擎对比.py）
                  每个引擎文件夹里各有一份「X引擎说明.md」
  可执行程序/     sillage_c.exe              ← 双击进入 REPL
  浏览器源码/     航迹浏览器.py  core.py  pagehost.py  paizhao.py
                  页面/*.sil（10 个页面）  页面模板.sil  PAGE_SPEC.md  README.md
  运行支撑/       _layout.py  langdata.py  engine.py
  语法手册/       语法手册.md（本文件）速查表.md 别名表.md C引擎接口.md
  示例程序/       入门/01..12.sil   程序/calc.sil forum.sil zoo.sil demo.sil
  数据/           sillage.json（集合存储）手册示例.json 交互方式自测.json
  文档/           交互方式自测.json
  网页端/         index.html（网页语言演示）
  截图/           浏览器与 REPL 截图
  编译.ps1  编译.sh   从 解释器源码/C引擎/sillage_c.c 重新编译 可执行程序/sillage_c.exe
  使用说明.md   README.md   一页纸总览
  语言信息.json            语言元数据（扩展名 / 入口 / 主色 / 地址 scheme）
  许可证.txt               MIT
```

### 3.3 重新编译 C 引擎

需要任意 C11 编译器（推荐 Zig，自带 clang + mingw）：

```powershell
.\编译.ps1              # 已存在且比源码新则跳过
.\编译.ps1 -Rebuild     # 强制重编
.\编译.ps1 -Clean       # 清掉旧产物
```

等价命令：

```powershell
zig cc -std=c11 -O2 -o 可执行程序\sillage_c.exe 解释器源码/C引擎/sillage_c.c
```

### 3.4 自测

```powershell
.\可执行程序\sillage_c.exe test              # C 引擎内置断言
python -X utf8 解释器源码/Python引擎/sillage.py test     # Python 参考实现
```

实测输出：

```text
sillage_c: 360 passed, 0 failed
```

---

# 第二部分 · 语法

## 4. 词法：一行一句

### 4.1 源文件

* UTF-8 文本，扩展名 **`.sil`**；
* **一行一句**，`;` **不是**分隔符（写 `;` 会报语法错）；
* 空行随便加；
* 字符串**不能跨行**，要换行写 `\n`；
* 块的 `⟦` **必须与引导词同一行**：`si … alors ⟦`、`pour chaque … ⟦`、`on définit … comme ⟦`。
  换行再写 `⟦` 是语法错——新手最常踩的坑。

### 4.2 注释

`‖` 或 `#` 起到行尾，整行/行尾都合法：

```sillage
‖ 这是整行注释
# 这也是整行注释
que x vaille 1   # 这是行尾注释
x
```

实测输出：`1`（注释被完全忽略）。

**字符串里的 `‖`、`#` 不是注释**：

```sillage
«原样 # ‖ 文本»
```

实测输出：`"原样 # ‖ 文本"`。颜色字面量 `"#2f6b5e"` 同理——那是文本。

### 4.3 标识符

* 字母开头，可含**字母、数字、`_`、`-`、`'`**；
* 所以 `tête-à-tête`、`aujourd'hui`、`ajoute-fin` 都是合法名字；
* **副作用**：`x-1` 会被读成一个名字，减法必须写 `x moins 1` 或 `x - 1`（两侧留空格）。

### 4.4 数字

| 写法 | 含义 | 备注 |
|---|---|---|
| `42` | 整数 | 内部存 `42 × 10⁹` |
| `3.5` | 小数 | 最多 **9 位**小数，超出四舍五入 |
| `0.000000001` | 最小正数 | 即 `1e-9` |
| `0 moins 7` | 负数 | 一元 `moins`；也可写 `-7` |
| `1e3` | **不是** 1000 | 没有科学计数法：读成数字 `1` 紧跟名字 `e3` |

* 绝对值上限 `10¹⁵`，超出报 `calcul`；
* 整值不显示 `.0`：`7 sur 2` → `3.5`，`8 sur 2` → `4`。

### 4.5 文本

两种字面量，区别只在**转义**：

| 写法 | 转义 | 例子 | 结果 |
|---|---|---|---|
| `"…"` | 支持 `\n` `\t` `\r` `\"` `\\` | `"第一行\n第二行"` | 两行文本 |
| `«…»` | **不转义**，原样保留 | `«C:\path\file»` | 反斜杠就是反斜杠 |

需要原样保留 `"` 与大量反斜杠（Windows 路径、正则、JSON 片段）时用 `«…»`。

### 4.6 成对记号

| 记号 | 名字 | 用途 |
|---|---|---|
| `⟦` … `⟧` | 块 | 函数体、`si` 分支、循环体；键盘 `{` `}` 等价 |
| `‹` … `›` | 序列字面量 | `‹1, 2, 3›`、`‹›` 空序列 |
| `⟬` … `⟭` | 记录字面量 | `⟬a ⟼ 1, b ⟼ 2⟭`、`⟬⟭` 空记录 |
| `"…"` / `«…»` | 文本字面量 | 见 §4.5 |
| `⟼` | 键值箭头 | 只在记录字面量里用，不是赋值 |

**不要混用**：`‹›` 是空序列，`⟬⟭` 是空记录。

---

## 5. 值：数字、文本、序列、记录、布尔、空

SILLAGE 只有 6 种值。没有类、没有日期类型、没有可变的容器。

### 5.1 Nombre（数字）

只有一种数字类型，内部是 `1e-9` 单位的整数：

```sillage
0.1 plus 0.2
```

实测输出：`0.3`（不是 `0.30000000000000004`）。

```sillage
1 sur 3
```

实测输出：`0.333333333`（9 位小数封顶）。

| 写法 | 结果 | 说明 |
|---|---|---|
| `7 sur 2` | `3.5` | 除法得小数，不是整除 |
| `7 modulo 3` | `1` | 取余 |
| `2 puissance 10` | `1024` | 幂 |
| `2 puissance 3 puissance 2` | `512` | 幂**右结合**：`2^(3^2)` |
| `1 plus 2 fois 3` | `7` | 先乘后加 |

### 5.2 Texte（文本）

* 按**字素簇**计数与索引：`mesure de "é"` → `1`（不是 2 字节）；
* 中文一个字算一个：`mesure de "语文"` → `2`；
* 下标 **1 起**；
* 拼接只用 `plus`，两侧都必须是文本：`"a" plus "b"`。

### 5.3 Suite（序列）

```sillage
‹1, 2, 3›
‹›
élément 2 de ‹10, 20, 30›
```

实测输出依次：`[1,2,3]`、`[]`、`20`。

* **不可变**：`ajoute-fin de xs avec x` 返回**新**序列，不改 `xs`；
* 下标 1 起；越界报 `calcul: indice hors limites`；
* 可以混装：`‹1, "a", OUI, ‹2››` 合法（动态类型）；
* `ordonne` 排序：**数字在前、文本在后**，同类稳定。

### 5.4 Fiche（记录）

```sillage
⟬nom ⟼ "小明", nianji ⟼ 3⟭
champ "nianji" de ⟬nom ⟼ "x", nianji ⟼ 3⟭
```

实测输出：`{"nom":"小明","nianji":3}`、`3`。

* 键必须是文本，**保持插入序**（`clefs` 返回插入序）；
* **缺键直接报错**：`champ "nnianji" de …` → `{"étape":"forme","note":"champ absent: nnianji"}`；
* 改字段返回新记录：`tordu de f avec k et v`；
* `renferme de f dans "k"` 检查的是**键**，不是值。

### 5.5 布尔与空

| 字面量 | 含义 |
|---|---|
| `OUI` / `vrai` | 真 |
| `NON` / `faux` | 假 |
| `VIDE` / `vide` / `rien` | 空 |

**真值判断**（`si`、`tant que`、`et`、`ou` 都用它）：

* 假：`NON`、`0`、`""`、`‹›`、`⟬⟭`、`VIDE`；
* 真：其它一切，**包括文本 `"0"` 和 `"NON"`**。

```sillage
si "0" alors ⟦ rends "真" ⟧ sinon ⟦ rends "假" ⟧
```

实测：`"真"`。

### 5.6 相等与 JSON

* 相等 `égal à` / `=` / `==` / `≟`：**类型不同即不等**，`1` 与 `"1"` 不等；
* 序列、记录按内容比较；
* `fige de x` 转紧凑 JSON：`fige de ⟬a ⟼ 1, b ⟼ ‹2, 3›⟭` → `"{\"a\":1,\"b\":[2,3]}"`；
* 打印模块结果时：`Nombre` → 最短十进制（整值不带 `.0`），`OUI/NON` → `true/false`，
  `VIDE` → `null`，`Fiche` 保持插入序，中文**不转义**。

---

## 6. 表达式与运算符

### 6.1 介词调用（这门语言的标志）

没有括号，实参角色由**介词**标出：

| 形态 | 例子 | 含义 |
|---|---|---|
| 一元 | `mesure de xs` | `mesure(xs)` |
| 二元（第二实参 `par`） | `découpe de "a,b" par ","` | `découpe("a,b", ",")` |
| 二元（`avec`） | `recoud de ‹1,2› avec "-"` | `recoud(‹1,2›, "-")` |
| 二元（`dans`） | `renferme de "salut" dans "alu"` | 第一个是容器，第二个被查项 |
| 三元 | `tordu de f avec k et v` | `tordu(f, k, v)` |
| 区间 | `bribe de x depuis a jusqu'à b` | 三个实参 |
| 用户函数多参 | `somme de 3 et 4` | 参数用 `et` 串 |

⚠️ **`de` 之后的实参按「加减」层解析，会吃掉 `plus` / `moins`**：

```sillage
mesure de "salut" plus 1
```

实测：`{"défaut":{"étape":"forme","ligne":null,"note":"addition impossible sur ces valeurs"}}`
——它被解析成 `mesure("salut" + 1)`。要在结果上做加减就**加括号**：

```sillage
(mesure de "salut") plus 1
```

实测：`6`。

比较符与 `et`/`ou` **不会**被吞进实参：`mesure de xs plus petit que 30` 是 `mesure(xs) < 30`。

也接受括号写法 `découpe("a,b", ",")`，但规范写法是介词式。

### 6.2 运算符全表（词与符号完全等价，可混用）

| 语义 | 词拼写 | 符号拼写 |
|---|---|---|
| 加 / 文本拼接 | `plus` | `+` `⊕` |
| 减 | `moins` | `-` `−` `⊖` |
| 乘 | `fois` | `*` `×` `⊗` |
| 除 | `sur` | `/` `÷` `⊘` |
| 取模 | `modulo` | `%` `⊞` |
| 幂（右结合） | `puissance` | `^` `↑` |
| 等于 | `égal à` / `égal` | `=` `==` `≟` |
| 不等 | `différent de` / `différent` | `≠` `≢` |
| 大于 | `plus grand que` | `>` `⪢` |
| 小于 | `plus petit que` | `<` `⪡` |
| 大于等于 | `au moins` | `>=` `≥` `⩾` |
| 小于等于 | `au plus` | `<=` `≤` `⩽` |
| 且 | `et` | `&&` `∧` |
| 或 | `ou` | `||` `∨` |
| 非（一元） | `non` | `!` `¬` |

**优先级**（高 → 低）：

```text
élément / champ（索引与取字段）
    >  一元 non、一元 moins
    >  puissance（右结合）
    >  fois sur modulo
    >  plus moins
    >  比较（可连写，左结合）
    >  et
    >  ou
```

细节：

* `plus` 后紧跟 `petit`/`grand` 时算**比较符**：`1 plus petit que 2` → `true`；
* **没有 `!=`**：`1 != 2` 是语法错（实测 `expression attendue, trouvé '='`），
  请写 `≠` / `≢` / `différent de`；
* 比较可以连写：`1 plus petit que 2 égal à OUI`；
* 比较**只能同类型**：数字比数字、文本比文本，混比报 `forme`。

### 6.3 索引与字段（自然语序，不是后缀）

```sillage
élément 2 de ‹10, 20, 30›               ‖ → 20
champ "nianji" de ⟬nom ⟼ "x", nianji ⟼ 5⟭   ‖ → 5
élément (n moins 1) de xs
champ (en-texte de i) de f
```

* 下标 **1 起**；文本按字素、序列按元素；
* 越界 → `calcul: indice hors limites`；非整数下标 → `forme`；
* 下标/字段键只吃一个「基本项」或一对括号，之后的 `de` 留给外层；
* 记录缺键 → `forme: champ absent: k`，**先 `renferme de f dans k` 判断**再取。

### 6.4 宿主点分名（并置实参）

```sillage
carnet.lister "posts"
carnet.écrire "posts" p
horloge.maintenant
atelier.lancer "示例程序/程序/calc.sil" "handle" ⟬action ⟼ "ping"⟭
```

规则：**没有括号、没有介词，实参直接并排**。这也是区分「宿主 / 内置 / 自己的函数」的视觉信号。

### 6.5 字面量速览

```sillage
42                        ‖ 数字
"文本\n转义"               ‖ 文本（有转义）
«C:\no\escape»            ‖ 文本（不转义）
‹1, 2, 3›   ‹›            ‖ 序列 / 空序列
⟬a ⟼ 1, b ⟼ 2⟭   ⟬⟭       ‖ 记录 / 空记录
OUI   NON   VIDE          ‖ 布尔与空
```

---

## 7. 语句：绑定、判断、循环、返回

### 7.1 绑定、重绑定、等价绑定

| 语义 | 写法 | 全部等价拼写 |
|---|---|---|
| 新名绑定 | `que x vaille 42` | 引导词 `que`/`soit` + `vaille` |
| 等价绑定 | `donne 42 à x` | `donne` … `à` |
| 重绑定 | `que x devienne x plus 1` | 引导词 `que`/`soit` + `devienne`/`remue` |

实测：

```sillage
que x vaille 42
x
```
→ `42`

```sillage
que x vaille 1
que x devienne x plus 1
x
```
→ `2`

```sillage
donne 5 à y
y
```
→ `5`

规则：

* `vaille` 是**新名绑定**，`devienne`/`remue` 是**重绑定**；重绑定未定义的名字报
  `{"étape":"nom","ligne":1,"note":"reliaison d'un nom inconnu: x"}`；
* **块是作用域**：块里 `vaille` 定义的名字出了块就看不见；`devienne` 改的是**外层已有**的名字；
* 同名可以重绑定成不同类型的值（动态类型）。

### 7.2 打印

```sillage
affiche 1 plus 1
```

实测输出：

```text
2
null
```

`affiche` / `dis` / `trace` 三者等价。**打印语句的值是 `VIDE`**，
所以它作为最后一条语句时模块结果是 `null`。

### 7.3 条件

```sillage
si 1 plus petit que 2 alors ⟦ rends "对" ⟧
```
→ `"对"`

```sillage
si NON alors ⟦ rends 1 ⟧ sinon ⟦ rends 2 ⟧
```
→ `2`

```sillage
si NON alors ⟦ rends 1 ⟧ sinonsi OUI alors ⟦ rends 3 ⟧ sinon ⟦ rends 2 ⟧
```
→ `3`

* 引导词：`si` / `quand` / `lorsque` 等价；
* 否则如果：`sinonsi` / `oualors`；
* 否则：`sinon` / `autrement`；
* `alors` 必须有，且 **`⟦` 必须与 `alors` 同行**；
* `si / sinonsi / sinon` 是**同一条语句的连续片段**，可同行也可换段；
* 空块 `⟦ ⟧` 合法。

### 7.4 循环

**当型循环**（`tant que` 两个词 / `tantque` / `boucle` 等价）：

```sillage
que i vaille 0
tant que i plus petit que 3 ⟦ que i devienne i plus 1 ⟧
i
```
→ `3`

**遍历**（`pour chaque v dans e ⟦ … ⟧`）：

```sillage
que s vaille 0
pour chaque v dans ‹1, 2, 3› ⟦ que s devienne s plus v ⟧
s
```
→ `6`

**文本也可遍历**（按字素）：

```sillage
pour chaque c dans "语文" ⟦ affiche c ⟧
```
→ 打印 `语`、`文`，模块结果 `null`。

**跳出**：`arrête` / `casse`；**下一轮**：`continue` / `passe`。
二者**只能在块里**，顶层写 `arrête` 报 `{"étape":"calcul","ligne":1,"note":"arrête hors d'une boucle"}`（实测）。

```sillage
que i vaille 0
tant que i plus petit que 100 ⟦
  que i devienne i plus 1
  si i au moins 3 alors ⟦ arrête ⟧
⟧
i
```
→ `3`

```sillage
que i vaille 0
que n vaille 0
tant que i plus petit que 10 ⟦
  que i devienne i plus 1
  si (i modulo 2) égal à 0 alors ⟦ passe ⟧
  que n devienne n plus i
⟧
n
```
→ `25`（奇数 1+3+5+7+9）

护栏：循环上限 **200 万次**，超出报 `calcul: boucle trop longue`；
递归上限 **400 层**，超出报 `calcul: pile trop profonde`。

### 7.5 返回与空句

```sillage
rends e        ‖ 函数返回；顶层 rends 结束整个模块
silence        ‖ 空句，什么都不做
```

`silence` 唯一的作用：作为**最后一条顶层句**时把模块结果压成 `VIDE`（见 §12）。

---

## 8. 函数：定义、参数、递归、作用域

### 8.1 定义与调用

```sillage
on définit double de a comme ⟦ rends a fois 2 ⟧
double de 21
```
→ `42`

* 关键字固定：`on définit` … `comme`，中间是 `名字 de 参数表`；
* **`comme` 与 `⟦` 必须同一行**；
* 调用就是介词式：`double de 21`。

### 8.2 参数表的两种写法

```sillage
on définit somme de a et b comme ⟦ rends a plus b ⟧
somme de 3 et 4
```
→ `7`

```sillage
on définit f de (a, b) comme ⟦ rends a fois b ⟧
f de 3 et 4
```
→ `12`

两种写法等价：`de a et b` 或 `de (a, b)`。**实参一律用 `et` 串**。

⚠️ **必须至少一个参数**。要「无参函数」写 `de ()` 可以定义，但 **C 引擎不支持零参调用**
（`f de ()` 报语法错）。工程里请用「占位参数」：`on définit f de n comme ⟦ … ⟧`，调用 `f de 0`。

### 8.3 递归

```sillage
on définit fib de n comme ⟦
  si n plus petit que 2 alors ⟦ rends n ⟧
  rends (fib de n moins 1) plus (fib de n moins 2)
⟧
fib de 10
```
→ `55`

```sillage
on définit f de n comme ⟦
  si n plus petit que 1 alors ⟦ rends 0 ⟧
  rends n plus (f de n moins 1)
⟧
f de 100
```
→ `5050`

深递归请改循环：递归上限 400 层。

### 8.4 作用域

* 进入块时开新作用域：父作用域可见，子作用域的名字出块即消失；
* 函数体就是块，函数内的 `vaille` 不污染全局；
* 同名遮蔽：内层同名遮住外层，出块恢复；
* **没有闭包、没有 `import`**。

### 8.5 函数是值

```sillage
on définit deux de n comme ⟦ rends n fois 2 ⟧
que g vaille deux
g de 21
```

宿主服务里函数就是程序导出的入口：`atelier.lancer "程序.sil" "handle" ‹req›` 调的就是 `handle`。

---

# 第三部分 · 标准能力

## 9. 内置函数 18 个

全部是**普通名字**，可被本地绑定遮蔽；调用一律介词式。

| # | 调用 | 作用 | 实测例子 → 结果 |
|---|---|---|---|
| 1 | `mesure de x` | 长度：文本字素 / 序列元素 / 记录键数 | `mesure de "salut"` → `5`；`mesure de "é"` → `1`；`mesure de ‹1,2,3›` → `3` |
| 2 | `en-texte de x` | 转文本 | `en-texte de 42` → `"42"`；`en-texte de VIDE` → `""`；`en-texte de OUI` → `"oui"` |
| 3 | `en-nombre de t` | 文本转数字（容错空格与千分位逗号），失败**报错** | `en-nombre de " 1 234,5 "` → `1234.5`；`en-nombre de "abc"` → `{"étape":"calcul","note":"en-nombre: « abc » n'est pas un nombre"}` |
| 4 | `égalise de x` | 规范化数字 | `égalise de 2.500000000` → `2.5`（本实现数字本就不带尾零） |
| 5 | `abaissé de t` | 小写 | `abaissé de "ÉCOLE"` → `"école"` |
| 6 | `haussé de t` | 大写 | `haussé de "abc"` → `"ABC"` |
| 7 | `découpe de t par sep` | 拆分 | `découpe de "a,b,c" par ","` → `["a","b","c"]`；`par ""` 按字素切 |
| 8 | `recoud de xs avec sep` | 连接 | `recoud de ‹1, 2, 3› avec "-"` → `"1-2-3"` |
| 9 | `renferme de 容器 dans 被查项` | 包含判断：文本子串 / 序列元素 / 记录键 | `renferme de "salut" dans "alu"` → `true` |
| 10 | `clefs de f` | 记录的键序列（插入序） | `clefs de ⟬a ⟼ 1, b ⟼ 2⟭` → `["a","b"]` |
| 11 | `teneurs de f` | 记录的值序列（插入序） | `teneurs de ⟬a ⟼ 1, b ⟼ 2⟭` → `[1,2]` |
| 12 | `ajoute-fin de xs avec x` | 追加一个元素，返回**新**序列 | `ajoute-fin de ‹1, 2› avec 3` → `[1,2,3]` |
| 13 | `bribe de x depuis a jusqu'à b` | 切片：1 起、**闭区间**、越界夹紧 | `bribe de "abcdef" depuis 2 jusqu'à 4` → `"bcd"`；`bribe de ‹1,2,3,4› depuis 2 jusqu'à 3` → `[2,3]` |
| 14 | `ordonne de xs` | 排序：数字在前，文本按字典序，同类稳定 | `ordonne de ‹"b", 3, "a", 1›` → `[1,3,"a","b"]` |
| 15 | `total de xs` | 数字序列求和，空序列为 0 | `total de ‹1, 2, 3.5›` → `6.5` |
| 16 | `tordu de f avec k et v` | 改/加一个键，返回**新**记录 | `tordu de ⟬a ⟼ 1⟭ avec "b" et 2` → `{"a":1,"b":2}` |
| 17 | `fige de x` | 转紧凑 JSON 文本 | `fige de ⟬a ⟼ 1, b ⟼ ‹2, 3›⟭` → `"{\"a\":1,\"b\":[2,3]}"` |
| 18 | `absolu de x` | 绝对值 | `absolu de (0 moins 7)` → `7` |

注意：

* `renferme de f dans "k"` 查的是**键**，不是值；
* `bribe` 越界夹紧不报错；`élément` 越界**报错**；
* `en-nombre` 失败直接报 `calcul`，要自己先校验（如逐字符白名单）。

---

## 10. 宿主服务：集合、时间、输出、跨程序调用

这些不是「函数库」，而是**引擎内置服务**（C 引擎独立实现，无需宿主注入）。

### 10.1 集合 `carnet.*`

| 调用 | 说明 |
|---|---|
| `carnet.lister "集合名"` | 读集合（序列；不存在返回 `‹›`） |
| `carnet.lire "集合名"` | 读集合原值（不存在返回 `VIDE`） |
| `carnet.écrire "集合名" xs` | 覆盖写入并落盘 |
| `carnet.ajouter "集合名" u` | 追加一条并落盘 |
| `carnet.compter "集合名"` | 条数 |
| `carnet.vider "集合名"` | 清空 |
| `carnet.retirer` / `carnet.fusionner` | 删一条 / 批量合并（C 引擎提供） |

数据存在**一个本地 JSON 文件**里（默认 `数据/sillage.json`，`--store` 可指定）；
写操作原子替换（临时文件 + rename），单机、无数据库、无网络。

```sillage
carnet.vider "demo"
carnet.ajouter "demo" ⟬id ⟼ 1, mot ⟼ "春天"⟭
affiche en-texte de (carnet.compter "demo")
```

### 10.2 时间与输出

```sillage
horloge.maintenant        ‖ "2026-11-26T17:00:00"
voix.dire "处理完成"       ‖ 打印到 stdout
voix.signaler "记一条"     ‖ 同上
```

### 10.3 跨程序调用 `atelier.lancer`

```sillage
que rep vaille atelier.lancer "示例程序/程序/calc.sil" "handle" ⟬action ⟼ "ping"⟭
```

* 文件名含 `/`：按**项目根**相对路径；
* 只给短名：在 `juanzong/`、`huapu/`、项目根依次找 `<名>.sil`；
* 函数不存在 → `nom: geste inconnu`；文件不存在 → `hôte: atelier.lancer: program-not-found: …`；
* 解释器按「路径 + 修改时间」缓存。

### 10.4 其他宿主（引擎自带）

| 名字 | 说明 |
|---|---|
| `atelier.évaluer "源码"` | 把一段源码当模块求值 |
| `atelier.noms` | 列出当前模块已定义的名字 |

---

## 11. 错误模型与调试

```text
Défaut:
  étape ∈ lexique | syntaxe | forme | nom | calcul | hôte
  ligne : 行号（解析期多半有，运行期常为 null）
  note  : 法文提示
```

| étape | 什么时候 | 实测例子 |
|---|---|---|
| `lexique` | 意外字符、字符串没闭合、数字读不出 | `que x vaille "未闭合` → `texte non terminé` |
| `syntaxe` | 缺关键字、块没闭合、多余记号 | `que vaille 3` → `attendu « vaille » ou « devienne »`；`1 != 2` → `expression attendue, trouvé '='` |
| `forme` | 类型不对、缺字段、参数个数不对 | `affiche 1 plus "a"` → `addition impossible sur ces valeurs` |
| `nom` | 未定义名字、重绑定未定义 | `que x devienne 1` → `reliaison d'un nom inconnu: x` |
| `calcul` | 除零、下标越界、循环太久、栈太深 | `élément 9 de ‹1, 2›` → `indice hors limites` |
| `hôte` | 内置服务/跨程序调用失败 | 文件不存在 → `fichier introuvable` |

命令行输出一行 JSON：

```text
{"défaut": {"étape": "calcul", "ligne": null, "note": "indice hors limites"}}
```

REPL 里出错只打印这一行，然后回到提示符，**不退出**。调试四招：

1. 用 `affiche` 把中间值打出来；
2. 用 `:reset` 重开会话，避免坏语句留在会话里；
3. 用 `ast` 子命令看解析结果；
4. 用 `fige` 看容器真实内容。

---

## 12. 模块结果的三条规则

`eval(src)` / `run` 的返回值：

1. 第一条真正执行的**顶层** `rends e` → 返回 e，模块**立即结束**；
2. 否则取最后一条**顶层表达式句**的值；
3. 否则 `VIDE`；
4. 特例：若最后一条顶层句是 `silence`，模块结果强制 `VIDE`。

函数体里的 `rends` 只结束该函数。实测：

```sillage
que x vaille 5
x
```
→ `5`

```sillage
que x vaille 5
rends 99
que x devienne 6
```
→ `99`（第一条顶层 `rends` 立刻结束，后面的语句不跑）

```sillage
que x vaille 5
```
→ `null`（没有表达式句）

```sillage
que x vaille 5
x
silence
```
→ `null`（最后一条是 `silence`）

---

# 第四部分 · 用它

## 13. 四种交互方式与完整命令参考

### 13.1 总览

| 方式 | 命令 | 适合 | 输出 |
|---|---|---|---|
| **双击 / REPL** | `sillage_c.exe`、`sillage_c.exe repl` | 边写边试、教学演示 | 交互式回显 |
| **脚本 CLI** | `sillage_c.exe run <文件> [JSON]`、`call`、`ast` | 跑程序、调试解析 | stdout + 模块结果 JSON |
| **JSON-RPC** | `sillage_c.exe --server --store <JSON>` | 被浏览器 / 其他语言调用 | 逐行 JSON |
| **页面程序** | `python 浏览器源码\航迹浏览器.py` | 用 SILLAGE 写界面 | 原生窗口 |

### 13.2 REPL 命令

| 命令 | 作用 |
|---|---|
| `:help` | 显示命令帮助 |
| `:reset` | 清空当前会话已输入的代码 |
| `:load <文件.sil>` | 把文件并入当前会话并执行 |
| `:quit` / `:q` | 退出 |

补充：块未闭合时提示 `...` 续行；`{` `}` 自动等价成 `⟦` `⟧`；
控制台按 UTF-16 读入再转 UTF-8，中文可直接输入。

### 13.3 CLI

```text
sillage_c.exe                          双击/无参：交互式解释器
sillage_c.exe repl                     同上（显式）
sillage_c.exe test                     内置自测（360 条）
sillage_c.exe run <文件.sil> [JSON]    运行模块：先执行 affiche，最后打印模块结果 JSON
sillage_c.exe call <文件.sil> <函数> [JSON]   调用函数并打印返回值
sillage_c.exe ast <文件.sil>           打印语法树（每行一条，数字为 1e-9 单位整数）
sillage_c.exe --server [--store <JSON>]   RPC 服务（浏览器用）
```

真实例子：

```powershell
.\可执行程序\sillage_c.exe run 示例程序\入门\01-hello.sil
.\可执行程序\sillage_c.exe call 示例程序\程序\calc.sil handle '{"action":"calc","params":{"op":"mul","a":6,"b":7}}'
```

实测输出（第二条）：

```json
{
  "status": 200,
  "body": {
    "op": "mul",
    "a": 6,
    "b": 7,
    "expr": "6 * 7",
    "result": 42
  }
}
```

### 13.4 RPC 协议（逐行 JSON，UTF-8，不换行）

请求：

```json
{"id":1,"cmd":"ping"}
{"id":2,"cmd":"eval","source":"que x vaille 2 plus 3\nrends x","name":"<module>"}
{"id":3,"cmd":"call","file":"D:/…/示例程序/程序/calc.sil","func":"handle","arg":{"action":"calc","params":{"op":"div","a":9,"b":2}}}
{"id":4,"cmd":"names","file":"D:/…/示例程序/程序/demo.sil"}
{"id":5,"cmd":"store"}
{"id":6,"cmd":"quit"}
```

响应：

```json
{"id":1,"ok":true,"value":"pong"}
{"id":3,"ok":true,"value":{"status":200,"body":{"result":4.5}}}
{"id":3,"ok":false,"error":{"étape":"nom","ligne":null,"note":"geste inconnu: x"}}
```

服务端不打印 `affiche`，保证 stdout 只有 JSON。

---

## 14. 12 课示例逐段讲解（真实输出）

`示例程序/入门/` 有 12 个可直接运行的小程序。下面每课都给出**文件真实内容**与
`.\可执行程序\sillage_c.exe --store 数据\sillage.json run <文件>` 的**真实输出**
（`--store` 让「项目根」= 分发包根，第 12 课的跨程序调用才找得到文件）。

### 14.1 `01-hello.sil` — 变量与打印

```sillage
‖ 第 1 课：变量与打印
que nom vaille "小明"
affiche "你好，" plus nom
affiche 2 plus 3 fois 4          ‖ 14，先乘后加
```

实测输出：

```text
你好，小明
14
null
```

要点：`affiche` 的值是 `VIDE`，所以最后一条语句是 `affiche` 时模块结果 `null`。

### 14.2 `02-arithmetique.sil` — 算术与优先级

```sillage
‖ 第 2 课：算术与优先级
affiche 1 plus 2 fois 3          ‖ 7
affiche (1 plus 2) fois 3        ‖ 9
affiche 7 sur 2                  ‖ 3.5
affiche 7 modulo 3               ‖ 1
affiche 2 puissance 10           ‖ 1024
```

实测输出：

```text
7
9
3.5
1
1024
null
```

要点：先乘后加；括号改变结合；除法得小数。

### 14.3 `03-texte.sil` — 文本

```sillage
‖ 第 3 课：文本
que t vaille "小学语文"
affiche mesure de t              ‖ 4
affiche élément 2 de t           ‖ 学
affiche bribe de t depuis 3 jusqu'à 4   ‖ 语文
affiche haussé de "abc"          ‖ ABC
affiche "第一行\n第二行"
```

实测输出：

```text
4
学
语文
ABC
第一行
第二行
null
```

要点：文本按**字素**计数与索引（下标 1 起）；`bribe` 闭区间；`\n` 只在 `"…"` 里是换行。

### 14.4 `04-suites.sil` — 序列

```sillage
‖ 第 4 课：序列
que xs vaille ‹3, 1, 2›
affiche total de xs              ‖ 6
affiche élément 2 de xs          ‖ 1
affiche ajoute-fin de xs avec 9  ‖ [3,1,2,9]
affiche bribe de xs depuis 2 jusqu'à 3  ‖ [1,2]
affiche ordonne de xs            ‖ [1,2,3]
```

实测输出：

```text
6
1
[3,1,2,9]
[1,2]
[1,2,3]
null
```

要点：序列**不可变**——`xs` 始终是 `‹3,1,2›`，`ajoute-fin` 只是返回新序列。

### 14.5 `05-fiches.sil` — 记录

```sillage
‖ 第 5 课：记录
que u vaille ⟬nom ⟼ "小雨", nianji ⟼ "三年级"⟭
affiche champ "nom" de u
affiche clefs de u
que u2 vaille tordu de u avec "banji" et "三(1)班"
affiche fige de u2
```

实测输出：

```text
小雨
["nom","nianji"]
{"nom":"小雨","nianji":"三年级","banji":"三(1)班"}
null
```

要点：记录保持**插入序**；`tordu` 返回新记录；`fige` 是紧凑 JSON。

### 14.6 `06-condition.sil` — 条件

```sillage
‖ 第 6 课：条件
que prix vaille 12
si prix plus grand que 10 alors ⟦
  affiche "贵"
⟧ sinonsi prix plus grand que 5 alors ⟦
  affiche "中等"
⟧ sinon ⟦
  affiche "便宜"
⟧
```

实测输出：

```text
贵
null
```

要点：`alors` 与 `⟦` 必须同一行；`⟧ sinonsi … alors ⟦` 可以连写在同一行。

### 14.7 `07-boucle.sil` — 当型循环

```sillage
‖ 第 7 课：当型循环
que i vaille 1
tant que i au plus 10 ⟦
  affiche i
  que i devienne i plus 1
⟧
```

实测输出：`1` 到 `10` 各一行，末尾 `null`。

要点：重绑定必须用 `devienne`；忘记递增就是死循环（护栏 200 万次）。

### 14.8 `08-parcours.sil` — 遍历

```sillage
‖ 第 8 课：遍历
pour chaque c dans "小学语文" ⟦
  affiche c
⟧
que xs vaille ‹"春", "夏", "秋", "冬"›
pour chaque x dans xs ⟦
  affiche "季节：" plus x
⟧
```

实测输出：

```text
小
学
语
文
季节：春
季节：夏
季节：秋
季节：冬
null
```

要点：文本按字素遍历；序列按元素遍历。

### 14.9 `09-fonction.sil` — 函数与返回

```sillage
‖ 第 9 课：函数
on définit somme de xs comme ⟦
  que s vaille 0
  pour chaque x dans xs ⟦
    que s devienne s plus x
  ⟧
  rends s
⟧

on définit moyenne de xs comme ⟦
  si mesure de xs plus petit que 1 alors ⟦
    rends 0
  ⟧
  rends (somme de xs) sur (mesure de xs)
⟧

affiche somme de ‹1, 2, 3›
affiche moyenne de ‹1, 2, 3, 4›
```

实测输出：

```text
6
2.5
null
```

要点：`(somme de xs) sur (mesure de xs)` 的括号不能省——`de` 会吃掉右侧加减层。

### 14.10 `10-recursion.sil` — 递归

```sillage
‖ 第 10 课：递归
on définit fib de n comme ⟦
  si n plus petit que 2 alors ⟦
    rends n
  ⟧
  rends (fib de (n moins 1)) plus (fib de (n moins 2))
⟧

que i vaille 0
tant que i plus petit que 10 ⟦
  affiche fib de i
  que i devienne i plus 1
⟧
```

实测输出：

```text
0
1
1
2
3
5
8
13
21
34
null
```

要点：递归参数要写 `(n moins 1)` 加括号；上限 400 层。

### 14.11 `11-collections.sil` — 内置集合服务

```sillage
‖ 第 11 课：内置集合服务
carnet.vider "learn"
carnet.ajouter "learn" ⟬id ⟼ 1, mot ⟼ "春天"⟭
carnet.ajouter "learn" ⟬id ⟼ 2, mot ⟼ "柳树"⟭
que xs vaille carnet.lister "learn"
affiche "条数：" plus (en-texte de (mesure de xs))
pour chaque u dans xs ⟦
  affiche (en-texte de (champ "id" de u)) plus " " plus (champ "mot" de u)
⟧
```

实测输出：

```text
条数：2
1 春天
2 柳树
{
  "id": 2,
  "mot": "柳树"
}
```

要点：

* 集合是**宿主内置服务**，CLI 也能直接读写（落到 `数据/sillage.json`）；
* 最后打印的 JSON 是**模块结果**：按 §12 规则 2，取「最后一条顶层**表达式句**」的值——
  即第 4 行 `carnet.ajouter …` 返回的那条记录。`que`/`affiche`/`pour chaque` 都不算表达式句，所以没被选中。

### 14.12 `12-lancer.sil` — 跨程序调用

```sillage
‖ 第 12 课：跨程序调用
que rep vaille atelier.lancer "示例程序/入门/09-fonction.sil" "somme" ‹1, 2, 3›
affiche "另一个程序算出来：" plus (en-texte de rep)
```

实测输出：

```text
6
2.5
另一个程序算出来：6
null
```

要点：被调用的文件**先整体执行顶层语句**（于是先看到它自己的两条 `affiche`：`6`、`2.5`），
再调 `somme` 得到 `6`。运行跨程序示例务必加 `--store 数据\sillage.json`，否则按项目根找不到文件。

---

## 15. 四周学习路线

| 周 | 目标 | 内容 | 产出 |
|---|---|---|---|
| **第 1 周** | 能读写小脚本 | §4–§7；12 课里的 01–06 | 输入输出、算术、文本、序列、记录、条件 |
| **第 2 周** | 能写函数与循环 | §7.4–§8；07–10 | 当型循环、遍历、函数、递归 |
| **第 3 周** | 能用标准能力 | §9–§12；11–12 | 18 个内置、集合存储、跨程序调用、错误处理 |
| **第 4 周** | 能写完整程序 | §13 + 四个示例程序源码 | 自己写一个 100 行以上、有 `handle(req)` 的程序，用 RPC 调通 |

每天 30–60 分钟，每周结束做 §16 对应编号的练习。

---

## 16. 24 道练习与答案要点

> 先自己写，再对照 `示例程序/入门/` 的写法。每题给的答案要点都在本机验证过。

**第 1 周**

1. 输出 1 到 10。
   *要点*：`que i vaille 1` + `tant que i au plus 10` + `affiche i`。
2. 求 1 到 100 的和。
   *要点*：循环里 `que s devienne s plus i`；结果 `5050`。
3. 判断奇偶。
   *要点*：`si (n modulo 2) égal à 0 alors …`。
4. 把 `"a,b,c"` 拆开再拼成 `"a-b-c"`。
   *要点*：`recoud de (découpe de "a,b,c" par ",") avec "-"` → `"a-b-c"`。
5. 求序列最大值（不许用 `ordonne`）。
   *要点*：`que m vaille élément 1 de xs` + 遍历比较。
6. 统计一段文本里「的」出现几次。
   *要点*：`pour chaque c dans t` + `si c égal à "的"`。
7. 把记录里某个字段加 1。
   *要点*：`tordu de u avec "n" et ((champ "n" de u) plus 1)`（返回新记录）。
8. 生成 10 个平方数。
   *要点*：`ajoute-fin de xs avec (i fois i)`，注意要 `que xs devienne …`。

**第 2 周**

9. 写函数 `moyenne de xs`。
   *要点*：空序列先返回 0，再 `(somme de xs) sur (mesure de xs)`。
10. 写递归阶乘。
    *要点*：`si n plus petit que 2 alors ⟦ rends 1 ⟧`。
11. 写递归斐波那契（n ≤ 20）。
    *要点*：见 14.10；n 大于 25 会因递归深度/指数时间变慢。
12. 用 `bribe` 取文本前 3 个字。
    *要点*：`bribe de t depuis 1 jusqu'à 3`，越界自动夹紧所以短文本也安全。
13. 判断回文。
    *要点*：用 `bribe` 从两端取、循环比较长度的一半。
14. 两个序列拼起来再排序。
    *要点*：`ordonne de (xs plus ys)`——注意 `plus` 直接拼序列。
15. 用记录表示学生（姓名、语文、数学），算总分。
    *要点*：`(champ "yuwen" de u) plus (champ "shuxue" de u)`。
16. 遍历学生序列，打印总分最高的人。
    *要点*：保存当前最高分与对应姓名。

**第 3 周**

17. 用 `carnet.*` 往集合里写 3 条再读出来。
    *要点*：`carnet.écrire` 会覆盖，`carnet.ajouter` 才是追加。
18. 用 `atelier.lancer` 调另一个 `.sil` 的函数。
    *要点*：加 `--store 数据\sillage.json`，路径按项目根写 `示例程序/入门/09-fonction.sil`。
19. 找出序列里所有大于 10 的数（生成新序列）。
    *要点*：`que out devienne (ajoute-fin de out avec x)`。
20. 文本替换：把所有「。」换成「！」。
    *要点*：`recoud de (découpe de t par "。") avec "！"`。
21. 实现 `map`：每个元素乘 2。
    *要点*：一个「函数参数」不行——SILLAGE 不做高阶函数，直接写循环。
22. 实现 `filter`：只保留偶数。
    *要点*：同 19 的思路。
23. 用 `fige` 打印记录 JSON。
    *要点*：`affiche fige de u`。
24. 用 `:load` 载入自己的程序并改一行再运行。
    *要点*：`:reset` 后 `:load` 干净重来。

---

## 17. 20 个常见错误与陷阱

| # | 现象 | 原因 | 正确写法 |
|---|---|---|---|
| 1 | `attendu « vaille » ou « devienne »` | `que` 后面漏了动词 | `que x vaille 3` |
| 2 | 语法错在 `⟦` 那一行 | `⟦` 与引导词换了行 | `si … alors ⟦` 必须同一行 |
| 3 | `expression attendue, trouvé '='` | 用了 `!=` | `1 ≠ 2` 或 `1 différent de 2` |
| 4 | `reliaison d'un nom inconnu` | 用 `devienne` 改没 `vaille` 过的名字 | 先 `que x vaille 0` |
| 5 | `x-1` 被当成一个名字 | `-` 是标识符合法字符 | `x moins 1` / `x - 1` |
| 6 | `1e3` 不是 1000 | 没有科学计数法 | 写 `1000` |
| 7 | `addition impossible sur ces valeurs` | 数字与文本相加 | 先 `en-texte` / `en-nombre` |
| 8 | `mesure de xs plus 1` 报错 | `de` 吃掉了 `plus` | `(mesure de xs) plus 1` |
| 9 | `ajoute-fin` 后原序列没变 | 序列不可变 | `que xs devienne (ajoute-fin de xs avec x)` |
| 10 | `ajoute-fin de xs avec ‹1,2›` 只加了一个元素 | 追加的是列表 | 拼列表用 `xs plus ‹1,2›` |
| 11 | `champ absent: k` | 记录没有这个键 | 先 `renferme de f dans k` |
| 12 | `indice hors limites` | 下标从 1 起 / 越界 | 检查长度 `mesure de xs` |
| 13 | `texte non terminé` | 字符串漏引号或跨行 | 一行内闭合；换行写 `\n` |
| 14 | 中文路径反斜杠丢字 | 用了 `"…"` 转义 | 用 `«C:\path»` |
| 15 | `arrête hors d'une boucle` | 循环控制在块外 | 放进循环体 |
| 16 | `boucle trop longue` | 循环没收敛 | 检查循环条件与重绑定 |
| 17 | `pile trop profonde` | 递归太深 | 改循环 / 减层数 |
| 18 | 无参函数 `f de ()` 语法错 | C 引擎不支持零参调用 | 用占位参数 `f de 0` |
| 19 | `atelier.lancer: program-not-found` | 没指定项目根 | 加 `--store 数据\sillage.json` |
| 20 | `en-nombre de "abc"` 抛错 | 转换失败即报错 | 先逐字符白名单校验 |

---

## 18. 附录：全部关键字 / 运算符 / 内置 / 宿主

### 18.1 语句关键字

```text
que  soit  vaille  devienne  remue  donne  à
affiche  dis  trace  rends  silence
si  quand  lorsque  alors  sinonsi  oualors  sinon  autrement
tant  que  tantque  boucle  pour  chaque  dans  arrête  casse  continue  passe
on  définit  de  et  comme  non
```

### 18.2 运算符（词与符号等价）

```text
plus + ⊕        moins - − ⊖      fois * × ⊗       sur / ÷ ⊘      modulo % ⊞      puissance ^ ↑
égal à = == ≟   différent de ≠ ≢  plus grand que > ⪢   plus petit que < ⪡
au moins >= ≥ ⩾   au plus <= ≤ ⩽   et && ∧   ou || ∨   non ! ¬
```

### 18.3 内置函数（18）

```text
mesure  en-texte  en-nombre  égalise  abaissé  haussé  découpe  recoud  renferme
clefs   teneurs    ajoute-fin  bribe    ordonne  total   tordu    fige    absolu
```

### 18.4 宿主服务

```text
carnet.lister  carnet.lire  carnet.écrire  carnet.ajouter  carnet.compter  carnet.vider
carnet.retirer carnet.fusionner
horloge.maintenant   voix.dire   voix.signaler
atelier.lancer   atelier.évaluer   atelier.noms
```

### 18.5 错误 étape（六类）

```text
lexique  syntaxe  forme  nom  calcul  hôte
```

### 18.6 符号速查

| 符号 | 作用 |
|---|---|
| `⟦ ⟧`（或 `{ }`） | 块 |
| `‹ ›` | 序列；`‹›` 空序列 |
| `⟬ ⟭` | 记录；`⟬⟭` 空记录 |
| `⟼` | 记录的键值箭头 |
| `"…"` / `«…»` | 文本（有转义 / 无转义） |
| `‖` / `#` | 注释到行尾 |
| `!` `¬` | 一元非 |

---

## 19. 已知限制

* **没有科学计数法**：`1e3` 不是 1000；
* **数字上限** `10¹⁵`，除法结果截断到 9 位小数；
* **没有 `!=`**，没有三目运算符，没有 `switch`；
* **没有高阶函数 / 闭包 / 模块系统**（跨程序调用靠 `atelier.lancer`）；
* **没有异常捕获**：`Défaut` 一律上抛，脚本里不 try/catch（宿主可捕获）；
* **循环 200 万次、递归 400 层**护栏；
* **C 引擎不支持零参调用** `f de ()`（Python/JS 引擎支持）；
* REPL 的 `:load` 是「并入当前会话」，不是独立模块；
* 单机、无并发：集合存储是一个 JSON 文件，多进程同时写需要自己串行化。

---

## 20. 协议、参与与致谢

* **协议**：MIT，见 `许可证.txt`；可自由使用、修改、分发，保留版权声明即可。
* **参与**：见 `参与约定.md`。要点：
  1. 三份引擎语义必须一致，改语义时三份一起改并补 `test` 用例；
  2. 新关键字/别名要同步更新本手册与 `示例程序/`；
  3. 错误格式固定为 `{"défaut": {"étape","ligne","note"}}`，`étape` 只能是那六个；
  4. 提交前跑 `.\可执行程序\sillage_c.exe test` 与 `python -X utf8 解释器源码/Python引擎/sillage.py test`。
* **配套**：`浏览器源码/`（语言专用浏览器，页面本身就是 SILLAGE 程序）、
  `网页端/index.html`（网页演示）、`示例程序/`（12 课 + 4 个完整程序）。

---

*SILLAGE v1.0 · MIT License · 本手册可自由分发与修改，保留版权声明即可。*
