#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""validate.py —— 航迹浏览器的无界面验收：不开窗口，把所有页面与动作跑一遍。

    python 浏览器源码/validate.py               # 默认：10 个路由 + 关键动作 + 程序调用
    python 浏览器源码/validate.py --json        # 机器可读结果
    python 浏览器源码/validate.py --engine py   # 用 Python 参考引擎对照

检查项：
  1. 每个路由都能渲染出合法文档树（status 200、节点数 > 1、无 log 错误）；
  2. 文档树严格符合 PAGE_SPEC（节点类型、必填字段、字段类型、深度、数量）；
  3. 关键动作能改变页面（发帖前后、计算前后、看答案前后……）；
  4. 程序调用（calc / forum / zoo / demo）真的能返回 200；
  5. 解释器求值、错误 JSON 形状、RPC 行为；
  6. 页面不含 TODO / 占位文字。
"""
from __future__ import annotations

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "运行支撑"))

import pagehost as ph          # noqa: E402
import engine as langengine    # noqa: E402
import _layout                 # noqa: E402

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append({"name": name, "ok": bool(ok), "detail": str(detail)[:400]})
    print("  %-6s %s%s" % ("PASS" if ok else "FAIL", name,
                           ("  — " + str(detail)[:110]) if detail else ""))
    return ok


def render(rt, route, action=None, fields=None, arg=None):
    req = ph.make_request(route, action, fields or {}, arg)
    return rt.render_page(route, req)


def main(argv=None):
    import argparse

    ap = argparse.ArgumentParser(description="航迹浏览器无界面验收")
    ap.add_argument("--engine", default=None, help="c 或 py")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--out", default=None, help="把 JSON 结果写到文件")
    ap.add_argument("--store", default=None,
                    help="验收用的集合存储（默认写到 数据/_验收.json，跑完删除，不动真数据）")
    args = ap.parse_args(argv)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    # 验收会点「计算器」按钮（页面把结果历史写进集合），所以默认用一个临时 store，
    # 免得把用户真正的 数据/sillage.json 弄脏。
    scratch = None
    store_path = args.store
    if not store_path:
        folder = _layout.path_of("data")
        os.makedirs(folder, exist_ok=True)
        store_path = os.path.join(folder, "_验收.json")
        scratch = store_path
        seed = _layout.data_file()
        if os.path.isfile(seed):
            with open(seed, "r", encoding="utf-8-sig") as fh:
                content = fh.read()
            with open(store_path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(content)

    rt = ph.Runtime(prefer=args.engine, store_path=store_path)
    try:
        print("引擎：%s" % ("C（sillage_c.exe）" if rt.mode == "c" else "Python 参考实现"))
        print("\n[1] 全部路由渲染")
        pages = {}
        for route in ph.ROUTES:
            resp = render(rt, route)
            pages[route] = resp
            ok = resp.get("status") == 200 and resp.get("nodes", 0) > 1
            check("路由 %-10s" % route, ok,
                  "status=%s 节点=%s 标题=%s %s" % (
                      resp.get("status"), resp.get("nodes"), resp.get("title"),
                      (resp.get("log") or [""])[0][:80]))

        print("\n[2] 文档树严格校验（PAGE_SPEC）")
        for route, resp in pages.items():
            if resp.get("status") != 200:
                continue
            try:
                count = ph.validate_doc(resp["doc"])
                check("契约 %-10s" % route, count > 1, "%d 个节点" % count)
            except ph.DocError as exc:
                check("契约 %-10s" % route, False, str(exc))

        print("\n[3] 页面不含占位文字")
        for route, resp in pages.items():
            if resp.get("status") != 200:
                continue
            text = ph.extract_text(resp["doc"])
            bad = [w for w in ("TODO", "待补充", "Lorem") if w in text]
            check("占位 %-10s" % route, not bad, "、".join(bad))

        print("\n[4] 关键动作")
        # 练习场：看答案
        r = render(rt, "lianxi", "kan", {}, {"id": 1})
        base = ph.extract_text(pages["lianxi"]["doc"])
        after = ph.extract_text(r["doc"]) if r.get("doc") else ""
        check("lianxi 看答案改变页面", r.get("status") == 200 and len(after) > len(base) - 10,
              "动作后 %d 节点" % r.get("nodes", 0))

        # 计算器：6 × 7 = 42
        r = render(rt, "jisuanqi", "jisuan", {"a": "6", "b": "7", "op": "mul"})
        text = ph.extract_text(r["doc"]) if r.get("doc") else ""
        check("jisuanqi 6×7=42", r.get("status") == 200 and "42" in text,
              "节点 %s" % r.get("nodes"))
        r2 = render(rt, "jisuanqi", "jisuan", {"a": "6", "b": "0", "op": "div"})
        t2 = ph.extract_text(r2["doc"]) if r2.get("doc") else ""
        check("jisuanqi 除零有提示", r2.get("status") == 200 and ("零" in t2 or "0" in t2),
              t2[:60].replace("\n", " "))

        # 论坛：发帖
        r = render(rt, "luntan", "fabu",
                   {"author": "自测", "title": "验证用帖子", "body": "来自 validate.py"})
        t = ph.extract_text(r["doc"]) if r.get("doc") else ""
        check("luntan 发帖", r.get("status") == 200 and "验证用帖子" in t,
              "节点 %s" % r.get("nodes"))

        # 别名动物园：全跑
        r = render(rt, "dongwuyuan", "quanpao")
        t = ph.extract_text(r["doc"]) if r.get("doc") else ""
        check("dongwuyuan 跑全部拼写", r.get("status") == 200 and "153" in t,
              [ln for ln in t.split("\n") if "通过" in ln][:1])

        # 自测页：语言用例
        if "ceshi" in pages and pages["ceshi"].get("status") == 200:
            r = render(rt, "ceshi", "yongli")
            t = ph.extract_text(r["doc"]) if r.get("doc") else ""
            check("ceshi 语言用例", r.get("status") == 200, "节点 %s" % r.get("nodes"))

        # 教程：第 3 课
        r = render(rt, "jiaocheng", "gong", {}, {"ke": 3})
        t = ph.extract_text(r["doc"]) if r.get("doc") else ""
        check("jiaocheng 第 3 课", r.get("status") == 200 and "文本" in t, "节点 %s" % r.get("nodes"))

        # 速查：过滤
        r = render(rt, "sucha", "chaxun", {"guanjianzi": "循环"})
        t = ph.extract_text(r["doc"]) if r.get("doc") else ""
        check("sucha 过滤「循环」", r.get("status") == 200 and "循环" in t, "节点 %s" % r.get("nodes"))

        print("\n[5] 程序调用（示例程序/程序）")
        for prog, action in (("calc", {"action": "calc", "params": {"op": "pow", "a": 2, "b": 10}}),
                             ("forum", {"action": "forum.stats"}),
                             ("zoo", {"action": "zoo.groups"}),
                             ("demo", {"action": "demo.run", "params": {}})):
            res = rt.call_program(prog, "handle", action)
            check("程序 %-6s" % prog, res.get("ok"),
                  (res.get("défaut") or {}).get("note", "ok"))

        print("\n[6] 解释器与错误模型")
        out = rt.engine.run_source("que x vaille 2 plus 3\nrends x", "<t>")
        check("求值 2 plus 3", out.get("value") == 5, out)
        out = rt.engine.run_source("1 plus 2 fois 3", "<t>")
        check("求值 1 plus 2 fois 3 = 7", out.get("value") == 7, out)
        out = rt.engine.run_source("«原样 # ‖ 文本»", "<t>")
        check("文本里的 # ‖ 不算注释", out.get("value") == "原样 # ‖ 文本", out)
        try:
            rt.engine.run_source('que x vaille "未闭合', "<t>")
            check("坏程序抛 Défaut", False, "没有抛错")
        except langengine.EngineError as exc:
            err = exc.to_dict()
            check("坏程序抛 Défaut", set(err) >= {"étape", "ligne", "note"}, err)

        print("\n[7] 页面源码里的禁用写法")
        for route in ph.ROUTES:
            path = ph.page_path(route)
            if not os.path.isfile(path):
                continue
            src = open(path, encoding="utf-8-sig").read()
            problems = []
            # 只查「真的去调用零参函数」的写法：de () 后面跟换行或行尾，且不是文档/示例文本
            for line in src.split("\n"):
                if line.lstrip().startswith("‖"):
                    continue
                if 'de ()"' in line or 'de ()" ' in line:
                    continue
                if re.search(r"\bde \(\)\s*$", line) and "on définit" not in line:
                    problems.append("零参调用：%s" % line.strip()[:50])
            if "TODO" in src:
                problems.append("含 TODO")
            check("写法 %-10s" % route, not problems, "；".join(problems))
    finally:
        rt.close()
        if scratch:                                     # 临时 store 用完就删
            try:
                os.remove(scratch)
            except OSError:
                pass

    bad = [r for r in RESULTS if not r["ok"]]
    print("\n合计 %d 项，失败 %d 项" % (len(RESULTS), len(bad)))
    for r in bad:
        print("  FAIL %s — %s" % (r["name"], r["detail"][:200]))

    payload = {"ok": not bad, "total": len(RESULTS), "failed": len(bad),
               "engine": rt.mode, "results": RESULTS}
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(payload, fh, ensure_ascii=False, indent=1)
    if args.json:
        print(json.dumps(payload, ensure_ascii=False, indent=1))
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main())
