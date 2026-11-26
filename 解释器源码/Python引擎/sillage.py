#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SILLAGE 3.0 — 一门独立的法语编程语言（Python 参考引擎，唯一实现）。

本文件是完整的 SILLAGE：自己的词法、语法、求值、值模型、错误模型、内置函数、
模块结果规则都在这里，**不引用任何其它语言的代码**。

理念：程序是一段法语陈述——先说"对什么做"，再说"做什么"；块用 `⟦ … ⟧`；
调用靠介词（`mesure de xs`、`découpe de t par ","`），不写括号。

值模型（刻意与同仓库另两门语言不同）
    Nombre  十进制定点，内部以 1e-9 为单位的整数；没有 int/float 之分
    Texte   UTF-8，按**字素簇**计长与索引，下标从 **1** 开始
    Suite   不可变序列，下标从 1 开始
    Fiche   保持插入顺序的表，键是 Texte
    OUI / NON / VIDE

错误：Défaut{étape, ligne, note}，étape ∈ lexique|syntaxe|forme|nom|calcul|hôte

模块结果：最后一条顶层 `rends` → 否则最后一条顶层表达式句 → 否则 VIDE；
          最后一条顶层句是 `silence` 时强制 VIDE。

CLI:
    python sillage.py test | run <f> [<json>] | ast <f> | tokens <f> | repl
"""
from __future__ import annotations

import json
import math
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

LANG = "sillage"
VERSION = "3.0.0"
FILE_EXT = ".sil"
ENTREE = "handle"

# 语言自己的递归护栏是 400 层；CPython 默认 1000 帧只够约 200 层语言调用，
# 所以先把 Python 的递归上限抬高，让语言的护栏先报出干净的 Défaut。
if sys.getrecursionlimit() < 20000:
    sys.setrecursionlimit(20000)

ECHELLE = 10 ** 9
NOTE_MAX = 1e15


# --------------------------------------------------------------------------- #
# 错误
# --------------------------------------------------------------------------- #
class Défaut(Exception):
    def __init__(self, étape, note, ligne=None):
        self.étape = étape
        self.note = note
        self.ligne = ligne
        Exception.__init__(self, note)

    def to_dict(self):
        return {"étape": self.étape, "ligne": self.ligne, "note": self.note}

    def __str__(self):
        return self.note


ERREUR = Défaut


class _Rends(Exception):
    def __init__(self, valeur):
        self.valeur = valeur


class _Brise(Exception):
    pass


class _Passe(Exception):
    pass


RIEN = object()
ABSENT = object()


# --------------------------------------------------------------------------- #
# 定点 Nombre
# --------------------------------------------------------------------------- #
def est_nombre(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _nombre_depuis_texte(t):
    s = t.strip().replace("\u00a0", "").replace(" ", "").replace(",", ".")
    if not s:
        return ABSENT
    signe = 1
    if s[0] in "+-":
        if s[0] == "-":
            signe = -1
        s = s[1:]
    if not s:
        return ABSENT
    if "." in s:
        entier, _, frac = s.partition(".")
    else:
        entier, frac = s, ""
    if entier and not entier.isdigit():
        return ABSENT
    if frac and not frac.isdigit():
        return ABSENT
    if not entier and not frac:
        return ABSENT
    if len(frac) > 9:
        if frac[9] >= "5":
            frac = frac[:9]
            base = int(entier or "0") * ECHELLE + int(frac) + 1
            return signe * base
        frac = frac[:9]
    frac = (frac + "0" * 9)[:9]
    return signe * (int(entier or "0") * ECHELLE + int(frac or "0"))


def nombre_texte(v):
    if v == 0:
        return "0"
    signe = "-" if v < 0 else ""
    v = abs(v)
    entier, frac = divmod(v, ECHELLE)
    if frac == 0:
        return signe + str(entier)
    return "%s%d.%s" % (signe, entier, ("%09d" % frac).rstrip("0"))


def _bornes(v):
    if abs(v) / ECHELLE > NOTE_MAX:
        raise Défaut("calcul", "nombre hors limites (|x| > 1e15)")
    return v


# --------------------------------------------------------------------------- #
# 词法
# --------------------------------------------------------------------------- #
T_MOT, T_NOMBRE, T_TEXTE, T_SYM, T_FIN, T_EOF = "mot", "nombre", "texte", "sym", "fin", "eof"

SYMBOLES = ["☑", "☒", "∅", "⟦", "⟧", "‹", "›", "⟬", "⟭", "⟼", "≠", "≢", "≤", "≥", "⩽", "⩾", ">=", "<=",
            "==", "≟", "∧", "∨", "&&", "||", "¬", "!", "×", "÷", "−", "⊕", "⊖", "⊗", "⊘",
            "⊞", "↑", "⪢", "⪡", "+", "-", "*", "/", "%", "^", "(", ")", "[", "]",
            "{", "}", ",", ":", "=", "<", ">"]
CHAÎNES = [("«", "»"), ('"', '"')]


def _lettre(ch):
    return ch.isalpha() or ch == "_"


def _alnum(ch):
    return ch.isalnum() or ch == "_" or ch == "'"


class Jeton:
    __slots__ = ("genre", "valeur", "ligne")

    def __init__(self, genre, valeur, ligne):
        self.genre = genre
        self.valeur = valeur
        self.ligne = ligne

    def __repr__(self):
        return "%s(%r)" % (self.genre, self.valeur)


def lexer(src):
    if src.startswith("\ufeff"):
        src = src[1:]
    src = src.replace("\r\n", "\n").replace("\r", "\n")
    jetons, i, n, ligne = [], 0, len(src), 1
    while i < n:
        ch = src[i]
        if ch == "\n":
            jetons.append(Jeton(T_FIN, None, ligne))
            ligne += 1
            i += 1
            continue
        if ch in " \t\f\v\u00a0":
            i += 1
            continue
        if ch == "#" or src.startswith("‖", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        fait = False
        for ouv, ferm in CHAÎNES:
            if src.startswith(ouv, i):
                i += len(ouv)
                buf = []
                while True:
                    if i >= n:
                        raise Défaut("lexique", "texte non terminé", ligne)
                    c = src[i]
                    if c == "\\" and ouv == '"':
                        if i + 1 >= n:
                            raise Défaut("lexique", "échappement incomplet", ligne)
                        e = src[i + 1]
                        table = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\",
                                 "«": "«", "»": "»"}
                        buf.append(table.get(e, e))
                        i += 2
                        continue
                    if src.startswith(ferm, i):
                        i += len(ferm)
                        break
                    if c == "\n":
                        raise Défaut("lexique", "texte non terminé", ligne)
                    buf.append(c)
                    i += 1
                jetons.append(Jeton(T_TEXTE, "".join(buf), ligne))
                fait = True
                break
        if fait:
            continue
        if ch.isdigit():
            j = i
            while j < n and (src[j].isdigit() or (src[j] == "." and j + 1 < n and src[j + 1].isdigit())):
                j += 1
            brut = src[i:j]
            val = _nombre_depuis_texte(brut)
            if val is ABSENT:
                raise Défaut("lexique", "nombre illisible: " + brut, ligne)
            jetons.append(Jeton(T_NOMBRE, _bornes(val), ligne))
            i = j
            continue
        if _lettre(ch):
            j = i
            while j < n and (_alnum(src[j]) or src[j] == "-"):
                j += 1
            mot = src[i:j]
            while j + 1 < n and src[j] == "." and _lettre(src[j + 1]):
                k = j + 1
                while k < n and (_alnum(src[k]) or src[k] == "-"):
                    k += 1
                mot += src[j:k]
                j = k
            jetons.append(Jeton(T_MOT, mot, ligne))
            i = j
            continue
        trouve = None
        for s in SYMBOLES:
            if src.startswith(s, i):
                trouve = s
                break
        if trouve is None:
            raise Défaut("lexique", "caractère inattendu: %r" % ch, ligne)
        jetons.append(Jeton(T_SYM, trouve, ligne))
        i += len(trouve)
    jetons.append(Jeton(T_EOF, None, ligne))
    return jetons


# --------------------------------------------------------------------------- #
# 语法
# --------------------------------------------------------------------------- #
class Parseur:
    def __init__(self, jetons):
        self.t, self.i = jetons, 0

    def pic(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else self.t[-1]

    def val(self, k=0):
        j = self.pic(k)
        return j.valeur if j.genre in (T_MOT, T_SYM) else None

    def genre(self, k=0):
        return self.pic(k).genre

    def est(self, valeurs, k=0):
        v = self.val(k)
        if v is None:
            return False
        if isinstance(valeurs, str):
            return v == valeurs
        return v in valeurs

    def avale(self):
        j = self.t[self.i]
        if j.genre != T_EOF:
            self.i += 1
        return j

    def attendre(self, valeurs, quoi):
        if not self.est(valeurs):
            j = self.pic()
            montre = j.valeur if j.valeur is not None else ("fin de ligne" if j.genre == T_FIN else "fin de fichier")
            raise Défaut("syntaxe", "attendu %s, trouvé %r" % (quoi, montre), j.ligne)
        return self.avale()

    def attendre_nom(self, quoi):
        j = self.pic()
        if j.genre != T_MOT:
            montre = j.valeur if j.valeur is not None else "fin d'instruction"
            raise Défaut("syntaxe", "attendu %s, trouvé %r" % (quoi, montre), j.ligne)
        self.avale()
        return j.valeur

    # -- 程序与块 -- #
    def programme(self):
        stmts = self.sequence(False)
        j = self.pic()
        if j.genre != T_EOF:
            raise Défaut("syntaxe", "entrée en trop: %r" % (j.valeur,), j.ligne)
        return stmts

    def sequence(self, dans_bloc):
        stmts = []
        while True:
            while self.genre() == T_FIN:
                self.avale()
            j = self.pic()
            if j.genre == T_EOF:
                break
            if dans_bloc and self.est("⟧"):
                break
            stmts.append(self.instruction())
        return stmts

    def bloc(self):
        self.attendre("⟦", "⟦ (ouverture de bloc)")
        corps = self.sequence(True)
        self.attendre("⟧", "⟧ (fermeture de bloc)")
        return corps

    def sauter_fins(self):
        while self.genre() == T_FIN:
            self.avale()

    # -- 语句 -- #
    def instruction(self):
        v = self.val()
        ligne = self.pic().ligne

        if v in ("que", "soit"):
            self.avale()
            nom = self.attendre_nom("un nom")
            if self.est("vaille"):
                self.avale()
                return ("lier", nom, self.expression(), ligne)
            if self.est("devienne") or self.est("remue"):
                self.avale()
                return ("relier", nom, self.expression(), ligne)
            raise Défaut("syntaxe", "attendu « vaille » ou « devienne »", ligne)

        if v == "donne":
            self.avale()
            val = self.expression()
            self.attendre("à", "« à »")
            nom = self.attendre_nom("un nom")
            return ("lier", nom, val, ligne)

        if v in ("affiche", "dis", "trace"):
            self.avale()
            return ("afficher", self.expression(), ligne)

        if v == "rends":
            self.avale()
            return ("rends", self.expression(), ligne)

        if v == "silence":
            self.avale()
            return ("silence", ligne)

        if v in ("si", "quand", "lorsque"):
            self.avale()
            return self.chaine_si(ligne)

        if v in ("tantque", "boucle"):
            self.avale()
            return ("tantque", self.expression(), self.bloc(), ligne)
        if v == "tant":
            self.avale()
            self.attendre("que", "« que »")
            return ("tantque", self.expression(), self.bloc(), ligne)

        if v == "pour":
            self.avale()
            self.attendre("chaque", "« chaque »")
            nom = self.attendre_nom("un nom")
            self.attendre("dans", "« dans »")
            return ("pourchaque", nom, self.expression(), self.bloc(), ligne)

        if v == "on":
            self.avale()
            self.attendre("définit", "« définit »")
            nom = self.attendre_nom("un nom de geste")
            self.attendre("de", "« de »")
            params = self.params()
            self.attendre("comme", "« comme »")
            return ("definir", nom, params, self.bloc(), ligne)

        if v in ("arrête", "casse"):
            self.avale()
            if not (self.genre() == T_FIN or self.est("⟧") or self.genre() == T_EOF):
                raise Défaut("calcul", "arrête hors d'une boucle", ligne)
            return ("brise", ligne)
        if v in ("continue", "passe"):
            self.avale()
            if not (self.genre() == T_FIN or self.est("⟧") or self.genre() == T_EOF):
                raise Défaut("calcul", "passe hors d'une boucle", ligne)
            return ("passe", ligne)

        return ("expr", self.expression(), ligne)

    def params(self):
        if self.est("("):
            self.avale()
            noms = []
            if not self.est(")"):
                while True:
                    noms.append(self.attendre_nom("un paramètre"))
                    if self.est(","):
                        self.avale()
                        continue
                    break
            self.attendre(")", ")")
            return noms
        noms = [self.attendre_nom("un paramètre")]
        while self.est("et"):
            self.avale()
            noms.append(self.attendre_nom("un paramètre"))
        return noms

    def chaine_si(self, ligne):
        cond = self.expression()
        self.attendre("alors", "« alors »")
        branches = [[cond, self.bloc()]]
        sinon = None
        while True:
            sauve = self.i
            self.sauter_fins()
            if self.est("sinonsi") or self.est("oualors"):
                self.avale()
                c2 = self.expression()
                self.attendre("alors", "« alors »")
                branches.append([c2, self.bloc()])
                continue
            if self.est("sinon") or self.est("autrement"):
                self.avale()
                sinon = self.bloc()
                break
            self.i = sauve
            break
        return ("si", branches, sinon, ligne)

    # -- 表达式 -- #
    def expression(self):
        return self.ou()

    def ou(self):
        gauche = self.et()
        while self.est("ou") or self.est("∨") or self.est("||"):
            self.avale()
            gauche = ("ou", gauche, self.et())
        return gauche

    def et(self):
        gauche = self.comparaison()
        while self.est("et") or self.est("∧") or self.est("&&"):
            self.avale()
            gauche = ("et", gauche, self.comparaison())
        return gauche

    SEQS_CMP = [
        (("plus", "grand", "que"), ">"),
        (("plus", "petit", "que"), "<"),
        (("au", "moins"), ">="),
        (("au", "plus"), "<="),
        (("égal", "à"), "=="),
        (("différent", "de"), "!="),
    ]
    SYMS_CMP = {">": ">", "<": "<", ">=": ">=", "<=": "<=", "≥": ">=", "≤": "<=",
                "⩾": ">=", "⩽": "<=", "⪢": ">", "⪡": "<",
                "=": "==", "==": "==", "≠": "!=", "≢": "!=", "≟": "=="}

    def comparaison(self):
        gauche = self.somme()
        while True:
            canon = None
            for mots, c in self.SEQS_CMP:
                if all(self.est(m, k) for k, m in enumerate(mots)):
                    for _ in mots:
                        self.avale()
                    canon = c
                    break
            if canon is None:
                v = self.val()
                if v in self.SYMS_CMP:
                    self.avale()
                    canon = self.SYMS_CMP[v]
                elif v in ("égal",):
                    self.avale()
                    canon = "=="
                elif v in ("différent",):
                    self.avale()
                    canon = "!="
            if canon is None:
                break
            gauche = ("cmp", canon, gauche, self.somme())
        return gauche

    def argument(self):
        """调用实参：只解析到「加减」层，不吞掉比较、逻辑 et/ou。"""
        return self.somme()

    def somme(self):
        gauche = self.produit()
        while True:
            v = self.val()
            # 「plus petit que」「plus grand que」是比较符，不是加法
            if v in ("plus", "+") and (self.est("petit", 1) or self.est("grand", 1)):
                break
            if v in ("plus", "+", "⊕", "moins", "−", "-", "⊖"):
                self.avale()
                canon = "+" if v in ("plus", "+", "⊕") else "-"
                gauche = ("bin", canon, gauche, self.produit())
                continue
            break
        return gauche

    def produit(self):
        gauche = self.puissance()
        CANON = {"fois": "*", "×": "*", "*": "*", "⊗": "*",
                 "sur": "/", "÷": "/", "/": "/", "⊘": "/",
                 "modulo": "%", "%": "%", "⊞": "%"}
        while True:
            v = self.val()
            if v in CANON:
                self.avale()
                gauche = ("bin", CANON[v], gauche, self.puissance())
                continue
            break
        return gauche

    def puissance(self):
        gauche = self.unaire()
        if self.est("puissance") or self.est("^") or self.est("↑"):
            self.avale()
            return ("bin", "**", gauche, self.puissance())
        return gauche

    def unaire(self):
        v = self.val()
        if v in ("non", "¬", "!"):
            self.avale()
            return ("un", "non", self.unaire())
        if v in ("moins", "−", "-"):
            self.avale()
            return ("un", "-", self.unaire())
        return self.primaire()

    def primaire(self):
        j = self.pic()
        genre, val, ligne = j.genre, j.valeur, j.ligne
        if genre == T_NOMBRE:
            self.avale()
            return ("nombre", val)
        if genre == T_TEXTE:
            self.avale()
            return ("texte", val)
        if genre == T_MOT:
            if val in ("OUI", "oui", "vrai", "☑"):
                self.avale()
                return ("oui",)
            if val in ("NON", "faux", "☒"):
                self.avale()
                return ("non",)
            if val in ("VIDE", "vide", "∅", "rien"):
                self.avale()
                return ("vide",)
            if val == "élément":
                self.avale()
                idx = self.simple()
                self.attendre("de", "« de »")
                return ("element", idx, self.primaire())
            if val == "champ":
                self.avale()
                cle = self.simple()
                self.attendre("de", "« de »")
                return ("champ-de", cle, self.primaire())
            self.avale()
            return self.application(val)
        if genre == T_SYM:
            if val == "‹":
                self.avale()
                items = []
                self.sauter_fins()
                if not self.est("›"):
                    while True:
                        self.sauter_fins()
                        items.append(self.expression())
                        self.sauter_fins()
                        if self.est(","):
                            self.avale()
                            continue
                        break
                self.sauter_fins()
                self.attendre("›", "›")
                return ("suite", items)
            if val == "⟬":
                self.avale()
                paires = []
                self.sauter_fins()
                if not self.est("⟭"):
                    while True:
                        self.sauter_fins()
                        kt = self.pic()
                        if kt.genre in (T_MOT, T_TEXTE):
                            cle = kt.valeur
                            self.avale()
                        else:
                            raise Défaut("syntaxe", "clé de fiche attendue", kt.ligne)
                        self.attendre("⟼", "⟼")
                        paires.append((cle, self.expression()))
                        self.sauter_fins()
                        if self.est(","):
                            self.avale()
                            continue
                        break
                self.sauter_fins()
                self.attendre("⟭", "⟭")
                return ("fiche", paires)
            if val == "(":
                self.avale()
                e = self.expression()
                self.attendre(")", ")")
                return e
            if val == "☑":
                self.avale()
                return ("oui",)
            if val == "☒":
                self.avale()
                return ("non",)
            if val == "∅":
                self.avale()
                return ("vide",)
        montre = val if val is not None else ("fin de ligne" if genre == T_FIN else "fin de fichier")
        raise Défaut("syntaxe", "expression attendue, trouvé %r" % (montre,), ligne)

    def simple(self):
        """下标 / 字段键：只吃一个基本项，**不做**介词调用（避免吞掉后面的 de）。"""
        j = self.pic()
        genre, val = j.genre, j.valeur
        if genre == T_NOMBRE:
            self.avale()
            return ("nombre", val)
        if genre == T_TEXTE:
            self.avale()
            return ("texte", val)
        if genre == T_MOT:
            self.avale()
            if val in ("OUI", "oui", "vrai", "☑"):
                return ("oui",)
            if val in ("NON", "faux", "☒"):
                return ("non",)
            if val in ("VIDE", "vide", "∅", "rien"):
                return ("vide",)
            return ("nom", val)
        if genre == T_SYM and val == "(":
            self.avale()
            e = self.expression()
            self.attendre(")", ")")
            return e
        if genre == T_SYM and val == "‹":
            return self.primaire()
        if genre == T_SYM and val == "⟬":
            return self.primaire()
        montre = val if val is not None else ("fin de ligne" if genre == T_FIN else "fin de fichier")
        raise Défaut("syntaxe", "expression attendue, trouvé %r" % (montre,), j.ligne)

    def application(self, nom):
        if "." in nom:
            args = []
            while self.debut_expression():
                args.append(self.argument())
            return ("appel", nom, args)
        if self.est("de"):
            self.avale()
            args = [self.argument()]
            while True:
                if self.est("et"):
                    self.avale()
                    args.append(self.argument())
                    continue
                if self.est("par") or self.est("avec") or self.est("dans"):
                    self.avale()
                    args.append(self.argument())
                    continue
                if self.est("depuis"):
                    self.avale()
                    debut = self.argument()
                    self.attendre("jusqu'à", "« jusqu'à »")
                    fin = self.argument()
                    args.extend([debut, fin])
                    continue
                break
            return ("appel", nom, args)
        if self.est("("):
            self.avale()
            args = []
            if not self.est(")"):
                while True:
                    args.append(self.expression())
                    if self.est(","):
                        self.avale()
                        continue
                    break
            self.attendre(")", ")")
            return ("appel", nom, args)
        return ("nom", nom)

    def debut_expression(self, k=0):
        j = self.pic(k)
        if j.genre in (T_NOMBRE, T_TEXTE, T_MOT):
            return True
        return j.genre == T_SYM and j.valeur in ("‹", "⟬", "(", "¬", "−", "-")


def analyser(src):
    return Parseur(lexer(src)).programme()


# --------------------------------------------------------------------------- #
# 运行期
# --------------------------------------------------------------------------- #
class Portée:
    __slots__ = ("noms", "parent")

    def __init__(self, parent=None):
        self.noms = {}
        self.parent = parent

    def définir(self, nom, valeur):
        self.noms[nom] = valeur

    def _trouver(self, nom):
        p = self
        while p is not None:
            if nom in p.noms:
                return p
            p = p.parent
        return None

    def lire(self, nom):
        p = self._trouver(nom)
        return ABSENT if p is None else p.noms[nom]

    def affecter(self, nom, valeur):
        p = self._trouver(nom)
        if p is None:
            return False
        p.noms[nom] = valeur
        return True


class Geste:
    __slots__ = ("nom", "params", "corps", "portée")

    def __init__(self, nom, params, corps, portée):
        self.nom = nom
        self.params = params
        self.corps = corps
        self.portée = portée


class Natif:
    __slots__ = ("nom", "appel")

    def __init__(self, nom, appel):
        self.nom = nom
        self.appel = appel


def _graphemes(t):
    out = []
    for ch in t:
        if out:
            o = ord(ch)
            if (0x0300 <= o <= 0x036F or 0x1AB0 <= o <= 0x1AFF or 0x20D0 <= o <= 0x20FF
                    or 0xFE20 <= o <= 0xFE2F or o in (0xFE0F, 0x200D) or 0x1F3FB <= o <= 0x1F3FF):
                out[-1] += ch
                continue
        out.append(ch)
    return out


def texte_de(v):
    if v is None or v is RIEN:
        return ""
    if v is True:
        return "oui"
    if v is False:
        return "non"
    if est_nombre(v):
        return nombre_texte(v)
    if isinstance(v, str):
        return v
    if isinstance(v, (list, dict)):
        return json_compact(v)
    return "<geste>"


def json_compact(v):
    buf = []

    def écrire(x):
        if x is None or x is RIEN:
            buf.append("null")
        elif x is True:
            buf.append("true")
        elif x is False:
            buf.append("false")
        elif isinstance(x, str):
            buf.append(json.dumps(x, ensure_ascii=False))
        elif est_nombre(x):
            buf.append(nombre_texte(x))
        elif isinstance(x, list):
            buf.append("[")
            for k, e in enumerate(x):
                if k:
                    buf.append(",")
                écrire(e)
            buf.append("]")
        elif isinstance(x, dict):
            buf.append("{")
            premier = True
            for k, e in x.items():
                if not premier:
                    buf.append(",")
                premier = False
                buf.append(json.dumps(str(k), ensure_ascii=False))
                buf.append(":")
                écrire(e)
            buf.append("}")
        else:
            buf.append("null")

    écrire(v)
    return "".join(buf)


def égal(a, b):
    if a is None or a is RIEN:
        return b is None or b is RIEN
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    if est_nombre(a) and est_nombre(b):
        return a == b
    if isinstance(a, str) and isinstance(b, str):
        return a == b
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(égal(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return len(a) == len(b) and all(k in b and égal(v, b[k]) for k, v in a.items())
    return a is b


def vérité(v):
    if v is None or v is RIEN or v is False:
        return False
    if v is True:
        return True
    if est_nombre(v):
        return v != 0
    if isinstance(v, (str, list, dict)):
        return len(v) > 0
    return True


# --------------------------------------------------------------------------- #
# 内置（介词式）
# --------------------------------------------------------------------------- #
def _entier(v):
    if not est_nombre(v):
        raise Défaut("forme", "indice entier attendu")
    if v % ECHELLE != 0:
        raise Défaut("forme", "indice entier attendu")
    return v // ECHELLE


def _b_mesure(x):
    if isinstance(x, str):
        return len(_graphemes(x)) * ECHELLE
    if isinstance(x, (list, dict)):
        return len(x) * ECHELLE
    raise Défaut("forme", "mesure attend un texte, une suite ou une fiche")


def _b_texte(x):
    return texte_de(x)


def _b_nombre(x):
    if est_nombre(x):
        return x
    if isinstance(x, str):
        v = _nombre_depuis_texte(x)
        if v is ABSENT:
            raise Défaut("calcul", "en-nombre: « %s » n'est pas un nombre" % x)
        return _bornes(v)
    raise Défaut("calcul", "en-nombre attend un texte")


def _b_égalise(x):
    return x


def _b_abaissé(t):
    if not isinstance(t, str):
        raise Défaut("forme", "abaissé attend un texte")
    return t.lower()


def _b_haussé(t):
    if not isinstance(t, str):
        raise Défaut("forme", "haussé attend un texte")
    return t.upper()


def _b_découpe(t, sep):
    if not isinstance(t, str):
        raise Défaut("forme", "découpe attend un texte")
    if sep is None:
        return _graphemes(t)
    if not isinstance(sep, str):
        raise Défaut("forme", "découpe: séparateur texte attendu")
    if sep == "":
        return _graphemes(t)
    return t.split(sep)


def _b_recoud(suite, sep):
    if not isinstance(suite, list):
        raise Défaut("forme", "recoud attend une suite")
    if not isinstance(sep, str):
        raise Défaut("forme", "recoud: séparateur texte attendu")
    return sep.join(texte_de(x) for x in suite)


def _b_renferme(x, y):
    if isinstance(x, str):
        if not isinstance(y, str):
            raise Défaut("forme", "renferme: sous-texte attendu")
        return y in x
    if isinstance(x, list):
        return any(égal(e, y) for e in x)
    if isinstance(x, dict):
        return isinstance(y, str) and y in x
    raise Défaut("forme", "renferme: valeur non contenante")


def _b_clefs(f):
    if not isinstance(f, dict):
        raise Défaut("forme", "clefs attend une fiche")
    return list(f.keys())


def _b_teneurs(f):
    if not isinstance(f, dict):
        raise Défaut("forme", "teneurs attend une fiche")
    return list(f.values())


def _b_ajoute_fin(suite, x):
    if not isinstance(suite, list):
        raise Défaut("forme", "ajoute-fin attend une suite")
    return suite + [x]


def _b_bribe(x, debut, fin):
    if not isinstance(x, (str, list)):
        raise Défaut("forme", "bribe attend un texte ou une suite")
    a, b = _entier(debut), _entier(fin)
    if isinstance(x, str):
        g = _graphemes(x)
        n = len(g)
    else:
        g, n = x, len(x)
    a = max(1, min(n, a))
    b = max(0, min(n, b))
    if a > b:
        return "" if isinstance(x, str) else []
    return "".join(g[a - 1:b]) if isinstance(x, str) else g[a - 1:b]


def _b_ordonne(suite):
    if not isinstance(suite, list):
        raise Défaut("forme", "ordonne attend une suite")

    def cle(x):
        if isinstance(x, bool):
            return (0, 1 if x else 0, "")
        if est_nombre(x):
            return (0, x, "")
        if isinstance(x, str):
            return (1, 0, x)
        return (2, 0, texte_de(x))

    return sorted(suite, key=cle)


def _b_total(suite):
    if not isinstance(suite, list):
        raise Défaut("forme", "total attend une suite")
    s = 0
    for x in suite:
        if not est_nombre(x):
            raise Défaut("forme", "total: suite de nombres attendue")
        s += x
    return _bornes(s)


def _b_tordu(f, cle, val):
    if not isinstance(f, dict):
        raise Défaut("forme", "tordu attend une fiche")
    if not isinstance(cle, str):
        raise Défaut("forme", "tordu: clé texte attendue")
    out = dict(f)
    out[cle] = val
    return out


def _b_absolu(x):
    if not est_nombre(x):
        raise Défaut("forme", "absolu attend un nombre")
    return -x if x < 0 else x


def _b_fige(x):
    return json_compact(x)


NATIFS = {
    "mesure": _b_mesure, "en-texte": _b_texte, "en-nombre": _b_nombre, "égalise": _b_égalise,
    "abaissé": _b_abaissé, "haussé": _b_haussé, "découpe": _b_découpe, "recoud": _b_recoud,
    "renferme": _b_renferme, "clefs": _b_clefs, "teneurs": _b_teneurs,
    "ajoute-fin": _b_ajoute_fin, "bribe": _b_bribe, "ordonne": _b_ordonne,
    "total": _b_total, "tordu": _b_tordu, "fige": _b_fige, "absolu": _b_absolu,
}

HOSTE = {
    "carnet.lister": "store.list", "carnet.compter": "store.count",
    "carnet.lire": "store.get", "carnet.écrire": "store.put",
    "carnet.fusionner": "store.patch", "carnet.retirer": "store.remove",
    "carnet.vider": "store.clear", "horloge.maintenant": "clock.now",
    "voix.dire": "lib.say", "voix.signaler": "lib.emit",
    "atelier.évaluer": "lang.eval", "atelier.lancer": "lang.run",
    "atelier.noms": "lang.names",
}


# --------------------------------------------------------------------------- #
# 解释器
# --------------------------------------------------------------------------- #
class Interpreter:
    def __init__(self, host=None, sortie=None):
        self.host = host or {}
        self.sortie = sortie or (lambda s: print(s, flush=True))
        self.global_env = Portée()
        self.profondeur = 0
        self.prints = []

    def eval(self, source, nom="<module>"):
        if not isinstance(source, str):
            raise Défaut("forme", "eval attend un texte")
        stmts = analyser(source)
        self.global_env = Portée()
        self.prints = []
        dernier = None
        résultat = RIEN
        for idx, st in enumerate(stmts):
            try:
                v = self.exécuter(st, self.global_env)
            except _Rends as r:
                return None if r.valeur is RIEN else r.valeur
            except _Brise:
                raise Défaut("calcul", "arrête hors d'une boucle", st[-1] if st else None)
            except _Passe:
                raise Défaut("calcul", "passe hors d'une boucle", st[-1] if st else None)
            except RecursionError:
                raise Défaut("calcul", "pile trop profonde")
            if v is not RIEN:
                résultat = v
            dernier = st
        if dernier is not None and dernier[0] == "silence":
            return None
        return None if résultat is RIEN else résultat

    def call(self, nom, args):
        f = self.global_env.lire(nom)
        if f is ABSENT:
            raise Défaut("nom", "geste inconnu: " + str(nom))
        entrees = [_entrer(a) for a in (args or [])]
        return _sortir(self.invoquer(f, entrees))

    def globals(self):
        return dict(self.global_env.noms)

    def exécuter(self, st, portée):
        t = st[0]
        if t == "expr":
            return self.évaluer(st[1], portée)
        if t == "lier":
            portée.définir(st[1], self.évaluer(st[2], portée))
            return RIEN
        if t == "relier":
            val = self.évaluer(st[2], portée)
            if not portée.affecter(st[1], val):
                raise Défaut("nom", "reliaison d'un nom inconnu: " + st[1], st[3])
            return RIEN
        if t == "afficher":
            s = texte_de(self.évaluer(st[1], portée))
            self.prints.append(s)
            self.sortie(s)
            return RIEN
        if t == "rends":
            raise _Rends(self.évaluer(st[1], portée))
        if t == "silence":
            return RIEN
        if t == "si":
            for cond, corps in st[1]:
                if vérité(self.évaluer(cond, portée)):
                    return self.exécuter_bloc(corps, Portée(portée))
            if st[2] is not None:
                return self.exécuter_bloc(st[2], Portée(portée))
            return RIEN
        if t == "tantque":
            tours = 0
            while vérité(self.évaluer(st[1], portée)):
                tours += 1
                if tours > 2000000:
                    raise Défaut("calcul", "boucle trop longue", st[3])
                try:
                    self.exécuter_bloc(st[2], Portée(portée))
                except _Brise:
                    break
                except _Passe:
                    continue
            return RIEN
        if t == "pourchaque":
            seq = self.évaluer(st[2], portée)
            if isinstance(seq, str):
                seq = _graphemes(seq)
            if not isinstance(seq, list):
                raise Défaut("forme", "pour chaque attend une suite", st[4])
            for item in seq:
                p = Portée(portée)
                p.définir(st[1], item)
                try:
                    self.exécuter_bloc(st[3], p)
                except _Brise:
                    break
                except _Passe:
                    continue
            return RIEN
        if t == "definir":
            portée.définir(st[1], Geste(st[1], st[2], st[3], portée))
            return RIEN
        if t == "brise":
            raise _Brise()
        if t == "passe":
            raise _Passe()
        raise Défaut("calcul", "instruction inconnue: " + str(t))

    def exécuter_bloc(self, stmts, portée):
        for st in stmts:
            self.exécuter(st, portée)

    def évaluer(self, e, portée):
        t = e[0]
        if t == "nombre":
            return e[1]
        if t == "texte":
            return e[1]
        if t == "oui":
            return True
        if t == "non":
            return False
        if t == "vide":
            return None
        if t == "nom":
            nom = e[1]
            v = portée.lire(nom)
            if v is not ABSENT:
                return v
            if nom in NATIFS:
                return Natif(nom, lambda args, _n=nom: NATIFS[_n](*args))
            if nom in self.host:
                return Natif(nom, lambda args, _n=nom: self._hôte(_n, args))
            raise Défaut("nom", "nom inconnu: " + nom)
        if t == "suite":
            return [self.évaluer(x, portée) for x in e[1]]
        if t == "fiche":
            out = {}
            for k, ex in e[1]:
                out[k] = self.évaluer(ex, portée)
            return out
        if t == "element":
            coll = self.évaluer(e[2], portée)
            return self._indexer(coll, self.évaluer(e[1], portée))
        if t == "champ-de":
            f = self.évaluer(e[2], portée)
            cle = self.évaluer(e[1], portée)
            if not isinstance(f, dict):
                raise Défaut("forme", "champ de: fiche attendue")
            if not isinstance(cle, str):
                raise Défaut("forme", "champ de: clé texte attendue")
            if cle not in f:
                raise Défaut("forme", "champ absent: " + cle)
            return f[cle]
        if t == "un":
            v = self.évaluer(e[2], portée)
            if e[1] == "non":
                return not vérité(v)
            if not est_nombre(v):
                raise Défaut("forme", "moins d'un non-nombre")
            return -v
        if t == "et":
            if not vérité(self.évaluer(e[1], portée)):
                return False
            return vérité(self.évaluer(e[2], portée))
        if t == "ou":
            if vérité(self.évaluer(e[1], portée)):
                return True
            return vérité(self.évaluer(e[2], portée))
        if t == "bin":
            return self._binaire(e[1], self.évaluer(e[2], portée), self.évaluer(e[3], portée))
        if t == "cmp":
            return self._comparer(e[1], self.évaluer(e[2], portée), self.évaluer(e[3], portée))
        if t == "appel":
            nom = e[1]
            args = [self.évaluer(a, portée) for a in e[2]]
            f = portée.lire(nom)
            if f is ABSENT:
                if nom in NATIFS:
                    return self.invoquer(Natif(nom, lambda a, _n=nom: NATIFS[_n](*a)), args)
                if nom in self.host:
                    return self.invoquer(Natif(nom, lambda a, _n=nom: self._hôte(_n, a)), args)
                raise Défaut("nom", "nom inconnu: " + nom)
            return self.invoquer(f, args)
        raise Défaut("calcul", "expression inconnue: " + str(t))

    def _hôte(self, nom, args):
        fn = self.host.get(nom)
        if not callable(fn):
            raise Défaut("hôte", "host-not-found: " + nom)
        try:
            return _entrer(fn(*[_sortir(a) for a in args]))
        except Défaut:
            raise
        except Exception as exc:
            raise Défaut("hôte", "%s: %s" % (nom, exc))

    def _indexer(self, coll, idx):
        i = _entier(idx)
        if isinstance(coll, list):
            if i < 1 or i > len(coll):
                raise Défaut("calcul", "indice hors limites")
            return coll[i - 1]
        if isinstance(coll, str):
            g = _graphemes(coll)
            if i < 1 or i > len(g):
                raise Défaut("calcul", "indice hors limites")
            return g[i - 1]
        raise Défaut("forme", "indexation impossible")

    def _binaire(self, op, a, b):
        if op == "+":
            if est_nombre(a) and est_nombre(b):
                return _bornes(a + b)
            if isinstance(a, str) and isinstance(b, str):
                return a + b
            if isinstance(a, list) and isinstance(b, list):
                return a + b
            raise Défaut("forme", "addition impossible sur ces valeurs")
        if op == "-":
            if not (est_nombre(a) and est_nombre(b)):
                raise Défaut("forme", "soustraction de non-nombres")
            return _bornes(a - b)
        if op == "*":
            if not (est_nombre(a) and est_nombre(b)):
                raise Défaut("forme", "multiplication de non-nombres")
            return _bornes((a * b) // ECHELLE)
        if op == "/":
            if not (est_nombre(a) and est_nombre(b)):
                raise Défaut("forme", "division de non-nombres")
            if b == 0:
                raise Défaut("calcul", "division par zéro")
            return _bornes((a * ECHELLE) // b)
        if op == "%":
            if not (est_nombre(a) and est_nombre(b)):
                raise Défaut("forme", "modulo de non-nombres")
            if b == 0:
                raise Défaut("calcul", "division par zéro")
            q = int(a / b)
            return _bornes(a - b * q)
        if op == "**":
            if not (est_nombre(a) and est_nombre(b)):
                raise Défaut("forme", "puissance de non-nombres")
            if b % ECHELLE != 0:
                raise Défaut("forme", "exposant entier attendu")
            e = b // ECHELLE
            if e > 1000 or e < -1000:
                raise Défaut("calcul", "débordement")
            try:
                r = (a / ECHELLE) ** e
            except (OverflowError, ZeroDivisionError):
                raise Défaut("calcul", "débordement")
            if not math.isfinite(r):
                raise Défaut("calcul", "débordement")
            return _bornes(int(round(r * ECHELLE)))
        raise Défaut("calcul", "opérateur inconnu: " + op)

    def _comparer(self, op, a, b):
        if op == "==":
            return égal(a, b)
        if op == "!=":
            return not égal(a, b)
        if est_nombre(a) and est_nombre(b):
            x, y = a, b
        elif isinstance(a, str) and isinstance(b, str):
            x, y = a, b
        else:
            raise Défaut("forme", "comparaison entre types incompatibles")
        if op == ">":
            return x > y
        if op == "<":
            return x < y
        if op == ">=":
            return x >= y
        if op == "<=":
            return x <= y
        raise Défaut("calcul", "comparateur inconnu: " + op)

    def invoquer(self, f, args):
        if isinstance(f, Natif):
            try:
                return f.appel(args)
            except Défaut:
                raise
            except Exception as exc:
                raise Défaut("hôte", "%s: %s" % (f.nom, exc))
        if isinstance(f, Geste):
            if len(args) != len(f.params):
                raise Défaut("forme", "%s attend %d argument(s), reçu %d"
                            % (f.nom, len(f.params), len(args)))
            self.profondeur += 1
            if self.profondeur > 400:
                self.profondeur -= 1
                raise Défaut("calcul", "pile trop profonde")
            p = Portée(f.portée)
            for nom, val in zip(f.params, args):
                p.définir(nom, val)
            try:
                self.exécuter_bloc(f.corps, p)
                return None
            except _Rends as r:
                return r.valeur
            except RecursionError:
                raise Défaut("calcul", "pile trop profonde")
            finally:
                self.profondeur -= 1
        raise Défaut("forme", "valeur non appelable")


def _entrer(v):
    """外部 JSON -> 语言值：数字变定点 Nombre。"""
    if isinstance(v, bool) or v is None:
        return v
    if isinstance(v, int):
        return v * ECHELLE
    if isinstance(v, float):
        return _bornes(int(round(v * ECHELLE)))
    if isinstance(v, str):
        return v
    if isinstance(v, list):
        return [_entrer(x) for x in v]
    if isinstance(v, dict):
        return {str(k): _entrer(x) for k, x in v.items()}
    return v


def _sortir(v):
    """语言值 -> 外部 JSON。"""
    if v is None or v is RIEN:
        return None
    if isinstance(v, bool) or isinstance(v, str):
        return v
    if est_nombre(v):
        t = nombre_texte(v)
        return int(t) if "." not in t else float(t)
    if isinstance(v, list):
        return [_sortir(x) for x in v]
    if isinstance(v, dict):
        return {str(k): _sortir(x) for k, x in v.items()}
    return None


externe = _sortir
interne = _entrer


def create(host=None, sortie=None):
    return Interpreter(host, sortie)


# --------------------------------------------------------------------------- #
# 自测
# --------------------------------------------------------------------------- #
def _cas():
    T, E = [], []
    N = ECHELLE

    def ok(src, want):
        T.append((src, want))

    def mal(src, étape):
        E.append((src, étape))

    # 字面量
    for txt, val in [("0", 0), ("1", N), ("42", 42 * N), ("3.5", 35 * 10 ** 8),
                     ("0.000000001", 1), ("1.000000001", 1000000001), ("-7", -7 * N)]:
        ok(txt, val)
    ok('"bonjour"', "bonjour")
    ok("«bonjour»", "bonjour")
    ok("OUI", True)
    ok("NON", False)
    ok("VIDE", None)
    ok("‹1, 2, 3›", [N, 2 * N, 3 * N])
    ok("‹›", [])
    ok("⟬⟭", {})
    ok('⟬nom ⟼ "ada"⟭', {"nom": "ada"})

    # 算术
    ok("1 plus 2 fois 3", 7 * N)
    ok("(1 plus 2) fois 3", 9 * N)
    ok("7 sur 2", 35 * 10 ** 8)
    ok("6 sur 2", 3 * N)
    ok("7 modulo 3", 1 * N)
    ok("2 puissance 10", 1024 * N)
    ok("2 fois 1.5", 3 * N)
    ok("10 sur 4", 25 * 10 ** 8)
    ok("moins 5 plus 3", -2 * N)
    ok("1 plus 2 plus 3", 6 * N)
    ok("10 moins 3 moins 2", 5 * N)
    ok("2 puissance 3 puissance 2", 512 * N)
    # 比较与逻辑
    ok("1 plus petit que 2", True)
    ok("2 plus grand que 1", True)
    ok("1 égal à 1", True)
    ok("1 différent de 2", True)
    ok("5 au moins 3", True)
    ok("2 au plus 2", True)
    ok("OUI et NON", False)
    ok("NON ou OUI", True)
    ok("non OUI", False)
    # 文本
    ok('"a" plus "b"', "ab")
    ok('mesure de "salut"', 5 * N)
    ok('mesure de "é"', 1 * N)
    ok('en-texte de 3.5', "3.5")
    ok('en-texte de 3', "3")
    ok('abaissé de "ABC"', "abc")
    ok('haussé de "abc"', "ABC")
    ok('découpe de "a,b,c" par ","', ["a", "b", "c"])
    ok('découpe de "abc" par ""', ["a", "b", "c"])
    ok('recoud de ‹"a", "b"› avec "-"', "a-b")
    ok('renferme de "salut" dans "alu"', True)
    ok('renferme de "salut" dans "zzz"', False)
    ok('bribe de "abcdef" depuis 2 jusqu\'à 4', "bcd")
    ok('en-nombre de "42"', 42 * N)
    ok('égalise de "x"', "x")
    # 序列
    ok("élément 1 de ‹10, 20›", 10 * N)
    ok("élément 2 de ‹10, 20›", 20 * N)
    ok("mesure de ‹1, 2, 3›", 3 * N)
    ok("total de ‹1, 2, 3›", 6 * N)
    ok("total de ‹1.5, 2.5›", 4 * N)
    ok("ordonne de ‹3, 1, 2›", [N, 2 * N, 3 * N])
    ok("ordonne de ‹\"b\", \"a\"›", ["a", "b"])
    ok("ajoute-fin de ‹1› avec 2", [N, 2 * N])
    ok("bribe de ‹1, 2, 3, 4› depuis 2 jusqu'à 3", [2 * N, 3 * N])
    ok("renferme de ‹1, 2› dans 2", True)
    # 记录
    ok('champ "score" de ⟬nom ⟼ "ada", score ⟼ 5⟭', 5 * N)
    ok('clefs de ⟬a ⟼ 1, b ⟼ 2⟭', ["a", "b"])
    ok('teneurs de ⟬a ⟼ 1, b ⟼ 2⟭', [N, 2 * N])
    ok('tordu de ⟬a ⟼ 1⟭ avec "b" et 2', {"a": N, "b": 2 * N})
    ok('mesure de ⟬a ⟼ 1⟭', N)
    ok('fige de ⟬a ⟼ 1⟭', '{"a":1}')
    ok("fige de ‹1, 2›", "[1,2]")
    ok("fige de OUI", "true")
    ok("fige de VIDE", "null")
    # 语句
    ok("que x vaille 5\nx", 5 * N)
    ok("que x vaille 1\nque x devienne 9\nx", 9 * N)
    ok("donne 5 à y\ny", 5 * N)
    ok("que n vaille 0\ntant que n plus petit que 3 ⟦\n  que n devienne n plus 1\n⟧\nn", 3 * N)
    ok("que s vaille 0\npour chaque x dans ‹1, 2, 3› ⟦\n  que s devienne s plus x\n⟧\ns", 6 * N)
    ok("si 1 plus petit que 2 alors ⟦ rends 10 ⟧\nrends 20", 10 * N)
    ok("si 2 plus petit que 1 alors ⟦ rends 10 ⟧ sinon ⟦ rends 20 ⟧", 20 * N)
    ok("si NON alors ⟦ rends 1 ⟧ sinonsi OUI alors ⟦ rends 2 ⟧\nrends 3", 2 * N)
    ok("on définit doubler de x comme ⟦\n  rends x fois 2\n⟧\ndoubler de 21", 42 * N)
    ok("on définit fib de n comme ⟦\n  si n plus petit que 2 alors ⟦ rends n ⟧\n"
       "  rends (fib de n moins 1) plus (fib de n moins 2)\n⟧\nfib de 10", 55 * N)
    ok("on définit somme de a et b comme ⟦ rends a plus b ⟧\nsomme de 3 et 4", 7 * N)
    ok("que a vaille ‹1, 2›\nque b vaille ajoute-fin de a avec 3\nb", [N, 2 * N, 3 * N])
    ok("affiche \"bonjour\"\nVIDE", None)
    ok("silence", None)
    ok("que t vaille 0\npour chaque c dans \"abc\" ⟦\n  que t devienne t plus 1\n⟧\nt", 3 * N)
    # 错误
    mal("1 sur 0", "calcul")
    mal("inconnu", "nom")
    mal('1 plus "a"', "forme")
    mal("que 5 vaille 1", "syntaxe")
    mal('"abc', "lexique")
    mal("bribe de 5 depuis 1 jusqu'à 2", "forme")
    mal("que x devienne 1", "nom")
    mal('en-nombre de "abc"', "calcul")
    mal("mesure de 5", "forme")
    mal("arrête\n", "calcul")
    mal("(1 plus 2", "syntaxe")
    mal("1 plus", "syntaxe")
    mal("élément 9 de ‹1, 2›", "calcul")
    mal('champ "zzz" de ⟬a ⟼ 1⟭', "forme")

    # ---- 生成用例（期望值在这里独立计算） ---- #
    couples = [(0, 1), (1, 2), (2, 3), (5, 7), (10, 4), (100, 25), (3, 3), (1, 10),
               (12, 5), (9, 2)]
    for a, b in couples:
        A, B = a * N, b * N
        ok("%d plus %d" % (a, b), A + B)
        ok("%d moins %d" % (a, b), A - B)
        ok("%d fois %d" % (a, b), (A * B) // N)
        if b != 0:
            ok("%d sur %d" % (a, b), (A * N) // B)
            ok("%d modulo %d" % (a, b), A - B * int(A / B))
        ok("%d plus grand que %d" % (a, b), a > b)
        ok("%d plus petit que %d" % (a, b), a < b)
        ok("%d égal à %d" % (a, b), a == b)
        ok("%d différent de %d" % (a, b), a != b)
        ok("%d au moins %d" % (a, b), a >= b)
        ok("%d au plus %d" % (a, b), a <= b)
        ok("élément 1 de ‹%d, %d›" % (a, b), A)
        ok("total de ‹%d, %d›" % (a, b), A + B)
        ok("ordonne de ‹%d, %d›" % (b, a), sorted([A, B]))
        ok("ajoute-fin de ‹%d› avec %d" % (a, b), [A, B])
        ok("bribe de ‹%d, %d› depuis 1 jusqu'à 1" % (a, b), [A])
        ok('clefs de ⟬a ⟼ %d, b ⟼ %d⟭' % (a, b), ["a", "b"])
        ok('teneurs de ⟬a ⟼ %d, b ⟼ %d⟭' % (a, b), [A, B])
        ok('champ "b" de ⟬a ⟼ %d, b ⟼ %d⟭' % (a, b), B)
        ok('mesure de ⟬a ⟼ %d, b ⟼ %d⟭' % (a, b), 2 * N)

    for txt, n in [("", 0), ("a", 1), ("abc", 3), ("é", 1), ("e\u0301", 1), ("ab cd", 5)]:
        ok('mesure de "%s"' % txt, n * N)
        ok('bribe de "%s" depuis 1 jusqu\'à %d' % (txt, max(1, n)), txt if n else "")
        ok('en-texte de "%s"' % txt, txt)

    for txt in ["AbC", "salut", "ÉCOLE", "x"]:
        ok('abaissé de "%s"' % txt, txt.lower())
        ok('haussé de "%s"' % txt, txt.upper())
        ok('mesure de "%s"' % txt, len(txt) * N)

    paires = [("a,b,c", ",", ["a", "b", "c"]), ("a-b", "-", ["a", "-b"][:0] or ["a", "b"]),
              ("1;2", ";", ["1", "2"]), ("abc", "b", ["a", "c"])]
    for txt, sep, want in paires:
        ok('découpe de "%s" par "%s"' % (txt, sep), want)
        ok('recoud de %s avec "%s"' % (
            "‹" + ", ".join('"%s"' % x for x in want) + "›", sep), sep.join(want))

    for src, want in [
        ("2 puissance 0", N), ("2 puissance 1", 2 * N), ("2 puissance 8", 256 * N),
        ("10 puissance 3", 1000 * N), ("1 puissance 99", N),
        ("0 fois 999", 0), ("0 plus 0", 0), ("1 moins 1", 0),
        ("100 sur 4", 25 * N), ("100 sur 8", 125 * 10 ** 8),
        ("9 modulo 4", 1 * N), ("10 modulo 5", 0),
    ]:
        ok(src, want)

    for k in (1, 2, 3, 5):
        corps = "\n".join("  que s devienne s plus %d" % i for i in range(1, k + 1))
        ok("que s vaille 0\npour chaque x dans ‹1, 2, 3› ⟦\n%s\n⟧\ns" % corps,
           (3 * (k * (k + 1) // 2)) * N)

    for n in (0, 1, 2, 3, 4, 5):
        ok("on définit f de n comme ⟦\n"
           "  si n plus petit que 1 alors ⟦ rends 0 ⟧\n"
           "  rends n plus f de n moins 1\n⟧\nf de %d" % n, (n * (n + 1) // 2) * N)

    for texte, sous in [("bonjour", "jour"), ("bonjour", "xyz"), ("", ""), ("abc", "")]:
        ok('renferme de "%s" dans "%s"' % (texte, sous), sous in texte)

    for a, b in [(1, 2), (2, 1), (3, 3)]:
        ok("si %d plus grand que %d alors ⟦ rends 100 ⟧ sinon ⟦ rends 200 ⟧" % (a, b),
           100 * N if a > b else 200 * N)

    for a, b in [(1, 2), (5, 6)]:
        ok("élément %d de ‹%d, %d›" % (1, a, b), a * N)
        ok("élément %d de ‹%d, %d›" % (2, a, b), b * N)

    for k in (1, 2, 4):
        ok("que x vaille %d\ntant que x plus petit que %d ⟦\n"
           "  que x devienne x plus 1\n⟧\nx" % (0, k), k * N)

    return T, E


def _self_test(verbeux=False):
    T, E = _cas()
    passé = raté = 0
    for src, want in T:
        try:
            interp = create({}, sortie=lambda s: None)
            got = interp.eval(src, "test")
        except Défaut as exc:
            raté += 1
            if verbeux:
                print("  FAIL", json.dumps(src, ensure_ascii=False)[:70], "->", exc.étape, exc.note)
            continue
        if égal(got, want):
            passé += 1
        else:
            raté += 1
            if verbeux:
                print("  FAIL", json.dumps(src, ensure_ascii=False)[:70], "->", repr(got), "≠", repr(want))
    for src, étape in E:
        try:
            interp = create({}, sortie=lambda s: None)
            interp.eval(src, "test")
        except Défaut as exc:
            if exc.étape == étape:
                passé += 1
            else:
                raté += 1
                if verbeux:
                    print("  FAIL", json.dumps(src, ensure_ascii=False)[:70], "->", exc.étape, "≠", étape)
        except Exception as exc:
            raté += 1
            if verbeux:
                print("  FAIL", json.dumps(src, ensure_ascii=False)[:70], "-> exception", exc)
        else:
            raté += 1
            if verbeux:
                print("  FAIL", json.dumps(src, ensure_ascii=False)[:70], "-> aucune erreur")
    print("%s: %d passed, %d failed" % (LANG, passé, raté))
    return 0 if raté == 0 else 1


def _plain(v):
    return _sortir(v)


def _err(exc):
    if isinstance(exc, Défaut):
        return {"défaut": exc.to_dict()}
    return {"défaut": {"étape": "hôte", "ligne": None, "note": "%s: %s" % (type(exc).__name__, exc)}}


def _lire(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return f.read()


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        print("usage: python sillage.py {test|run|ast|tokens|repl} ...")
        return 2
    cmd = argv[0]
    if cmd == "test":
        return _self_test("-v" in argv)
    if cmd in ("run", "ast", "tokens"):
        if len(argv) < 2:
            print(json.dumps({"défaut": {"étape": "syntaxe", "ligne": None, "note": "fichier manquant"}}))
            return 2
        try:
            src = _lire(argv[1])
            if cmd == "tokens":
                print(json.dumps([[j.genre, j.valeur, j.ligne] for j in lexer(src)],
                                 ensure_ascii=False, default=str))
                return 0
            if cmd == "ast":
                print(json.dumps(analyser(src), ensure_ascii=False, default=str))
                return 0
            interp = create({})
            valeur = interp.eval(src, argv[1])
            if len(argv) > 2:
                # call() 已经返回「外部形态」，不要再编组一次
                out = interp.call(ENTREE, [json.loads(argv[2])])
                print(json.dumps(out, ensure_ascii=False, indent=2))
            else:
                print(json.dumps(_plain(valeur), ensure_ascii=False, indent=2))
            return 0
        except Exception as exc:
            print(json.dumps(_err(exc), ensure_ascii=False))
            return 1
    if cmd == "repl":
        interp = create({})
        while True:
            try:
                ligne = input("sillage> ")
            except EOFError:
                break
            if not ligne.strip():
                continue
            try:
                print(texte_de(interp.eval(ligne, "repl")))
            except Défaut as exc:
                print(json.dumps(_err(exc), ensure_ascii=False))
        return 0
    print("commande inconnue: " + cmd)
    return 2


if __name__ == "__main__":
    sys.exit(main())
