#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pagehost.py —— 航迹浏览器与「SILLAGE 页面程序」之间的无界面核心。

一个页面不是 HTML，而是一段 SILLAGE 程序：导出 `page de req`，返回一棵**文档树**，
渲染器（core.py）把它画成真实控件。本模块只做与界面无关的事：

  · 引擎选择（C 引擎优先，Python 引擎后备）与程序缓存；
  · 加载页面源码 → 调用 page(req) → 校验文档树 → 归一化响应；
  · 示例程序调用（`示例程序/程序/*.sil` 的 `handle`）；
  · 路由表、请求对象构造函数；
  · 文本抽取、动作枚举（自测与截图走查用）；
  · 文档树规则校验（节点类型 / 必填字段 / 深度 / 数量 / 字段类型）。

命令行自查：

    python 浏览器源码/pagehost.py jiaocheng            # 渲染一页，打印纯文本
    python 浏览器源码/pagehost.py --all               # 全部路由状态
    python 浏览器源码/pagehost.py lianxi --json       # 完整响应 JSON
    python 浏览器源码/pagehost.py --run "1 plus 2 fois 3"   # 直接求值一段源码
"""
from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "运行支撑"))

import _layout          # noqa: E402  路径的唯一出处
import engine as langengine  # noqa: E402  引擎适配层（C 优先，Python 后备）

INFO = _layout.INFO
EXT = INFO["ext"]
SCHEME = INFO["scheme"]
TITLE = INFO["title"]

# --------------------------------------------------------------------------- #
# 路由表：地址栏 sillage:// 后面那一段 → 页面文件
# --------------------------------------------------------------------------- #
ROUTE_FILES = {
    "shouye": "shouye",
    "jiaocheng": "jiaocheng",
    "sucha": "sucha",
    "lianxi": "lianxi",
    "mingling": "mingling",
    "ceshi": "ceshi",
    "jisuanqi": "jisuanqi",
    "luntan": "luntan",
    "dongwuyuan": "dongwuyuan",
    "guanyu": "guanyu",
}
ROUTES = list(ROUTE_FILES)

# 站内导航：(路由, 菜单文字)
NAV = [
    ("shouye", "首页"),
    ("jiaocheng", "语法教程"),
    ("sucha", "语法速查"),
    ("lianxi", "练习场"),
    ("mingling", "命令行"),
    ("jisuanqi", "计算器"),
    ("luntan", "论坛"),
    ("dongwuyuan", "别名动物园"),
    ("ceshi", "自测"),
    ("guanyu", "关于"),
]

MAX_DEPTH = 24
MAX_NODES = 6000

# --------------------------------------------------------------------------- #
# 文档树规则（与 PAGE_SPEC.md §2 一一对应）
# --------------------------------------------------------------------------- #
TONE = {"dim", "ok", "warn", "bad", "info"}
NODE_RULES = {
    "page": ({"children"}, set()),
    "heading": ({"text"}, {"level", "tone"}),
    "text": ({"text"}, {"tone"}),
    "row": ({"children"}, {"gap"}),
    "card": ({"children"}, {"title"}),
    "button": ({"text"}, {"action", "args", "goto", "style"}),
    "field": ({"name"}, {"label", "value", "placeholder", "kind", "width"}),
    "select": ({"name", "options"}, {"label", "value", "width"}),
    "list": ({"items"}, {"ordered"}),
    "kv": ({"items"}, set()),
    "table": ({"headers", "rows"}, set()),
    "code": ({"text"}, set()),
    "divider": (set(), set()),
    "badge": ({"text"}, {"tone"}),
    "link": ({"text", "goto"}, set()),
    "spacer": (set(), {"size"}),
}
TEXT_FIELDS = ("text", "title", "name", "label", "value", "placeholder",
               "action", "goto", "style")


class DocError(Exception):
    """文档树违反 PAGE_SPEC。"""


class LangError(Exception):
    """SILLAGE 缺陷（étape / ligne / note）。"""

    def __init__(self, error):
        self.error = error if isinstance(error, dict) else {
            "étape": "hôte", "ligne": None, "note": str(error)}
        super().__init__(self.error.get("note", str(error)))

    def to_dict(self):
        return self.error


def validate_doc(node, path="doc", depth=0, count=None):
    """校验文档树；返回节点数。任何不合规都抛 DocError。"""
    if count is None:
        count = [0]
    count[0] += 1
    if count[0] > MAX_NODES:
        raise DocError("%s：文档过大（> %d 节点）" % (path, MAX_NODES))
    if depth > MAX_DEPTH:
        raise DocError("%s：嵌套过深（> %d 层）" % (path, MAX_DEPTH))
    if not isinstance(node, dict):
        raise DocError("%s：节点必须是记录，实际是 %s" % (path, type(node).__name__))
    ntype = node.get("type")
    if not isinstance(ntype, str) or ntype not in NODE_RULES:
        raise DocError("%s：未知节点类型 %r" % (path, ntype))
    required, optional = NODE_RULES[ntype]
    missing = sorted(required - set(node))
    if missing:
        raise DocError("%s (%s)：缺少字段 %s" % (path, ntype, "、".join(missing)))
    unknown = sorted(set(node) - required - optional - {"type"})
    if unknown:
        raise DocError("%s (%s)：多余字段 %s" % (path, ntype, "、".join(unknown)))

    for key in TEXT_FIELDS:
        if key in node and node[key] is not None and not isinstance(node[key], str):
            raise DocError("%s (%s)：字段 %s 必须是文本" % (path, ntype, key))
    if node.get("tone") is not None and node["tone"] not in TONE:
        raise DocError("%s：未知 tone %r" % (path, node["tone"]))
    if ntype == "heading" and "level" in node:
        if not isinstance(node["level"], int) or isinstance(node["level"], bool) \
                or node["level"] not in (1, 2, 3):
            raise DocError("%s：heading.level 必须是 1..3" % path)
    if ntype == "button":
        if ("action" in node) == ("goto" in node):
            raise DocError("%s：button 必须且只能有 action 或 goto 之一" % path)
    if ntype == "list":
        if not isinstance(node["items"], list):
            raise DocError("%s：list.items 必须是序列" % path)
        for i, item in enumerate(node["items"]):
            if not isinstance(item, str):
                raise DocError("%s：list.items[%d] 必须是文本" % (path, i))
    if ntype == "kv":
        if not isinstance(node["items"], list):
            raise DocError("%s：kv.items 必须是序列" % path)
        for i, pair in enumerate(node["items"]):
            if not isinstance(pair, list) or len(pair) != 2:
                raise DocError("%s：kv.items[%d] 必须是 [键, 值]" % (path, i))
            for j, cell in enumerate(pair):
                if not isinstance(cell, str):
                    raise DocError("%s：kv.items[%d][%d] 必须是文本" % (path, i, j))
    if ntype == "table":
        if not isinstance(node["headers"], list) or not isinstance(node["rows"], list):
            raise DocError("%s：table.headers/rows 必须是序列" % path)
        for i, head in enumerate(node["headers"]):
            if not isinstance(head, str):
                raise DocError("%s：table.headers[%d] 必须是文本" % (path, i))
        for i, row in enumerate(node["rows"]):
            if not isinstance(row, list):
                raise DocError("%s：table.rows[%d] 必须是序列" % (path, i))
            for j, cell in enumerate(row):
                if isinstance(cell, bool) or not isinstance(cell, (str, int, float)):
                    raise DocError("%s：table.rows[%d][%d] 必须是文本或数字" % (path, i, j))
    if ntype == "select":
        if not isinstance(node["options"], list) or not all(
                isinstance(o, str) for o in node["options"]):
            raise DocError("%s：select.options 必须是文本序列" % path)
    if ntype == "spacer" and "size" in node:
        if not isinstance(node["size"], int) or isinstance(node["size"], bool):
            raise DocError("%s：spacer.size 必须是整数" % path)
    for key in ("gap", "width", "level"):
        if key in node and (not isinstance(node[key], int) or isinstance(node[key], bool)):
            raise DocError("%s：%s 必须是整数" % (path, key))
    if "ordered" in node and not isinstance(node["ordered"], bool):
        raise DocError("%s：ordered 必须是布尔" % path)

    children = node.get("children")
    if children is not None:
        if not isinstance(children, list):
            raise DocError("%s (%s)：children 必须是序列" % (path, ntype))
        for i, child in enumerate(children):
            validate_doc(child, "%s.children[%d]" % (path, i), depth + 1, count)
    return count[0]


# --------------------------------------------------------------------------- #
# 文档树遍历小工具
# --------------------------------------------------------------------------- #
def walk(node):
    yield node
    for child in node.get("children") or []:
        yield from walk(child)


def extract_text(node):
    """把文档树拉平成纯文本（自测断言「页面上有没有出现某个词」用）。"""
    parts = []

    def take(value):
        if isinstance(value, str):
            parts.append(value)
        elif isinstance(value, (int, float)) and not isinstance(value, bool):
            parts.append(str(value))

    for n in walk(node):
        for key in ("text", "title", "label", "value", "placeholder"):
            take(n.get(key))
        for key in ("items", "options"):
            for item in n.get(key) or []:
                take(item)
        if n.get("type") == "table":
            for head in n.get("headers") or []:
                take(head)
            for row in n.get("rows") or []:
                for cell in row:
                    take(cell)
        if n.get("type") == "kv":
            for pair in n.get("items") or []:
                for cell in pair:
                    take(cell)
    return "\n".join(parts)


def collect_actions(node):
    """文档里所有按钮动作与链接的跳转：[("action", args)] / [("goto:route", None)]。"""
    out = []
    for n in walk(node):
        if n.get("type") == "button":
            if n.get("action"):
                out.append((n["action"], n.get("args")))
            elif n.get("goto"):
                out.append(("goto:" + n["goto"], None))
        elif n.get("type") == "link" and n.get("goto"):
            out.append(("goto:" + n["goto"], None))
    return out


def collect_fields(node):
    """文档里所有输入框/下拉框的默认值：{name: value}。"""
    out = {}
    for n in walk(node):
        if n.get("type") in ("field", "select") and n.get("name"):
            out.setdefault(n["name"], n.get("value") or "")
    return out


def page_path(route):
    rel = ROUTE_FILES.get(route)
    if rel is None:
        return None
    return _layout.page_path(rel + EXT)


def make_request(route, action=None, fields=None, arg=None, history=None):
    """构造传给页面的请求对象（PAGE_SPEC §1）。"""
    return {
        "route": route,
        "action": action,
        "fields": fields or {},
        "arg": arg,
        "history": list(history or []),
    }


def error_doc(title, message):
    return {
        "type": "page",
        "children": [
            {"type": "heading", "level": 1, "text": title, "tone": "bad"},
            {"type": "text", "text": message, "tone": "bad"},
            {"type": "spacer", "size": 12},
            {"type": "button", "text": "回到首页", "goto": "shouye"},
        ],
    }


def error_response(title, message, status=500):
    return {"status": status, "title": title, "doc": error_doc(title, message),
            "log": [message], "engine": None, "ms": 0.0}


# --------------------------------------------------------------------------- #
# 运行时：一个进程一个引擎 + 页面解释器缓存
# --------------------------------------------------------------------------- #
class Runtime:
    def __init__(self, prefer=None, store_path=None):
        prefer = prefer or os.environ.get("SILLAGE_ENGINE") or "c"
        self.engine = langengine.Engine(prefer=prefer, store_path=store_path)
        self.mode = self.engine.mode
        self._pages = {}
        self._programs = {}

    # -- 页面 ------------------------------------------------------------- #
    def page_source(self, route):
        path = page_path(route)
        if not path or not os.path.isfile(path):
            raise DocError("缺少页面文件：%s" % (_layout.rel(path) if path else route))
        with open(path, "r", encoding="utf-8-sig") as fh:
            return fh.read()

    def interpreter(self, path):
        cached = self._pages.get(path)
        if cached is not None:
            return cached
        interp = self.engine.interpreter(path)
        self._pages[path] = interp
        return interp

    def render_page(self, route, req):
        if route not in ROUTE_FILES:
            return error_response("页面不存在", "地址栏里没有这个页面：%s" % route, 404)
        path = page_path(route)
        if not path or not os.path.isfile(path):
            return error_response(
                "页面还没写", "缺少页面文件：%s（复制 浏览器源码/页面模板.sil 开始）"
                % (_layout.rel(path) if path else route), 404)
        try:
            interp = self.interpreter(path)
            raw = interp.call("page", [req])
        except (DocError, langengine.EngineError) as exc:
            detail = exc.to_dict() if hasattr(exc, "to_dict") else str(exc)
            return error_response("页面运行出错",
                                   "SILLAGE 缺陷：%s" % json.dumps(detail, ensure_ascii=False),
                                   500)
        except Exception as exc:                        # noqa: BLE001
            return error_response("页面运行出错", "%s: %s" % (type(exc).__name__, exc), 500)
        return normalize(route, raw)

    def program_path_of(self, name):
        """解析程序路径：带 "/" 的按分发包根相对路径，否则在 示例程序/程序 里找。"""
        rel = name if name.endswith(EXT) else name + EXT
        rel = rel.replace("/", os.sep)
        if os.sep in rel:
            cand = os.path.join(_layout.ROOT, rel)
            if os.path.isfile(cand):
                return cand
        direct = _layout.program_path(rel)
        if os.path.isfile(direct):
            return direct
        base = os.path.basename(rel)
        for folder, _dirs, files in os.walk(_layout.path_of("programs")):
            if base in files:
                return os.path.join(folder, base)
        return direct

    # -- 程序 ------------------------------------------------------------- #
    def call_program(self, name, func, arg):
        """调程序（`示例程序/程序/zoo` 或裸名 `zoo`）的函数；返回 {ok, value} / {ok:false, défaut}。"""
        path = self.program_path_of(name)
        if not os.path.isfile(path):
            return {"ok": False, "défaut": {"étape": "hôte", "ligne": None,
                                            "note": "程序不存在：%s" % name}}
        try:
            interp = self._programs.get(path)
            if interp is None:
                interp = self.engine.interpreter(path)
                self._programs[path] = interp
            return {"ok": True, "value": interp.call(func, [arg])}
        except langengine.EngineError as exc:
            return {"ok": False, "défaut": exc.to_dict()}
        except Exception as exc:                        # noqa: BLE001
            return {"ok": False, "défaut": {"étape": "hôte", "ligne": None,
                                            "note": "%s: %s" % (type(exc).__name__, exc)}}

    def names_of(self, path):
        return self.engine.names_of(path)

    def collections(self):
        data = self.engine.collections() or {}
        if isinstance(data, dict) and "collections" in data:
            data = data["collections"]
        return data if isinstance(data, dict) else {}

    def close(self):
        try:
            self.engine.close()
        except Exception:
            pass


def normalize(route, raw):
    """把页面返回的法文键记录转成浏览器用的英文键，并做形状校验。"""

    def pick(d, *names, default=None):
        for name in names:
            if name in d:
                return d[name]
        return default

    if not isinstance(raw, dict):
        raise DocError("page 必须返回记录，实际返回 %r" % (raw,))
    status = pick(raw, "statut", "status", default=200)
    title = pick(raw, "titre", "title", default=route)
    doc = pick(raw, "doc", default=None)
    log = pick(raw, "journal", "log", default=[])
    try:
        status = int(status)
    except Exception:
        status = 500
    if not isinstance(title, str):
        title = str(title)
    if not isinstance(log, list):
        log = [str(log)]
    log = [x if isinstance(x, str) else json.dumps(x, ensure_ascii=False) for x in log]
    if not isinstance(doc, dict):
        raise DocError("page 必须返回 doc 文档树，实际返回 %r" % (doc,))
    nodes = validate_doc(doc)
    return {"status": status, "title": title, "doc": doc, "log": log, "nodes": nodes}


# --------------------------------------------------------------------------- #
# 命令行自查
# --------------------------------------------------------------------------- #
def main(argv=None):
    import argparse

    ap = argparse.ArgumentParser(description="SILLAGE 页面宿主（无界面）")
    ap.add_argument("route", nargs="?", default="shouye")
    ap.add_argument("--action", default=None)
    ap.add_argument("--field", action="append", default=[], help="k=v，可重复")
    ap.add_argument("--arg", default=None, help="JSON，按钮参数")
    ap.add_argument("--json", action="store_true", help="打印完整响应 JSON")
    ap.add_argument("--all", action="store_true", help="渲染全部路由，只报状态")
    ap.add_argument("--run", default=None, help="直接求值一段 SILLAGE 源码")
    ap.add_argument("--engine", default=None, help="c 或 py")
    args = ap.parse_args(argv)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    if args.run is not None:
        rt = Runtime(prefer=args.engine)
        try:
            out = rt.engine.run_source(args.run, "<cli>")
            print(json.dumps(out, ensure_ascii=False, indent=2))
        finally:
            rt.close()
        return 0

    rt = Runtime(prefer=args.engine)
    try:
        if args.all:
            bad = 0
            for route in ROUTES:
                resp = rt.render_page(route, make_request(route))
                ok = resp.get("status") == 200 and resp.get("nodes", 0) > 1
                print("  %-10s %-4s status=%-3s 节点=%s 标题=%s" % (
                    route, "PASS" if ok else "FAIL", resp.get("status"),
                    resp.get("nodes", 0), resp.get("title")))
                if not ok:
                    bad += 1
                    for line in (resp.get("log") or [])[:3]:
                        print("      " + line)
            print("=> %s" % ("PASS" if bad == 0 else "FAIL %d" % bad))
            return 0 if bad == 0 else 1

        fields = {}
        for item in args.field:
            if "=" in item:
                k, v = item.split("=", 1)
                fields[k] = v
        arg = json.loads(args.arg) if args.arg else None
        resp = rt.render_page(args.route, make_request(args.route, args.action, fields, arg))
        if args.json:
            print(json.dumps(resp, ensure_ascii=False, indent=2))
        else:
            print("status=%s title=%s 节点=%s" % (resp["status"], resp["title"],
                                                resp.get("nodes", 0)))
            print(extract_text(resp["doc"]))
        return 0 if resp["status"] == 200 else 1
    finally:
        rt.close()


if __name__ == "__main__":
    raise SystemExit(main())
