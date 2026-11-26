#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""paizhao.py —— 给**原生桌面浏览器窗口**拍一张 PNG（后台/被遮挡也能拍到自身内容）。

为什么不直接用屏幕抓图：那只抓屏幕，窗口被别的窗口挡住就拍到别人的画面。
这里用 Win32 的 PrintWindow(..., PW_RENDERFULLCONTENT)：让窗口把自己画到
一块内存位图上。

    from paizhao import paizhao
    paizhao(root, "截图/首页.png")     # → (宽, 高, 字节数)

非 Windows 上退化成 ImageGrab（窗口必须在最前面）。
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import os

from PIL import Image

PW_RENDERFULLCONTENT = 0x00000002
DIB_RGB_COLORS = 0
GA_ROOT = 2

if os.name == "nt":
    _user32 = ctypes.windll.user32
    _gdi32 = ctypes.windll.gdi32
else:                                            # pragma: no cover
    _user32 = None
    _gdi32 = None


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD),
        ("biWidth", wt.LONG),
        ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD),
        ("biBitCount", wt.WORD),
        ("biCompression", wt.DWORD),
        ("biSizeImage", wt.DWORD),
        ("biXPelsPerMeter", wt.LONG),
        ("biYPelsPerMeter", wt.LONG),
        ("biClrUsed", wt.DWORD),
        ("biClrImportant", wt.DWORD),
    ]


class _BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", _BITMAPINFOHEADER), ("bmiColors", wt.DWORD * 3)]


def _hwnd_of(root):
    """从一个 tkinter 根窗口找到它真正的顶层 HWND。"""
    try:
        hwnd = _user32.GetAncestor(root.winfo_id(), GA_ROOT)
    except Exception:
        hwnd = 0
    return hwnd or root.winfo_id()


def _grab_window(hwnd):
    rect = wt.RECT()
    _user32.GetWindowRect(hwnd, ctypes.byref(rect))
    width = rect.right - rect.left
    height = rect.bottom - rect.top
    if width <= 0 or height <= 0:
        raise RuntimeError("窗口尺寸为零：%dx%d" % (width, height))

    win_dc = _user32.GetWindowDC(hwnd)
    mem_dc = _gdi32.CreateCompatibleDC(win_dc)
    bitmap = _gdi32.CreateCompatibleBitmap(win_dc, width, height)
    _gdi32.SelectObject(mem_dc, bitmap)
    try:
        _user32.PrintWindow(hwnd, mem_dc, PW_RENDERFULLCONTENT)
        info = _BITMAPINFO()
        info.bmiHeader.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
        info.bmiHeader.biWidth = width
        info.bmiHeader.biHeight = -height          # 负数 = 自上而下
        info.bmiHeader.biPlanes = 1
        info.bmiHeader.biBitCount = 32
        info.bmiHeader.biCompression = 0
        buf = ctypes.create_string_buffer(width * height * 4)
        _gdi32.GetDIBits(mem_dc, bitmap, 0, height, buf, ctypes.byref(info), DIB_RGB_COLORS)
        return Image.frombuffer("RGB", (width, height), buf, "raw", "BGRX", 0, 1)
    finally:
        _gdi32.DeleteObject(bitmap)
        _gdi32.DeleteDC(mem_dc)
        _user32.ReleaseDC(hwnd, win_dc)


def _grab_screen(root):
    """退路：屏幕矩形抓图（窗口必须在最前面）。"""
    from PIL import ImageGrab

    x = root.winfo_rootx()
    y = root.winfo_rooty()
    return ImageGrab.grab(bbox=(x, y, x + root.winfo_width(), y + root.winfo_height()))


def zhengli(root, times=3):
    """让 tkinter 把所有排队的重绘都画完，再拍照。"""
    for _ in range(times):
        try:
            root.update_idletasks()
            root.update()
        except Exception:
            break


def paizhao(root, path, settle=True):
    """给这个 tkinter 窗口拍一张 PNG，写到 path（目录自动建）。"""
    if settle:
        zhengli(root)
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    image = None
    if os.name == "nt":
        try:
            image = _grab_window(_hwnd_of(root))
        except Exception:
            image = None
    if image is None:
        image = _grab_screen(root)
    image.save(path)
    return image.size[0], image.size[1], os.path.getsize(path)
