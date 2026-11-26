#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""航迹浏览器.py —— SILLAGE 语言专用浏览器的唯一入口。

    python 浏览器源码/航迹浏览器.py              打开首页
    python 浏览器源码/航迹浏览器.py jiaocheng    直接进语法教程
    python 浏览器源码/航迹浏览器.py --selftest   无窗口渲染全部页面，输出 JSON
    python 浏览器源码/航迹浏览器.py --selftest --shots 截图  同时截图
    python 浏览器源码/航迹浏览器.py --shot 1.png 当前页截图后退出
    python 浏览器源码/航迹浏览器.py --text       打印当前页文本
    python 浏览器源码/航迹浏览器.py --engine py  强制用 Python 参考引擎

页面不是 HTML：`浏览器源码/页面/*.sil` 是用 SILLAGE 写出的程序，返回文档树，
渲染器把它画成真实控件。规范见 `浏览器源码/PAGE_SPEC.md`。
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "运行支撑"))

import core  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(core.run(sys.argv[1:]))
