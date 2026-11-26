# 航迹浏览器 · 页面规范（PAGE_SPEC）

> 本文件是**航迹语言专用浏览器**与「用 SILLAGE 写的页面程序」之间唯一的接口约定。
> 浏览器是原生桌面窗口（Python + tkinter，无 HTML/CSS/JS）；
> 页面不是网页，而是**一段 SILLAGE 程序**，返回一棵**文档树**，渲染器把它画成真实控件。

```
用户点击/输入
     │  req = {route, action, fields, arg, history}
     ▼
浏览器 ── 调用 ──▶ page de req        ← 用 SILLAGE 编写（浏览器源码/页面/*.sil）
     │                 │
     └── {statut,titre,doc} ◀──┘    （SILLAGE 值：数字/文本/序列/记录/布尔/空）
```

---

## 0. 硬性约束

1. 页面文件放在 `浏览器源码/页面/<拼音>.sil`，导出 `on définit page de req comme ⟦ … ⟧`。
2. 文件加载时**只做定义**：顶层不写 `rends`、不产生副作用（不要顶层 `affiche`）。
3. 返回值（法文键）：

   ```
   ⟬ statut ⟼ 200,          ‖ 200 正常；404 页面不存在；400 参数坏；500 内部错
     titre  ⟼ "笔触库",      ‖ 窗口标题
     doc    ⟼ 文档树根,      ‖ 必须是 ⟬type ⟼ "page", children ⟼ ‹…›⟭
     journal ⟼ ‹"提示"› ⟭    ‖ 可选：事件日志（状态栏右侧滚动）
   ```
4. `doc` 里只能用本文 §2 的节点类型与字段；未知类型、缺字段、类型不符都会判为错误页。
5. 所有文本字段必须是文本；表格单元格可以是文本或数字。
6. 深度 ≤ 24 层，节点 ≤ 6000 个。

---

## 1. 请求对象 `req`

```
req = ⟬
  route   ⟼ "bichu",                  ‖ 地址栏 sillage:// 后面那一段
  action  ⟼ "chaxun" | VIDE,           ‖ 按钮的 action；首次加载为空
  fields  ⟼ ⟬ name ⟼ "值" ⟭,           ‖ 当前页面所有输入框/下拉框的值（文本）
  arg     ⟼ 任意 | VIDE,               ‖ 按钮的 args（如 ⟬id ⟼ 3⟭、翻页 ⟬ye ⟼ 2⟭）
  history ⟼ ‹"shouye", "bichu"›       ‖ 最近访问的路由（最多 20 条）
⟭
```

页面必须防御式读取：`(champ-texte de req et "k")` 这类函数在模板里，缺键返回空。

---

## 2. 文档节点（16 种）

| type | 必填字段 | 可选字段 | 说明 |
|---|---|---|---|
| `page` | `children` | — | 根节点，唯一允许的根 |
| `heading` | `text` | `level`(1..3)、`tone` | 标题 |
| `text` | `text` | `tone` | 段落；tone ∈ dim/ok/warn/bad/info |
| `row` | `children` | `gap` | 横排 |
| `card` | `children` | `title` | 卡片 |
| `button` | `text` | `action` 或 `goto`（二选一）、`args`、`style`("primary") | 按钮 |
| `field` | `name` | `label`、`value`、`placeholder`、`kind`("password"/"number")、`width` | 输入框 |
| `select` | `name`、`options` | `label`、`value`、`width` | 下拉框 |
| `list` | `items`（文本序列） | `ordered`(布尔) | 列表 |
| `kv` | `items`（`‹‹"键","值"›…›`） | — | 键值表 |
| `table` | `headers`（文本序列）、`rows`（行的序列） | — | 表格 |
| `code` | `text`（多行用 `\n`） | — | 等宽代码块 |
| `divider` | — | — | 分隔线 |
| `badge` | `text` | `tone` | 徽章 |
| `link` | `text`、`goto` | — | 文字链接（直接切路由） |
| `spacer` | — | `size`（整数像素） | 空白 |

> 颜色、字号、留白由渲染器统一决定，页面不控制样式。

---

## 3. 交互语义

* **首次加载**：`action` 为空 → 页面返回默认文档（通常带一个列表/表单）。
* **点按钮**：`action` = 按钮的 action，`fields` = 当前所有输入框的值，`arg` = 按钮的 args。
* **文字链接 / goto 按钮**：浏览器直接切换路由，不经过页面。
* **翻页**：按钮写 `args ⟼ ⟬ye ⟼ 2⟭`，动作名统一用 `chaxun` 或 `liebiao`，页面用
  `(numero-page de req)` 解析。
* 页面返回 500 时浏览器显示「页面运行出错」，并把 `{"défaut": …}` 原文放进事件日志。

---

## 4. 宿主函数（C 引擎内置，页面与程序通用）

| 名字 | 签名 | 语义 |
|---|---|---|
| `carnet.lister` | (集合名) → 序列 | 读集合，缺省 `‹›` |
| `carnet.lire` | (集合名) → 任意 | 读集合原值，缺省 `VIDE` |
| `carnet.écrire` | (集合名, 值) → 值 | 覆盖并落盘 |
| `carnet.ajouter` | (集合名, 记录) → 记录 | 追加并落盘 |
| `carnet.compter` | (集合名) → 数字 | 条数 |
| `carnet.vider` | (集合名) → 0 | 清空 |
| `horloge.maintenant` | → 文本 | 本地时间 `YYYY-MM-DDTHH:MM:SS` |
| `voix.dire` / `voix.signaler` | (文本) → 文本 | 写事件日志 |
| `atelier.lancer` | (程序名, 函数名, 参数) → 任意 | 调用 `示例程序/程序/*.sil` 里的函数 |

浏览器另外注入（页面可直接用，不写括号、点分并置）：

| 名字 | 语义 |
|---|---|
| `ui.route` | 当前路由（无参直接写点分名） |
| `ui.now` | 当前时间文本 |
| `ui.lang` | 语言名 `"sillage"` |
| `ui.engine` | 当前引擎 `"c"` 或 `"py"` |

---

## 5. 路由与文件对照（浏览器源码/pagehost.py）

| 路由（`sillage://…`） | 页面文件 | 标题 |
|---|---|---|
| `shouye` | `页面/shouye.sil` | 首页 |
| `jiaocheng` | `页面/jiaocheng.sil` | 语法教程（12 课） |
| `sucha` | `页面/sucha.sil` | 语法速查（关键字/运算符/内置） |
| `lianxi` | `页面/lianxi.sil` | 交互练习场（运行/分词/语法树） |
| `mingling` | `页面/mingling.sil` | 命令行参考 |
| `ceshi` | `页面/ceshi.sil` | 自测报告 |
| `jisuanqi` | `页面/jisuanqi.sil` | 计算器（调 `calc.sil`） |
| `luntan` | `页面/luntan.sil` | 论坛（调 `forum.sil`） |
| `dongwuyuan` | `页面/dongwuyuan.sil` | 别名动物园（调 `zoo.sil`） |
| `guanyu` | `页面/guanyu.sil` | 关于 |

未登录/无账号概念：本语言浏览器**不做登录注册**，直接可用。

---

## 6. 渲染器要求（core.py）

* 顶部工具条：后退 / 前进 / 刷新 / 首页 + 地址栏（只接受 `sillage://`）+ 前往；
* 站内导航：十个路由的按钮，当前页高亮；
* 右侧「解释器」面板（可折叠）：源码编辑框 + `运行` / `分词` / `语法树` / `清空`，
  下方是输出区（等宽字体、可滚动）；
* 底部状态栏：地址、引擎、耗时（毫秒）、当前时间；
* 地址栏输入 `http://…` 必须被拒绝并给出提示，不得跳转；
* 支持 `--selftest`：无窗口渲染全部路由 + 截图 + 输出 JSON。

---

## 7. 页面模板

复制 `浏览器源码/页面模板.sil` 到 `页面/<你的页面>.sil`，只改 `page` 与构建函数。
模板里的公共函数（安全取值、页码解析、节点构造、分页）**原样保留**：

* 取值：`champ-texte`、`champ-nombre`、`champ-liste`、`champ-vrai`、`arg-de`、`numero-page`
* 节点：`h1` `h2` `h3` `p` `p-ton` `carte` `rangee` `rangee-large` `tableau` `liste-points`
  `liste-num` `kv` `saisie` `saisie-large` `saisie-secrete` `choix` `bouton` `bouton-doux`
  `bouton-args` `bouton-aller` `lien` `badge` `bloc-code` `separateur` `espace`
* 响应：`reponse de titre et doc`
* 分页：`pagination de action et total et ye et meiye`
