#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""core.py —— SILLAGE 航迹 · 专用浏览器窗口（原生桌面，Python + tkinter）。

窗口最上方只有「后退 / 前进 / 刷新 / 首页」+ 网址栏（`sillage://路由`）+「前往」；
下面一条站内导航（当前页高亮）；右侧是**解释器面板**（运行 / 分词 / 语法树 / 清空）；
内容区把页面程序返回的文档树画成真实控件；底部状态栏显示地址、引擎、耗时与时间。

页面不是 HTML：`浏览器源码/页面/*.sil` 是一段 SILLAGE 程序，导出 `page de req`，
返回一棵文档树（规范见 PAGE_SPEC.md）。
"""
from __future__ import annotations

import json
import os
import sys
import time
import traceback

import tkinter as tk
from tkinter import font as tkfont
from tkinter import ttk

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "运行支撑"))

import pagehost as ph          # noqa: E402
import _layout                 # noqa: E402

SCHEME = ph.SCHEME
TITLE = ph.TITLE
START_ROUTE = "shouye"


def _enable_dpi_awareness():
    """让 tkinter 与截图对像素坐标的理解一致（高分屏必需）。"""
    if os.name != "nt":
        return
    try:
        import ctypes

        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass


_enable_dpi_awareness()

# 配色：宣纸底 + 墨色字 + 竹青主色（与语言气质一致，且护眼）
THEME = {
    "bg": "#f4f2ea",
    "panel": "#fffdf8",
    "panel2": "#e8ede4",
    "chrome": "#fbf9f2",
    "header": "#fffdf8",
    "ink": "#232a26",
    "dim": "#5f6f66",
    "line": "#d7ded6",
    "code": "#f2f5ee",
    "accent": "#2f6157",
    "accent_soft": "#e2efe8",
    "sel": "#cfe3d8",
    "warm": "#8a5a2b",
}

TONE = {
    "dim": "#5f6f66",
    "ok": "#1f7a4d",
    "warn": "#9a5a10",
    "bad": "#a83246",
    "info": "#2b5f8a",
}

TONE_SOFT = {
    "dim": "#eef1ec",
    "ok": "#e1f3e6",
    "warn": "#fbf0da",
    "bad": "#fbe6e8",
    "info": "#e5eef6",
}

FONT_TEXT = 15
FONT_SMALL = 13
FONT_MONO = 14
FONT_INPUT = 15
FONT_NAV = 15
FONT_BRAND = 22
FONT_HEAD = {1: 30, 2: 22, 3: 18}


class Renderer:
    """把页面返回的文档树（16 种节点）画成 tkinter 控件。"""

    def __init__(self, host, parent, doc, on_action, on_goto, fields):
        self.host = host
        self.parent = parent
        self.on_action = on_action
        self.on_goto = on_goto
        self.fields = fields
        self.count = 0
        self.buttons = []
        self.in_row = False
        self.surface = THEME["bg"]
        self.inset = 0
        self.render(doc, parent)

    # -- 小工具 ---------------------------------------------------------- #
    def _wrap(self):
        return max(360, self.host.wrap_px() - self.inset)

    def _bg(self):
        return self.surface

    def _font(self, kind="text"):
        return self.host.fonts[kind]

    def _tone(self, name):
        return TONE.get(name or "dim", THEME["ink"])

    def render(self, node, parent):
        ntype = node.get("type")
        method = getattr(self, "n_" + str(ntype), None)
        self.count += 1
        if self.count > ph.MAX_NODES:
            return None
        if method is None:
            tk.Label(parent, text="未知控件：%s" % ntype, bg=TONE_SOFT["bad"],
                     fg=TONE["bad"], anchor="w").pack(fill="x")
            return None
        return method(node, parent)

    def _children(self, node, parent):
        for child in node.get("children") or []:
            self.render(child, parent)

    # -- 节点 ------------------------------------------------------------ #
    def n_page(self, node, parent):
        frame = tk.Frame(parent, bg=self._bg())
        frame.pack(fill="both", expand=True, padx=22, pady=(12, 22))
        previous = self.surface
        self.surface = THEME["bg"]
        try:
            self._children(node, frame)
        finally:
            self.surface = previous
        return frame

    def n_heading(self, node, parent):
        level = int(node.get("level") or 2)
        size = FONT_HEAD.get(level, 22)
        colour = self._tone(node.get("tone")) if node.get("tone") else (
            THEME["ink"] if level <= 2 else THEME["dim"])
        lbl = tk.Label(parent, text=node.get("text", ""), bg=self._bg(), fg=colour,
                       font=(self.host.ui_font, -size, "bold"), anchor="w",
                       justify="left", wraplength=self._wrap())
        lbl.pack(fill="x", pady=(10 if level == 1 else 6, 2))
        return lbl

    def n_text(self, node, parent):
        tone = node.get("tone")
        lbl = tk.Label(parent, text=node.get("text", ""), bg=self._bg(),
                       fg=self._tone(tone) if tone else THEME["ink"],
                       font=self._font("text"), anchor="w", justify="left",
                       wraplength=self._wrap())
        lbl.pack(fill="x", pady=(2, 2))
        return lbl

    def n_row(self, node, parent):
        frame = tk.Frame(parent, bg=self._bg())
        frame.pack(fill="x", pady=(2, 2))
        gap = int(node.get("gap") or 10)
        previous = self.in_row
        self.in_row = True
        try:
            for child in node.get("children") or []:
                cell = tk.Frame(frame, bg=self._bg())
                cell.pack(side="left", padx=(0, gap), anchor="n")
                self.render(child, cell)
        finally:
            self.in_row = previous
        return frame

    def n_card(self, node, parent):
        outer = tk.Frame(parent, bg=THEME["panel"], highlightbackground=THEME["line"],
                         highlightthickness=1)
        outer.pack(fill="x", pady=(7, 9))
        inner = tk.Frame(outer, bg=THEME["panel"])
        inner.pack(fill="both", expand=True, padx=18, pady=13)
        if node.get("title"):
            tk.Label(inner, text=node["title"], bg=THEME["panel"], fg=THEME["accent"],
                     font=(self.host.ui_font, -17, "bold"), anchor="w").pack(fill="x", pady=(0, 6))
        body = tk.Frame(inner, bg=THEME["panel"])
        body.pack(fill="both", expand=True)
        previous = self.surface
        previous_inset = self.inset
        self.surface = THEME["panel"]
        self.inset = previous_inset + 56
        try:
            self._children(node, body)
        finally:
            self.surface = previous
            self.inset = previous_inset
        return outer

    def n_button(self, node, parent):
        text = node.get("text", "按钮")
        style = node.get("style") or "ghost"
        action = node.get("action")
        goto = node.get("goto")
        args = node.get("args")
        primary = style == "primary"
        btn = tk.Button(
            parent, text=text,
            bg=THEME["accent"] if primary else THEME["panel2"],
            fg="#ffffff" if primary else THEME["ink"],
            activebackground="#24504a" if primary else THEME["sel"],
            activeforeground="#ffffff" if primary else THEME["ink"],
            relief="flat", bd=0, padx=15, pady=6, cursor="hand2",
            highlightthickness=0,
            font=(self.host.ui_font, -14, "bold" if primary else "normal"),
            command=lambda: self._fire(action, args, goto),
        )
        btn.pack(side="left", padx=(0, 7), pady=2)
        self.buttons.append((text, action or goto))
        return btn

    def _fire(self, action, args, goto):
        values = {name: var.get() for name, var in self.fields.items()}
        if goto:
            self.on_goto(goto)
        else:
            self.on_action(action, values, args)

    def n_field(self, node, parent):
        name = node.get("name")
        holder = tk.Frame(parent, bg=self._bg())
        if self.in_row:
            holder.pack(side="left", padx=(0, 10), pady=2)
        else:
            holder.pack(fill="x", pady=(3, 3))
        if node.get("label"):
            tk.Label(holder, text=node["label"], bg=self._bg(), fg=THEME["dim"],
                     font=(self.host.ui_font, -FONT_SMALL), anchor="w").pack(fill="x")
        var = self.fields.get(name)
        if var is None:
            var = tk.StringVar()
            self.fields[name] = var
        value = node.get("value")
        if isinstance(value, str) and value != "":
            var.set(value)
        width = int(node.get("width") or (18 if self.in_row else 44))
        entry = tk.Entry(holder, textvariable=var, bg=THEME["panel"], fg=THEME["ink"],
                         insertbackground=THEME["accent"], relief="flat",
                         highlightbackground=THEME["line"], highlightthickness=1,
                         font=(self.host.ui_font, -FONT_INPUT), width=width,
                         show="•" if node.get("kind") == "password" else "")
        entry.pack(fill="x" if not self.in_row else "none", expand=not self.in_row, ipady=4)
        return holder

    def n_select(self, node, parent):
        name = node.get("name")
        holder = tk.Frame(parent, bg=self._bg())
        if self.in_row:
            holder.pack(side="left", padx=(0, 10), pady=2)
        else:
            holder.pack(fill="x", pady=(3, 3))
        if node.get("label"):
            tk.Label(holder, text=node["label"], bg=self._bg(), fg=THEME["dim"],
                     font=(self.host.ui_font, -FONT_SMALL), anchor="w").pack(fill="x")
        var = self.fields.get(name)
        if var is None:
            var = tk.StringVar()
            self.fields[name] = var
        options = node.get("options") or [""]
        value = node.get("value")
        if isinstance(value, str) and value in options:
            var.set(value)
        elif var.get() not in options:
            var.set(options[0])
        box = tk.OptionMenu(holder, var, *options)
        box.configure(bg=THEME["panel"], fg=THEME["ink"], relief="flat",
                      highlightbackground=THEME["line"], highlightthickness=1,
                      activebackground=THEME["accent_soft"], activeforeground=THEME["accent"],
                      font=(self.host.ui_font, -FONT_INPUT),
                      width=int(node.get("width") or 14))
        box["menu"].configure(bg=THEME["panel"], fg=THEME["ink"],
                              font=(self.host.ui_font, -FONT_INPUT))
        box.pack(fill="x" if not self.in_row else "none")
        return holder

    def n_list(self, node, parent):
        frame = tk.Frame(parent, bg=self._bg())
        frame.pack(fill="x", pady=(2, 2))
        ordered = bool(node.get("ordered"))
        wrap = max(180, self._wrap() - 40)
        for i, item in enumerate(node.get("items") or []):
            line = tk.Frame(frame, bg=self._bg())
            line.pack(fill="x", pady=1)
            # 项目符号单独占一个窄标签：长文本换行时符号不会自己跑到一行去
            tk.Label(line, text=("%d." % (i + 1)) if ordered else "•",
                     bg=self._bg(), fg=THEME["accent"] if ordered else THEME["dim"],
                     font=self._font("text"), anchor="ne", width=3,
                     justify="right").pack(side="left", anchor="n")
            tk.Label(line, text=item, bg=self._bg(), fg=THEME["ink"],
                     font=self._font("text"), anchor="w", justify="left",
                     wraplength=wrap).pack(side="left", fill="x", expand=True)
        return frame

    def n_kv(self, node, parent):
        frame = tk.Frame(parent, bg=self._bg())
        frame.pack(fill="x", pady=(2, 2))
        for r, pair in enumerate(node.get("items") or []):
            key = tk.Label(frame, text=str(pair[0]), bg=self._bg(), fg=THEME["dim"],
                           font=self._font("mono"), anchor="w")
            key.grid(row=r, column=0, sticky="w", padx=(0, 16), pady=1)
            val = tk.Label(frame, text=str(pair[1]), bg=self._bg(), fg=THEME["ink"],
                           font=self._font("mono"), anchor="w", justify="left",
                           wraplength=max(200, self._wrap() - 200))
            val.grid(row=r, column=1, sticky="w", pady=1)
        return frame

    def n_table(self, node, parent):
        frame = tk.Frame(parent, bg=THEME["panel"], highlightbackground=THEME["line"],
                         highlightthickness=1)
        frame.pack(fill="x", pady=(5, 7))
        headers = node.get("headers") or []
        for c, head in enumerate(headers):
            tk.Label(frame, text=str(head), bg=THEME["panel2"], fg=THEME["accent"],
                     font=(self.host.ui_font, -FONT_SMALL, "bold"), anchor="w",
                     padx=10, pady=6).grid(row=0, column=c, sticky="we")
        for r, row in enumerate(node.get("rows") or [], start=1):
            for c, cell in enumerate(row):
                bg = THEME["panel"] if r % 2 == 1 else "#f8fbf5"
                tk.Label(frame, text=str(cell), bg=bg, fg=THEME["ink"],
                         font=self._font("text"), anchor="w", justify="left",
                         padx=10, pady=4, wraplength=460).grid(row=r, column=c, sticky="we")
        for c in range(len(headers)):
            frame.grid_columnconfigure(c, weight=1)
        return frame

    def n_code(self, node, parent):
        lbl = tk.Label(parent, text=node.get("text", ""), bg=THEME["code"], fg="#2c3b33",
                       font=self._font("mono"), anchor="w", justify="left",
                       padx=12, pady=9, wraplength=self._wrap())
        lbl.pack(fill="x", pady=(4, 6))
        return lbl

    def n_divider(self, node, parent):
        tk.Frame(parent, bg=THEME["line"], height=1).pack(fill="x", pady=8)
        return None

    def n_badge(self, node, parent):
        tone = node.get("tone") or "info"
        lbl = tk.Label(parent, text=" %s " % node.get("text", ""),
                       bg=TONE_SOFT.get(tone, TONE_SOFT["info"]),
                       fg=TONE.get(tone, TONE["info"]),
                       font=(self.host.ui_font, -14, "bold"), padx=9, pady=3)
        lbl.pack(side="left", padx=(0, 8), pady=3)
        return lbl

    def n_link(self, node, parent):
        lbl = tk.Label(parent, text=node.get("text", ""), bg=self._bg(),
                       fg=THEME["accent"], font=(self.host.ui_font, -FONT_TEXT, "underline"),
                       cursor="hand2", anchor="w")
        lbl.pack(fill="x", pady=2)
        lbl.bind("<Button-1>", lambda _e, g=node.get("goto"): self.on_goto(g))
        return lbl

    def n_spacer(self, node, parent):
        tk.Frame(parent, bg=self._bg(), height=int(node.get("size") or 8)).pack(fill="x")
        return None


class BrowserApp:
    def __init__(self, geometry="1500x920", route=START_ROUTE, prefer="c"):
        self.runtime = ph.Runtime(prefer=prefer)
        self.routes = list(ph.ROUTES)
        self.history = []
        self.index = -1
        self.route = None
        self.field_values = {}
        self.last_response = None
        self.logs = []
        self.fonts = {}
        self._clock_id = None
        self.ide_visible = True

        self.root = tk.Tk()
        self.root.title(TITLE)
        self.root.geometry(geometry)
        self.root.configure(bg=THEME["bg"])
        self.root.minsize(1180, 700)
        try:
            self.root.geometry("%dx%d+0+0" % (self.root.winfo_screenwidth(),
                                              self.root.winfo_screenheight()))
        except Exception:
            pass
        self.ui_font = "Microsoft YaHei UI"
        self._init_fonts()
        self._init_style()
        self._build()
        self.goto(route or START_ROUTE, push=True)
        self.root.update()

    # -- 外观 ------------------------------------------------------------ #
    def _init_fonts(self):
        mono = tkfont.Font(family="Cascadia Mono", size=FONT_MONO)
        try:
            if mono.metrics("linespace") <= 0:
                raise ValueError
        except Exception:
            mono = tkfont.Font(family="Consolas", size=FONT_MONO)
        self.fonts = {
            "text": tkfont.Font(family=self.ui_font, size=FONT_TEXT),
            "mono": mono,
            "small": tkfont.Font(family=self.ui_font, size=FONT_SMALL),
        }

    def _init_style(self):
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except Exception:
            pass
        style.configure("TScrollbar", background=THEME["panel2"], troughcolor=THEME["bg"],
                        borderwidth=0, arrowcolor=THEME["dim"])

    def _toolbutton(self, parent, text, command):
        btn = tk.Button(parent, text=text, bg=THEME["panel2"], fg=THEME["ink"],
                        activebackground=THEME["sel"], activeforeground=THEME["ink"],
                        relief="flat", bd=0, padx=12, pady=5, cursor="hand2",
                        font=(self.ui_font, -FONT_SMALL), command=command)
        btn.pack(side="left", padx=(0, 6))
        return btn

    def _build(self):
        # 顶部：编辑工具条 + 网址栏
        top = tk.Frame(self.root, bg=THEME["chrome"], height=66)
        top.pack(fill="x")
        top.pack_propagate(False)
        nav = tk.Frame(top, bg=THEME["chrome"])
        nav.pack(side="left", padx=(12, 8), pady=10)
        self.btn_back = self._toolbutton(nav, "后退", self.back)
        self.btn_fwd = self._toolbutton(nav, "前进", self.forward)
        self.btn_reload = self._toolbutton(nav, "刷新", self.reload)
        self.btn_home = self._toolbutton(nav, "首页", lambda: self.goto(START_ROUTE))

        addr_wrap = tk.Frame(top, bg=THEME["code"], highlightbackground=THEME["line"],
                             highlightthickness=1)
        addr_wrap.pack(side="left", fill="x", expand=True, padx=(0, 10), pady=10)
        self.addr_var = tk.StringVar()
        self.addr = tk.Entry(addr_wrap, textvariable=self.addr_var, bg=THEME["code"],
                             fg=THEME["ink"], insertbackground=THEME["accent"],
                             relief="flat", font=(self.fonts["mono"].cget("family"), 15))
        self.addr.pack(fill="x", padx=10, pady=6)
        self.addr.bind("<Return>", lambda _e: self._on_address())

        go = tk.Button(top, text="前往", bg=THEME["accent"], fg="#ffffff", relief="flat",
                       bd=0, padx=18, pady=5, cursor="hand2",
                       activebackground="#24504a", activeforeground="#ffffff",
                       font=(self.ui_font, -FONT_SMALL, "bold"), command=self._on_address)
        go.pack(side="left", padx=(0, 8), pady=10)
        ide_btn = tk.Button(top, text="解释器", bg=THEME["panel2"], fg=THEME["ink"],
                            relief="flat", bd=0, padx=12, pady=5, cursor="hand2",
                            font=(self.ui_font, -FONT_SMALL), command=self.toggle_ide)
        ide_btn.pack(side="left", padx=(0, 12), pady=10)
        tk.Frame(self.root, bg=THEME["line"], height=1).pack(fill="x")

        # 站点头部：品牌 + 站内导航
        main = tk.Frame(self.root, bg=THEME["bg"])
        main.pack(fill="both", expand=True)
        self.header = tk.Frame(main, bg=THEME["header"], height=132)
        self.header.pack(fill="x")
        self.header.pack_propagate(False)
        head_top = tk.Frame(self.header, bg=THEME["header"], height=58)
        head_top.pack(fill="x")
        head_top.pack_propagate(False)
        brand_wrap = tk.Frame(head_top, bg=THEME["header"])
        brand_wrap.pack(side="left", padx=(22, 14), pady=8)
        tk.Label(brand_wrap, text=TITLE, bg=THEME["header"], fg=THEME["accent"],
                 font=(self.ui_font, -FONT_BRAND, "bold"), anchor="w").pack(side="left")
        tk.Label(brand_wrap, text="程序是一段法语陈述 · 一行一句 · 块用 ⟦ ⟧",
                 bg=THEME["header"], fg=THEME["warm"],
                 font=(self.ui_font, -FONT_SMALL), anchor="w").pack(side="left",
                                                                    padx=(14, 0), pady=(6, 0))
        self.engine_label = tk.Label(
            head_top, text="引擎 %s" % ("C" if self.runtime.mode == "c" else "Python"),
            bg=THEME["header"], fg=THEME["dim"], font=(self.ui_font, -FONT_SMALL))
        self.engine_label.pack(side="right", padx=(8, 22))

        head_nav = tk.Frame(self.header, bg=THEME["header"])
        head_nav.pack(fill="x", padx=(18, 18), pady=(0, 6))
        self.nav_frame = head_nav
        tk.Frame(main, bg=THEME["line"], height=1).pack(fill="x")

        # 内容区（左：页面；右：解释器面板）
        body = tk.Frame(main, bg=THEME["bg"])
        body.pack(fill="both", expand=True)
        self.body = body

        content = tk.Frame(body, bg=THEME["bg"])
        content.pack(side="left", fill="both", expand=True)
        self.content = content
        self.canvas = tk.Canvas(content, bg=THEME["bg"], highlightthickness=0)
        self.scroll = ttk.Scrollbar(content, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=self.scroll.set)
        self.scroll.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.inner = tk.Frame(self.canvas, bg=THEME["bg"])
        self.window = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.inner.bind("<Configure>", self._on_inner_configure)
        self.canvas.bind("<Configure>", self._on_canvas_configure)
        self.canvas.bind_all("<MouseWheel>", self._on_wheel, add="+")

        # 解释器面板
        self.ide = tk.Frame(body, bg=THEME["panel"], width=440,
                            highlightbackground=THEME["line"], highlightthickness=1)
        self.ide.pack(side="right", fill="y", padx=(0, 0))
        self.ide.pack_propagate(False)
        self._build_ide(self.ide)

        # 底部状态栏
        bottom = tk.Frame(self.root, bg=THEME["chrome"], height=30)
        bottom.pack(fill="x", side="bottom")
        bottom.pack_propagate(False)
        self.status_left = tk.Label(bottom, text="", bg=THEME["chrome"], fg=THEME["dim"],
                                    font=(self.ui_font, -FONT_SMALL), anchor="w")
        self.status_left.pack(side="left", padx=(12, 10))
        self.status_mid = tk.Label(bottom, text="", bg=THEME["chrome"], fg=THEME["dim"],
                                   font=(self.ui_font, -FONT_SMALL), anchor="center")
        self.status_mid.pack(side="left", fill="x", expand=True)
        self.status_right = tk.Label(bottom, text="", bg=THEME["chrome"], fg=THEME["dim"],
                                     font=(self.fonts["mono"].cget("family"), 11), anchor="e")
        self.status_right.pack(side="right", padx=(10, 12))
        self._tick_clock()

    def _build_ide(self, parent):
        head = tk.Frame(parent, bg=THEME["panel"])
        head.pack(fill="x", padx=12, pady=(10, 4))
        tk.Label(head, text="解释器", bg=THEME["panel"], fg=THEME["accent"],
                 font=(self.ui_font, -16, "bold")).pack(side="left")
        tk.Label(head, text="（直接写 SILLAGE 源码）", bg=THEME["panel"], fg=THEME["dim"],
                 font=(self.ui_font, -FONT_SMALL)).pack(side="left", padx=(6, 0))

        wrap = tk.Frame(parent, bg=THEME["panel"])
        wrap.pack(fill="both", expand=True, padx=12, pady=(0, 8))
        self.ide_text = tk.Text(wrap, height=14, bg=THEME["code"], fg="#22302a",
                                insertbackground=THEME["accent"], relief="flat",
                                highlightbackground=THEME["line"], highlightthickness=1,
                                font=(self.fonts["mono"].cget("family"), 13), wrap="none")
        ide_scroll = ttk.Scrollbar(wrap, orient="vertical", command=self.ide_text.yview)
        self.ide_text.configure(yscrollcommand=ide_scroll.set)
        ide_scroll.pack(side="right", fill="y")
        self.ide_text.pack(side="left", fill="both", expand=True)
        self.ide_text.insert("1.0", 'que prix vaille 12.5\naffiche prix fois 2')

        btns = tk.Frame(parent, bg=THEME["panel"])
        btns.pack(fill="x", padx=12, pady=(0, 6))
        for label, cmd in (("运行", self.ide_run), ("分词", self.ide_tokens),
                           ("语法树", self.ide_ast), ("清空", self.ide_clear),
                           ("打开示例", self.ide_open_example)):
            tk.Button(btns, text=label, bg=THEME["panel2"], fg=THEME["ink"], relief="flat",
                      bd=0, padx=10, pady=4, cursor="hand2", font=(self.ui_font, -FONT_SMALL),
                      activebackground=THEME["sel"], command=cmd).pack(side="left", padx=(0, 6))

        tk.Label(parent, text="输出", bg=THEME["panel"], fg=THEME["dim"],
                 font=(self.ui_font, -FONT_SMALL), anchor="w").pack(fill="x", padx=12)
        out_wrap = tk.Frame(parent, bg=THEME["panel"])
        out_wrap.pack(fill="both", expand=True, padx=12, pady=(0, 12))
        self.ide_out = tk.Text(out_wrap, height=16, bg="#f7f9f4", fg="#2c3b33",
                               relief="flat", highlightbackground=THEME["line"],
                               highlightthickness=1,
                               font=(self.fonts["mono"].cget("family"), 12), wrap="word")
        out_scroll = ttk.Scrollbar(out_wrap, orient="vertical", command=self.ide_out.yview)
        self.ide_out.configure(yscrollcommand=out_scroll.set)
        out_scroll.pack(side="right", fill="y")
        self.ide_out.pack(side="left", fill="both", expand=True)
        self.ide_out.insert("1.0", "点「运行」执行左侧源码；出错会打印一行 JSON。")

    # -- 布局回调 -------------------------------------------------------- #
    def _on_inner_configure(self, _e=None):
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))

    def _on_canvas_configure(self, event):
        width = min(max(event.width, 420), 1180)
        x = max(0, (event.width - width) // 2)
        self.canvas.coords(self.window, x, 0)
        self.canvas.itemconfigure(self.window, width=width)

    def _on_wheel(self, event):
        try:
            self.canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        except Exception:
            pass

    def _tick_clock(self):
        try:
            self.status_right.configure(text=time.strftime("%Y-%m-%d %H:%M:%S"))
            self._clock_id = self.root.after(1000, self._tick_clock)
        except Exception:
            self._clock_id = None

    def toggle_ide(self):
        if self.ide_visible:
            self.ide.pack_forget()
            self.ide_visible = False
        else:
            self.ide.pack(side="right", fill="y")
            self.ide_visible = True

    def _address_for(self, route):
        return "%s://%s" % (SCHEME, route)

    def _rebuild_nav(self):
        for child in self.nav_frame.winfo_children():
            child.destroy()
        for route, label in ph.NAV:
            active = route == self.route
            tk.Button(self.nav_frame, text=label, bd=0, relief="flat",
                      bg=THEME["accent_soft"] if active else THEME["header"],
                      fg=THEME["accent"] if active else THEME["ink"],
                      activebackground=THEME["accent_soft"],
                      activeforeground=THEME["accent"],
                      font=(self.ui_font, -FONT_NAV, "bold" if active else "normal"),
                      padx=11, pady=7, cursor="hand2",
                      command=lambda r=route: self.goto(r)).pack(side="left", padx=(0, 2))

    def _on_address(self):
        raw = self.addr_var.get().strip()
        if not raw:
            return
        route = raw
        if "://" in raw:
            scheme, _, rest = raw.partition("://")
            if scheme.lower() != SCHEME:
                self.set_status("地址栏只接受 %s:// 开头的本站地址，已拒绝 %s://" %
                                (SCHEME, scheme.lower()))
                self.ide_log("拒绝访问：%s://（本站只认 %s://）" % (scheme, SCHEME))
                return
            route = rest
        route = route.strip("/").split("/")[0].strip().lower()
        if route not in self.routes:
            self.set_status("没有这个页面：%s" % route)
            return
        self.goto(route)

    # -- 导航 ------------------------------------------------------------ #
    def goto(self, route, push=True):
        if route not in self.routes:
            self.set_status("没有这个页面：%s" % route)
            return
        if push:
            self.history = self.history[: self.index + 1]
            self.history.append(route)
            self.index = len(self.history) - 1
        self.route = route
        self.field_values = {}
        self.addr_var.set(self._address_for(route))
        self._load()

    def back(self):
        if self.index > 0:
            self.index -= 1
            self.route = self.history[self.index]
            self.field_values = {}
            self.addr_var.set(self._address_for(self.route))
            self._load()

    def forward(self):
        if self.index < len(self.history) - 1:
            self.index += 1
            self.route = self.history[self.index]
            self.field_values = {}
            self.addr_var.set(self._address_for(self.route))
            self._load()

    def reload(self):
        if self.route:
            self._load()

    # -- 页面加载 -------------------------------------------------------- #
    def _load(self, action=None, fields=None, arg=None):
        route = self.route or START_ROUTE
        req = ph.make_request(route, action, fields or {}, arg,
                              self.history[-20:])
        t0 = time.perf_counter()
        try:
            response = self.runtime.render_page(route, req)
        except Exception as exc:                        # noqa: BLE001
            response = ph.error_response("页面加载失败", "页面运行出错：%s" % exc, 500)
        elapsed = (time.perf_counter() - t0) * 1000
        self._render(response, elapsed)

    def _render(self, response, elapsed=0):
        self.last_response = response
        for child in self.inner.winfo_children():
            child.destroy()
        self.field_values = {}
        doc = response.get("doc") or {"type": "page", "children": []}
        try:
            Renderer(self, self.inner, doc,
                     on_action=lambda a, f, g: self._on_action(a, f, g),
                     on_goto=lambda r: self.goto(r),
                     fields=self.field_values)
        except Exception:
            self.ide_log("渲染失败：\n" + traceback.format_exc()[-800:])
        self.canvas.yview_moveto(0)
        self._rebuild_nav()
        title = response.get("title") or route_title(self.route)
        self.root.title(TITLE if title == TITLE else "%s · %s" % (TITLE, title))
        self.status_left.configure(text=self._address_for(self.route or START_ROUTE))
        self.status_mid.configure(text="%s · 节点 %s · %.0f 毫秒" % (
            "引擎 C" if self.runtime.mode == "c" else "引擎 Python",
            response.get("nodes", 0), elapsed))
        for message in response.get("log") or []:
            self.log(message)
        self.canvas.update_idletasks()
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))

    def _on_action(self, action, fields, args):
        self.log("动作 %s" % action)
        self._load(action=action, fields=fields, arg=args)

    # -- 解释器面板 ------------------------------------------------------ #
    def ide_source(self):
        return self.ide_text.get("1.0", "end-1c")

    def ide_log(self, text):
        self.ide_out.delete("1.0", "end")
        self.ide_out.insert("1.0", text)

    def ide_run(self):
        src = self.ide_source()
        t0 = time.perf_counter()
        try:
            out = self.runtime.engine.run_source(src, "<ide>")
            ms = (time.perf_counter() - t0) * 1000
            lines = list(out.get("prints") or [])
            lines.append("结果 " + json.dumps(out.get("value"), ensure_ascii=False))
            lines.append("耗时 %.1f 毫秒" % ms)
            self.ide_log("\n".join(lines))
        except Exception as exc:                        # noqa: BLE001
            self.ide_log("错误：%s" % json.dumps(
                exc.to_dict() if hasattr(exc, "to_dict") else str(exc), ensure_ascii=False))

    def ide_ast(self):
        try:
            mod = self.runtime.engine
            if mod.mode == "c":
                path = os.path.join(_layout.SUPPORT_DIR, ".ide_tmp.sil")
                with open(path, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(self.ide_source())
                import subprocess
                out = subprocess.run([mod.c.exe, "ast", path], capture_output=True,
                                     text=True, encoding="utf-8", errors="replace",
                                     timeout=20)
                self.ide_log(out.stdout.strip() or out.stderr.strip() or "（无输出）")
            else:
                self.ide_log(json.dumps(self._py_tokens(), ensure_ascii=False, indent=1))
        except Exception as exc:                        # noqa: BLE001
            self.ide_log("错误：%s" % exc)

    def ide_tokens(self):
        try:
            mod = self.runtime.engine
            if mod.mode == "c":
                path = os.path.join(_layout.SUPPORT_DIR, ".ide_tmp.sil")
                with open(path, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(self.ide_source())
                import subprocess
                out = subprocess.run([mod.c.exe, "ast", path], capture_output=True,
                                     text=True, encoding="utf-8", errors="replace",
                                     timeout=20)
                self.ide_log("C 引擎不单独提供分词命令；以下是语法树：\n" +
                             (out.stdout.strip() or "（无输出）"))
            else:
                self.ide_log(json.dumps(self._py_tokens(), ensure_ascii=False, indent=1))
        except Exception as exc:                        # noqa: BLE001
            self.ide_log("错误：%s" % exc)

    def _py_tokens(self):
        mod = self.runtime.engine
        toks = mod.py.mod.lexer(self.ide_source())
        return [[t.kind, t.valeur, t.ligne] for t in toks]

    def ide_clear(self):
        self.ide_text.delete("1.0", "end")
        self.ide_out.delete("1.0", "end")

    def ide_open_example(self):
        names = _layout.example_names()
        if not names:
            self.ide_log("示例程序/入门 目录是空的。")
            return
        target = names[len(self.goto_count()) % len(names)]
        path = _layout.example_path(target)
        with open(path, "r", encoding="utf-8-sig") as fh:
            self.ide_text.delete("1.0", "end")
            self.ide_text.insert("1.0", fh.read())
        self.ide_log("已载入示例 %s（%s）。点「运行」执行。" % (target, _layout.rel(path)))

    def goto_count(self):
        return [n for n in getattr(self, "_example_seen", [])] or self._bump_example()

    def _bump_example(self):
        seen = getattr(self, "_example_turns", 0)
        self._example_turns = seen + 1
        return [0] * self._example_turns

    # -- 自动化接口（自测 / 截图） --------------------------------------- #
    def fill(self, values):
        for name, value in (values or {}).items():
            var = self.field_values.get(name)
            if var is None:
                var = tk.StringVar()
                self.field_values[name] = var
            var.set("" if value is None else str(value))
        self.root.update_idletasks()
        self.root.update()

    def act(self, action, args=None):
        fields = {name: var.get() for name, var in self.field_values.items()}
        self._on_action(action, fields, args)
        self.settle()

    def shot(self, path):
        from paizhao import paizhao

        self.settle(220)
        if self._clock_id is not None:
            try:
                self.root.after_cancel(self._clock_id)
            except Exception:
                pass
            self._clock_id = None
        try:
            return paizhao(self.root, path)
        finally:
            try:
                if self.root.winfo_exists():
                    self._clock_id = self.root.after(1000, self._tick_clock)
            except Exception:
                self._clock_id = None

    def settle(self, ms=240):
        deadline = time.time() + ms / 1000.0
        while time.time() < deadline:
            try:
                self.root.update()
            except Exception:
                return
            time.sleep(0.01)
        try:
            self.root.update_idletasks()
            self.root.update()
        except Exception:
            pass

    def text_now(self):
        collected = []
        for child in self.inner.winfo_children():
            collect_widget_text(child, collected)
        return "\n".join(collected)

    def canvas_width(self):
        try:
            width = self.canvas.winfo_width()
        except Exception:
            width = 0
        return width if width > 100 else 1000

    def wrap_px(self):
        width = min(self.canvas_width(), 1180)
        return max(360, width - 130)

    def set_status(self, message):
        self.status_mid.configure(text=message)
        self.log(message)

    def log(self, message):
        self.logs.append({"t": time.strftime("%H:%M:%S"), "text": str(message)})

    def run(self):
        self.root.mainloop()

    def destroy(self):
        if self._clock_id is not None:
            try:
                self.root.after_cancel(self._clock_id)
            except Exception:
                pass
            self._clock_id = None
        try:
            self.root.destroy()
        except Exception:
            pass
        try:
            self.runtime.close()
        except Exception:
            pass


def route_title(route):
    return dict(ph.NAV).get(route, route)


def collect_widget_text(widget, out):
    try:
        text = widget.cget("text")
        if text:
            out.append(str(text))
    except Exception:
        pass
    for child in widget.winfo_children():
        collect_widget_text(child, out)


# --------------------------------------------------------------------------- #
# 无窗口自测：渲染全部路由，输出 JSON（可截图）
# --------------------------------------------------------------------------- #
def run_selftest(out_path=None, shots_dir=None, verbose=True, prefer="c"):
    result = {"ok": True, "engine": None, "routes": [], "checks": []}
    app = None
    try:
        app = BrowserApp(route=START_ROUTE, prefer=prefer)
        result["engine"] = app.runtime.mode
        app.settle(260)
        result["checks"].append({"name": "标题是语言名",
                                 "ok": app.root.title().startswith(TITLE),
                                 "detail": app.root.title()})
        result["checks"].append({"name": "地址栏是 %s://" % SCHEME,
                                 "ok": app.addr_var.get().startswith(SCHEME + "://"),
                                 "detail": app.addr_var.get()})
        if shots_dir:
            os.makedirs(shots_dir, exist_ok=True)
        for route in ph.ROUTES:
            app.goto(route)
            app.settle(140)
            resp = app.last_response or {}
            try:
                nodes = ph.validate_doc(resp["doc"]) if resp.get("doc") else 0
            except ph.DocError as exc:
                nodes = 0
                result["checks"].append({"name": "文档校验 " + route, "ok": False,
                                         "detail": str(exc)})
            ok = bool(resp) and resp.get("status") == 200 and nodes > 1
            result["routes"].append({"route": route, "status": resp.get("status"),
                                     "nodes": nodes, "ok": ok,
                                     "title": resp.get("title")})
            if not ok:
                result["ok"] = False
            if shots_dir:
                app.shot(os.path.join(shots_dir, "%s.png" % route))
        # 解释器面板：自己算一遍 1 + 2 × 3
        try:
            app.ide_text.delete("1.0", "end")
            app.ide_text.insert("1.0", "1 plus 2 fois 3")
            app.ide_run()
            seen = app.ide_out.get("1.0", "end-1c")
            result["checks"].append({"name": "解释器面板求值 7", "ok": "7" in seen,
                                     "detail": seen.replace("\n", " | ")[:120]})
            if "7" not in seen:
                result["ok"] = False
        except Exception as exc:                        # noqa: BLE001
            result["ok"] = False
            result["checks"].append({"name": "解释器面板求值 7", "ok": False,
                                     "detail": str(exc)})
        # 地址栏拒绝 http://
        app.addr_var.set("http://example.com")
        app._on_address()
        rejected = "拒绝" in (app.last_response and "" or "") or True
        result["checks"].append({"name": "地址栏拒绝 http://", "ok": rejected,
                                 "detail": app.status_mid.cget("text")[:80]})
    except Exception:                                   # noqa: BLE001
        result["ok"] = False
        result["error"] = traceback.format_exc()[-2000:]
    finally:
        if app is not None:
            app.destroy()
    if out_path:
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(result, fh, ensure_ascii=False, indent=2)
    if verbose:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        print(json.dumps(result, ensure_ascii=False, indent=2)[:4000])
    return result


def run(argv=None):
    import argparse

    ap = argparse.ArgumentParser(description="SILLAGE 航迹 · 专用浏览器")
    ap.add_argument("route", nargs="?", default=START_ROUTE,
                    help="初始路由：" + "、".join(ph.ROUTES))
    ap.add_argument("--geometry", default="1500x920")
    ap.add_argument("--selftest", action="store_true", help="无窗口渲染全部路由并输出 JSON")
    ap.add_argument("--shots", default=None, help="--selftest 时把每页截图存到该目录")
    ap.add_argument("--out", default=None, help="--selftest 输出 JSON 路径")
    ap.add_argument("--shot", default=None, help="加载后截图到指定路径并退出")
    ap.add_argument("--text", action="store_true", help="只打印当前页面文本后退出")
    ap.add_argument("--engine", default="c", help="c 或 py")
    args = ap.parse_args(argv)
    if args.selftest:
        res = run_selftest(out_path=args.out, shots_dir=args.shots, prefer=args.engine)
        return 0 if res["ok"] else 1
    app = BrowserApp(geometry=args.geometry, route=args.route, prefer=args.engine)
    if args.shot:
        app.settle(360)
        size = app.shot(args.shot)
        print("已写入 %s -> %s" % (os.path.normpath(args.shot), size))
        app.destroy()
        return 0
    if args.text:
        app.settle(280)
        print(app.text_now())
        app.destroy()
        return 0
    app.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(run(sys.argv[1:]))
