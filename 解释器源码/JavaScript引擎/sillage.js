/* SILLAGE 3.0 — 一门独立的法语编程语言（JavaScript 参考引擎，与 sillage.py 逐行等价）。
 *
 * 本文件是 `sillage.py` 的**逐行忠实移植**：同样的词法、语法、求值、值模型、错误模型、
 * 内置函数、模块结果规则、宿主边界编组。无任何依赖，浏览器（<script>）与 node 都能直接加载，
 * 加载后挂全局 `SILLAGE`（同时写 `module.exports`）。
 *
 * 值模型（与 Python 版完全一致）
 *   Nombre  十进制定点，内部为 BigInt，单位 1e-9（ECHELLE = 1000000000n）
 *   Texte   JS 字符串，按字素簇计长与索引，下标从 1 开始
 *   Suite   Array，下标从 1 开始（不可变使用）
 *   Fiche   Map，保持插入顺序，键是 Texte
 *   OUI=true / NON=false / VIDE=null
 *
 * 错误：Écart{étape, ligne, note}，étape ∈ lexique|syntaxe|forme|nom|calcul|hôte
 *
 * 模块结果：最后一条顶层 `rends` → 否则最后一条顶层表达式句 → 否则 VIDE；
 *           最后一条顶层句是 `silence` 时强制 VIDE。
 *
 * 接口：
 *   SILLAGE.create(host) -> { eval(src, nom), call(nom, args), globals() }
 *   host: { "carnet.lister": fn, ... } —— 键是本语言宿主名，映射由宿主负责。
 *   eval 返回模块结果（内部值）；模块结果为 VIDE 时返回解释器自身，
 *   它可继续 `.call(...)`，并且 JSON.stringify 得到 null。
 *   call 返回**外部形态**（已编组）。
 *
 * 额外导出（便于比对/宿主使用）：entrer, sortir, json, texte, egal, lexer, analyser,
 *   graphemes, nombreTexte, ECHELLE, HOSTE, ERREUR, Ecart。
 */
(function (global) {
  "use strict";

  const LANG = "sillage";
  const VERSION = "3.0.0";
  const FILE_EXT = ".sil";
  const ENTREE = "handle";

  /* 定点比例：1e9。Python 用整数，这里用 BigInt，语义相同但不会丢精度。 */
  const ECHELLE = 1000000000n;
  const ECHELLE_N = 1e9;
  const NOTE_MAX = 1e15;
  /* |x| > NOTE_MAX  <=>  |v| > 1e24（内部单位 1e-9） */
  const LIMITE = 1000000000000000000000000n;
  /* Number 能精确表示的最大整数（2^53-1） */
  const MAX_SUR = 9007199254740991n;

  /* ------------------------------------------------------------------ */
  /* 错误                                                                */
  /* ------------------------------------------------------------------ */
  class Écart extends Error {
    constructor(étape, note, ligne) {
      super(note);
      this.name = "Écart";
      this.étape = étape;
      this.note = note;
      this.ligne = ligne === undefined ? null : ligne;
    }
    to_dict() {
      return { "étape": this.étape, "ligne": this.ligne, "note": this.note };
    }
    toString() {
      return this.note;
    }
  }

  const ERREUR = Écart;
  /* Python 版把错误类改名成 Défaut 过；两个名字都留作别名。 */
  const Défaut = Écart;
  const Ecart = Écart;

  class _Rends extends Error {
    constructor(valeur) {
      super("rends");
      this.name = "_Rends";
      this.valeur = valeur;
    }
  }

  class _Brise extends Error {
    constructor() {
      super("brise");
      this.name = "_Brise";
    }
  }

  class _Passe extends Error {
    constructor() {
      super("passe");
      this.name = "_Passe";
    }
  }

  const RIEN = Symbol("RIEN");
  const ABSENT = Symbol("ABSENT");

  /* ------------------------------------------------------------------ */
  /* 工具：Python 语义的小助手                                            */
  /* ------------------------------------------------------------------ */
  function est_nombre(v) {
    return typeof v === "bigint";
  }

  /* Python 的 round()：四舍六入、.5 取偶。用于 _entrer 与 ** */
  function arrondi_py(x) {
    if (!isFinite(x)) return x;
    const f = Math.floor(x);
    const d = x - f;
    if (d > 0.5) return f + 1;
    if (d < 0.5) return f;
    return f % 2 === 0 ? f : f + 1;
  }

  /* Python 的 repr(str)，用于错误消息里的 %r */
  function repr_texte(s) {
    s = String(s);
    const quote = s.indexOf("'") >= 0 && s.indexOf('"') < 0 ? '"' : "'";
    let out = quote;
    for (const ch of s) {
      if (ch === "\\") out += "\\\\";
      else if (ch === quote) out += "\\" + quote;
      else if (ch === "\n") out += "\\n";
      else if (ch === "\r") out += "\\r";
      else if (ch === "\t") out += "\\t";
      else {
        const o = ch.codePointAt(0);
        if (o < 0x20 || o === 0x7f) out += "\\x" + o.toString(16).padStart(2, "0");
        else out += ch;
      }
    }
    return out + quote;
  }

  function message_de(e) {
    if (e instanceof Error) return e.message === undefined ? String(e) : e.message;
    return String(e);
  }

  /* ------------------------------------------------------------------ */
  /* 定点 Nombre                                                         */
  /* ------------------------------------------------------------------ */
  const RE_CHIFFRES = /^[0-9]+$/;

  function _nombre_depuis_texte(t) {
    let s = String(t).trim().replace(/\u00a0/g, "").replace(/ /g, "").replace(/,/g, ".");
    if (!s) return ABSENT;
    let signe = 1n;
    if (s[0] === "+" || s[0] === "-") {
      if (s[0] === "-") signe = -1n;
      s = s.slice(1);
    }
    if (!s) return ABSENT;
    let entier, frac;
    const point = s.indexOf(".");
    if (point >= 0) {
      entier = s.slice(0, point);
      frac = s.slice(point + 1);
    } else {
      entier = s;
      frac = "";
    }
    if (entier && !RE_CHIFFRES.test(entier)) return ABSENT;
    if (frac && !RE_CHIFFRES.test(frac)) return ABSENT;
    if (!entier && !frac) return ABSENT;
    if (frac.length > 9) {
      if (frac[9] >= "5") {
        frac = frac.slice(0, 9);
        const base = BigInt(entier || "0") * ECHELLE + BigInt(frac) + 1n;
        return signe * base;
      }
      frac = frac.slice(0, 9);
    }
    frac = (frac + "000000000").slice(0, 9);
    return signe * (BigInt(entier || "0") * ECHELLE + BigInt(frac || "0"));
  }

  function nombre_texte(v) {
    if (v === 0n) return "0";
    const signe = v < 0n ? "-" : "";
    const a = v < 0n ? -v : v;
    const entier = a / ECHELLE;
    const frac = a % ECHELLE;
    if (frac === 0n) return signe + entier.toString();
    return signe + entier.toString() + "." + frac.toString().padStart(9, "0").replace(/0+$/, "");
  }

  function _bornes(v) {
    const a = v < 0n ? -v : v;
    if (a > LIMITE) throw new Écart("calcul", "nombre hors limites (|x| > 1e15)");
    return v;
  }

  /* ------------------------------------------------------------------ */
  /* 词法                                                                */
  /* ------------------------------------------------------------------ */
  const T_MOT = "mot";
  const T_NOMBRE = "nombre";
  const T_TEXTE = "texte";
  const T_SYM = "sym";
  const T_FIN = "fin";
  const T_EOF = "eof";

  const SYMBOLES = ["☑", "☒", "∅", "⟦", "⟧", "‹", "›", "⟬", "⟭", "⟼", "≠", "≢", "≤", "≥", "⩽", "⩾", ">=", "<=",
    "==", "≟", "∧", "∨", "&&", "||", "¬", "!", "×", "÷", "−", "⊕", "⊖", "⊗", "⊘",
    "⊞", "↑", "⪢", "⪡", "+", "-", "*", "/", "%", "^", "(", ")", "[", "]",
    "{", "}", ",", ":", "=", "<", ">"];
  const CHAÎNES = [["«", "»"], ['"', '"']];

  const RE_LETTRE = /^[\p{L}_]$/u;
  const RE_ALNUM = /^[\p{L}\p{N}_']$/u;
  const RE_CHIFFRE = /^[0-9]$/;

  function _lettre(ch) {
    return RE_LETTRE.test(ch);
  }

  function _alnum(ch) {
    return RE_ALNUM.test(ch);
  }

  class Jeton {
    constructor(genre, valeur, ligne) {
      this.genre = genre;
      this.valeur = valeur;
      this.ligne = ligne;
    }
    toString() {
      return this.genre + "(" + repr_texte(this.valeur) + ")";
    }
  }

  function lexer(src) {
    if (src.charCodeAt(0) === 0xfeff) src = src.slice(1);
    src = src.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
    const jetons = [];
    const n = src.length;
    let i = 0;
    let ligne = 1;
    const echappements = { "n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "«": "«", "»": "»" };
    while (i < n) {
      const ch = String.fromCodePoint(src.codePointAt(i));
      if (ch === "\n") {
        jetons.push(new Jeton(T_FIN, null, ligne));
        ligne += 1;
        i += 1;
        continue;
      }
      if (ch === " " || ch === "\t" || ch === "\f" || ch === "\v" || ch === "\u00a0") {
        i += 1;
        continue;
      }
      if (ch === "#" || src.startsWith("‖", i)) {
        const j = src.indexOf("\n", i);
        i = j < 0 ? n : j;
        continue;
      }
      let fait = false;
      for (let k = 0; k < CHAÎNES.length; k++) {
        const ouv = CHAÎNES[k][0];
        const ferm = CHAÎNES[k][1];
        if (src.startsWith(ouv, i)) {
          i += ouv.length;
          const buf = [];
          for (;;) {
            if (i >= n) throw new Écart("lexique", "texte non terminé", ligne);
            const c = String.fromCodePoint(src.codePointAt(i));
            if (c === "\\" && ouv === '"') {
              if (i + 1 >= n) throw new Écart("lexique", "échappement incomplet", ligne);
              const e = String.fromCodePoint(src.codePointAt(i + 1));
              buf.push(Object.prototype.hasOwnProperty.call(echappements, e) ? echappements[e] : e);
              i += 1 + e.length;
              continue;
            }
            if (src.startsWith(ferm, i)) {
              i += ferm.length;
              break;
            }
            if (c === "\n") throw new Écart("lexique", "texte non terminé", ligne);
            buf.push(c);
            i += c.length;
          }
          jetons.push(new Jeton(T_TEXTE, buf.join(""), ligne));
          fait = true;
          break;
        }
      }
      if (fait) continue;
      if (RE_CHIFFRE.test(ch)) {
        let j = i;
        for (;;) {
          if (j >= n) break;
          const c = String.fromCodePoint(src.codePointAt(j));
          if (RE_CHIFFRE.test(c)) {
            j += 1;
            continue;
          }
          if (c === "." && j + 1 < n) {
            const d = String.fromCodePoint(src.codePointAt(j + 1));
            if (RE_CHIFFRE.test(d)) {
              j += 1;
              continue;
            }
          }
          break;
        }
        const brut = src.slice(i, j);
        const val = _nombre_depuis_texte(brut);
        if (val === ABSENT) throw new Écart("lexique", "nombre illisible: " + brut, ligne);
        jetons.push(new Jeton(T_NOMBRE, _bornes(val), ligne));
        i = j;
        continue;
      }
      if (_lettre(ch)) {
        let j = i;
        for (;;) {
          if (j >= n) break;
          const c = String.fromCodePoint(src.codePointAt(j));
          if (_alnum(c) || c === "-") {
            j += c.length;
            continue;
          }
          break;
        }
        let mot = src.slice(i, j);
        for (;;) {
          if (!(j + 1 < n && src[j] === ".")) break;
          const d = String.fromCodePoint(src.codePointAt(j + 1));
          if (!_lettre(d)) break;
          let k = j + 1;
          for (;;) {
            if (k >= n) break;
            const c = String.fromCodePoint(src.codePointAt(k));
            if (_alnum(c) || c === "-") {
              k += c.length;
              continue;
            }
            break;
          }
          mot += src.slice(j, k);
          j = k;
        }
        jetons.push(new Jeton(T_MOT, mot, ligne));
        i = j;
        continue;
      }
      let trouve = null;
      for (let k = 0; k < SYMBOLES.length; k++) {
        if (src.startsWith(SYMBOLES[k], i)) {
          trouve = SYMBOLES[k];
          break;
        }
      }
      if (trouve === null) throw new Écart("lexique", "caractère inattendu: " + repr_texte(ch), ligne);
      jetons.push(new Jeton(T_SYM, trouve, ligne));
      i += trouve.length;
    }
    jetons.push(new Jeton(T_EOF, null, ligne));
    return jetons;
  }

  /* Python 的 repr()，用于 %r 位置（数字不带引号） */
  function repr_py(x) {
    if (typeof x === "bigint") return x.toString();
    if (typeof x === "string") return repr_texte(x);
    if (x === null || x === undefined) return "None";
    if (typeof x === "boolean") return x ? "True" : "False";
    return String(x);
  }

  /* ------------------------------------------------------------------ */
  /* 语法                                                                */
  /* ------------------------------------------------------------------ */
  class Parseur {
    constructor(jetons) {
      this.t = jetons;
      this.i = 0;
    }

    pic(k) {
      const j = this.i + (k || 0);
      return j < this.t.length ? this.t[j] : this.t[this.t.length - 1];
    }

    val(k) {
      const j = this.pic(k);
      return j.genre === T_MOT || j.genre === T_SYM ? j.valeur : null;
    }

    genre(k) {
      return this.pic(k).genre;
    }

    est(valeurs, k) {
      const v = this.val(k);
      if (v === null || v === undefined) return false;
      if (typeof valeurs === "string") return v === valeurs;
      return valeurs.indexOf(v) >= 0;
    }

    avale() {
      const j = this.t[this.i];
      if (j.genre !== T_EOF) this.i += 1;
      return j;
    }

    attendre(valeurs, quoi) {
      if (!this.est(valeurs)) {
        const j = this.pic();
        const montre = j.valeur !== null && j.valeur !== undefined
          ? j.valeur
          : (j.genre === T_FIN ? "fin de ligne" : "fin de fichier");
        throw new Écart("syntaxe", "attendu " + quoi + ", trouvé " + repr_py(montre), j.ligne);
      }
      return this.avale();
    }

    attendre_nom(quoi) {
      const j = this.pic();
      if (j.genre !== T_MOT) {
        const montre = j.valeur !== null && j.valeur !== undefined ? j.valeur : "fin d'instruction";
        throw new Écart("syntaxe", "attendu " + quoi + ", trouvé " + repr_py(montre), j.ligne);
      }
      this.avale();
      return j.valeur;
    }

    /* -- 程序与块 -- */
    programme() {
      const stmts = this.sequence(false);
      const j = this.pic();
      if (j.genre !== T_EOF) {
        throw new Écart("syntaxe", "entrée en trop: " + repr_py(j.valeur), j.ligne);
      }
      return stmts;
    }

    sequence(dans_bloc) {
      const stmts = [];
      for (;;) {
        while (this.genre() === T_FIN) this.avale();
        const j = this.pic();
        if (j.genre === T_EOF) break;
        if (dans_bloc && this.est("⟧")) break;
        stmts.push(this.instruction());
      }
      return stmts;
    }

    bloc() {
      this.attendre("⟦", "⟦ (ouverture de bloc)");
      const corps = this.sequence(true);
      this.attendre("⟧", "⟧ (fermeture de bloc)");
      return corps;
    }

    sauter_fins() {
      while (this.genre() === T_FIN) this.avale();
    }

    /* -- 语句 -- */
    instruction() {
      const v = this.val();
      const ligne = this.pic().ligne;

      if (v === "que" || v === "soit") {
        this.avale();
        const nom = this.attendre_nom("un nom");
        if (this.est("vaille")) {
          this.avale();
          return ["lier", nom, this.expression(), ligne];
        }
        if (this.est("devienne") || this.est("remue")) {
          this.avale();
          return ["relier", nom, this.expression(), ligne];
        }
        throw new Écart("syntaxe", "attendu « vaille » ou « devienne »", ligne);
      }

      if (v === "donne") {
        this.avale();
        const val = this.expression();
        this.attendre("à", "« à »");
        const nom = this.attendre_nom("un nom");
        return ["lier", nom, val, ligne];
      }

      if (v === "affiche" || v === "dis" || v === "trace") {
        this.avale();
        return ["afficher", this.expression(), ligne];
      }

      if (v === "rends") {
        this.avale();
        return ["rends", this.expression(), ligne];
      }

      if (v === "silence") {
        this.avale();
        return ["silence", ligne];
      }

      if (v === "si" || v === "quand" || v === "lorsque") {
        this.avale();
        return this.chaine_si(ligne);
      }

      if (v === "tantque" || v === "boucle") {
        this.avale();
        return ["tantque", this.expression(), this.bloc(), ligne];
      }

      if (v === "tant") {
        this.avale();
        this.attendre("que", "« que »");
        return ["tantque", this.expression(), this.bloc(), ligne];
      }

      if (v === "pour") {
        this.avale();
        this.attendre("chaque", "« chaque »");
        const nom = this.attendre_nom("un nom");
        this.attendre("dans", "« dans »");
        return ["pourchaque", nom, this.expression(), this.bloc(), ligne];
      }

      if (v === "on") {
        this.avale();
        this.attendre("définit", "« définit »");
        const nom = this.attendre_nom("un nom de geste");
        this.attendre("de", "« de »");
        const params = this.params();
        this.attendre("comme", "« comme »");
        return ["definir", nom, params, this.bloc(), ligne];
      }

      if (v === "arrête" || v === "casse") {
        this.avale();
        if (!(this.genre() === T_FIN || this.est("⟧") || this.genre() === T_EOF)) {
          throw new Écart("calcul", "arrête hors d'une boucle", ligne);
        }
        return ["brise", ligne];
      }
      if (v === "continue" || v === "passe") {
        this.avale();
        if (!(this.genre() === T_FIN || this.est("⟧") || this.genre() === T_EOF)) {
          throw new Écart("calcul", "passe hors d'une boucle", ligne);
        }
        return ["passe", ligne];
      }

      return ["expr", this.expression(), ligne];
    }

    params() {
      if (this.est("(")) {
        this.avale();
        const noms = [];
        if (!this.est(")")) {
          for (;;) {
            noms.push(this.attendre_nom("un paramètre"));
            if (this.est(",")) {
              this.avale();
              continue;
            }
            break;
          }
        }
        this.attendre(")", ")");
        return noms;
      }
      const noms = [this.attendre_nom("un paramètre")];
      while (this.est("et")) {
        this.avale();
        noms.push(this.attendre_nom("un paramètre"));
      }
      return noms;
    }

    chaine_si(ligne) {
      const cond = this.expression();
      this.attendre("alors", "« alors »");
      const branches = [[cond, this.bloc()]];
      let sinon = null;
      for (;;) {
        const sauve = this.i;
        this.sauter_fins();
        if (this.est("sinonsi") || this.est("oualors")) {
          this.avale();
          const c2 = this.expression();
          this.attendre("alors", "« alors »");
          branches.push([c2, this.bloc()]);
          continue;
        }
        if (this.est("sinon") || this.est("autrement")) {
          this.avale();
          sinon = this.bloc();
          break;
        }
        this.i = sauve;
        break;
      }
      return ["si", branches, sinon, ligne];
    }

    /* -- 表达式 -- */
    expression() {
      return this.ou();
    }

    ou() {
      let gauche = this.et();
      while (this.est("ou") || this.est("∨") || this.est("||")) {
        this.avale();
        gauche = ["ou", gauche, this.et()];
      }
      return gauche;
    }

    et() {
      let gauche = this.comparaison();
      while (this.est("et") || this.est("∧") || this.est("&&")) {
        this.avale();
        gauche = ["et", gauche, this.comparaison()];
      }
      return gauche;
    }

    comparaison() {
      let gauche = this.somme();
      for (;;) {
        let canon = null;
        for (let c0 = 0; c0 < Parseur.SEQS_CMP.length; c0++) {
          const mots = Parseur.SEQS_CMP[c0][0];
          const c = Parseur.SEQS_CMP[c0][1];
          let ok = true;
          for (let k = 0; k < mots.length; k++) {
            if (!this.est(mots[k], k)) {
              ok = false;
              break;
            }
          }
          if (ok) {
            for (let k = 0; k < mots.length; k++) this.avale();
            canon = c;
            break;
          }
        }
        if (canon === null) {
          const v = this.val();
          if (v !== null && Object.prototype.hasOwnProperty.call(Parseur.SYMS_CMP, v)) {
            this.avale();
            canon = Parseur.SYMS_CMP[v];
          } else if (v === "égal") {
            this.avale();
            canon = "==";
          } else if (v === "différent") {
            this.avale();
            canon = "!=";
          }
        }
        if (canon === null) break;
        gauche = ["cmp", canon, gauche, this.somme()];
      }
      return gauche;
    }

    /* 调用实参：只解析到「加减」层，不吞掉比较、逻辑 et/ou。 */
    argument() {
      return this.somme();
    }

    somme() {
      let gauche = this.produit();
      for (;;) {
        const v = this.val();
        /* 「plus petit que」「plus grand que」是比较符，不是加法 */
        if ((v === "plus" || v === "+") && (this.est("petit", 1) || this.est("grand", 1))) break;
        if (v === "plus" || v === "+" || v === "⊕" || v === "moins" || v === "−" || v === "-" || v === "⊖") {
          this.avale();
          const canon = v === "plus" || v === "+" || v === "⊕" ? "+" : "-";
          gauche = ["bin", canon, gauche, this.produit()];
          continue;
        }
        break;
      }
      return gauche;
    }

    produit() {
      let gauche = this.puissance();
      for (;;) {
        const v = this.val();
        if (v !== null && Object.prototype.hasOwnProperty.call(Parseur.CANON_PRODUIT, v)) {
          this.avale();
          gauche = ["bin", Parseur.CANON_PRODUIT[v], gauche, this.puissance()];
          continue;
        }
        break;
      }
      return gauche;
    }

    puissance() {
      const gauche = this.unaire();
      if (this.est("puissance") || this.est("^") || this.est("↑")) {
        this.avale();
        return ["bin", "**", gauche, this.puissance()];
      }
      return gauche;
    }

    unaire() {
      const v = this.val();
      if (v === "non" || v === "¬" || v === "!") {
        this.avale();
        return ["un", "non", this.unaire()];
      }
      if (v === "moins" || v === "−" || v === "-") {
        this.avale();
        return ["un", "-", this.unaire()];
      }
      return this.primaire();
    }

    primaire() {
      const j = this.pic();
      const genre = j.genre;
      const val = j.valeur;
      const ligne = j.ligne;
      if (genre === T_NOMBRE) {
        this.avale();
        return ["nombre", val];
      }
      if (genre === T_TEXTE) {
        this.avale();
        return ["texte", val];
      }
      if (genre === T_MOT) {
        if (val === "OUI" || val === "oui" || val === "vrai" || val === "☑") {
          this.avale();
          return ["oui"];
        }
        if (val === "NON" || val === "faux" || val === "☒") {
          this.avale();
          return ["non"];
        }
        if (val === "VIDE" || val === "vide" || val === "∅" || val === "rien") {
          this.avale();
          return ["vide"];
        }
        if (val === "élément") {
          this.avale();
          const idx = this.simple();
          this.attendre("de", "« de »");
          return ["element", idx, this.primaire()];
        }
        if (val === "champ") {
          this.avale();
          const cle = this.simple();
          this.attendre("de", "« de »");
          return ["champ-de", cle, this.primaire()];
        }
        this.avale();
        return this.application(val);
      }
      if (genre === T_SYM) {
        if (val === "‹") {
          this.avale();
          const items = [];
          this.sauter_fins();
          if (!this.est("›")) {
            for (;;) {
              this.sauter_fins();
              items.push(this.expression());
              this.sauter_fins();
              if (this.est(",")) {
                this.avale();
                continue;
              }
              break;
            }
          }
          this.sauter_fins();
          this.attendre("›", "›");
          return ["suite", items];
        }
        if (val === "⟬") {
          this.avale();
          const paires = [];
          this.sauter_fins();
          if (!this.est("⟭")) {
            for (;;) {
              this.sauter_fins();
              const kt = this.pic();
              let cle;
              if (kt.genre === T_MOT || kt.genre === T_TEXTE) {
                cle = kt.valeur;
                this.avale();
              } else {
                throw new Écart("syntaxe", "clé de fiche attendue", kt.ligne);
              }
              this.attendre("⟼", "⟼");
              paires.push([cle, this.expression()]);
              this.sauter_fins();
              if (this.est(",")) {
                this.avale();
                continue;
              }
              break;
            }
          }
          this.sauter_fins();
          this.attendre("⟭", "⟭");
          return ["fiche", paires];
        }
        if (val === "(") {
          this.avale();
          const e = this.expression();
          this.attendre(")", ")");
          return e;
        }
        if (val === "☑") {
          this.avale();
          return ["oui"];
        }
        if (val === "☒") {
          this.avale();
          return ["non"];
        }
        if (val === "∅") {
          this.avale();
          return ["vide"];
        }
      }
      const montre = val !== null && val !== undefined
        ? val
        : (genre === T_FIN ? "fin de ligne" : "fin de fichier");
      throw new Écart("syntaxe", "expression attendue, trouvé " + repr_py(montre), ligne);
    }

    /* 下标 / 字段键：只吃一个基本项，**不做**介词调用（避免吞掉后面的 de）。 */
    simple() {
      const j = this.pic();
      const genre = j.genre;
      const val = j.valeur;
      if (genre === T_NOMBRE) {
        this.avale();
        return ["nombre", val];
      }
      if (genre === T_TEXTE) {
        this.avale();
        return ["texte", val];
      }
      if (genre === T_MOT) {
        this.avale();
        if (val === "OUI" || val === "oui" || val === "vrai" || val === "☑") return ["oui"];
        if (val === "NON" || val === "faux" || val === "☒") return ["non"];
        if (val === "VIDE" || val === "vide" || val === "∅" || val === "rien") return ["vide"];
        return ["nom", val];
      }
      if (genre === T_SYM && val === "(") {
        this.avale();
        const e = this.expression();
        this.attendre(")", ")");
        return e;
      }
      if (genre === T_SYM && val === "‹") return this.primaire();
      if (genre === T_SYM && val === "⟬") return this.primaire();
      const montre = val !== null && val !== undefined
        ? val
        : (genre === T_FIN ? "fin de ligne" : "fin de fichier");
      throw new Écart("syntaxe", "expression attendue, trouvé " + repr_py(montre), j.ligne);
    }

    application(nom) {
      if (nom.indexOf(".") >= 0) {
        const args = [];
        while (this.debut_expression()) args.push(this.argument());
        return ["appel", nom, args];
      }
      if (this.est("de")) {
        this.avale();
        const args = [this.argument()];
        for (;;) {
          if (this.est("et")) {
            this.avale();
            args.push(this.argument());
            continue;
          }
          if (this.est("par") || this.est("avec") || this.est("dans")) {
            this.avale();
            args.push(this.argument());
            continue;
          }
          if (this.est("depuis")) {
            this.avale();
            const debut = this.argument();
            this.attendre("jusqu'à", "« jusqu'à »");
            const fin = this.argument();
            args.push(debut);
            args.push(fin);
            continue;
          }
          break;
        }
        return ["appel", nom, args];
      }
      if (this.est("(")) {
        this.avale();
        const args = [];
        if (!this.est(")")) {
          for (;;) {
            args.push(this.expression());
            if (this.est(",")) {
              this.avale();
              continue;
            }
            break;
          }
        }
        this.attendre(")", ")");
        return ["appel", nom, args];
      }
      return ["nom", nom];
    }

    debut_expression(k) {
      const j = this.pic(k);
      if (j.genre === T_NOMBRE || j.genre === T_TEXTE || j.genre === T_MOT) return true;
      return j.genre === T_SYM && ["‹", "⟬", "(", "¬", "−", "-"].indexOf(j.valeur) >= 0;
    }
  }

  Parseur.SEQS_CMP = [
    [["plus", "grand", "que"], ">"],
    [["plus", "petit", "que"], "<"],
    [["au", "moins"], ">="],
    [["au", "plus"], "<="],
    [["égal", "à"], "=="],
    [["différent", "de"], "!="]
  ];
  Parseur.SYMS_CMP = {
    ">": ">", "<": "<", ">=": ">=", "<=": "<=", "≥": ">=", "≤": "<=",
    "⩾": ">=", "⩽": "<=", "⪢": ">", "⪡": "<",
    "=": "==", "==": "==", "≠": "!=", "≢": "!=", "≟": "=="
  };
  Parseur.CANON_PRODUIT = {
    "fois": "*", "×": "*", "*": "*", "⊗": "*",
    "sur": "/", "÷": "/", "/": "/", "⊘": "/",
    "modulo": "%", "%": "%", "⊞": "%"
  };

  function analyser(src) {
    return new Parseur(lexer(src)).programme();
  }

  /* ------------------------------------------------------------------ */
  /* 运行期                                                              */
  /* ------------------------------------------------------------------ */
  class Portée {
    constructor(parent) {
      this.noms = new Map();
      this.parent = parent === undefined ? null : parent;
    }

    définir(nom, valeur) {
      this.noms.set(nom, valeur);
    }

    _trouver(nom) {
      let p = this;
      while (p !== null) {
        if (p.noms.has(nom)) return p;
        p = p.parent;
      }
      return null;
    }

    lire(nom) {
      const p = this._trouver(nom);
      return p === null ? ABSENT : p.noms.get(nom);
    }

    affecter(nom, valeur) {
      const p = this._trouver(nom);
      if (p === null) return false;
      p.noms.set(nom, valeur);
      return true;
    }
  }

  class Geste {
    constructor(nom, params, corps, portée) {
      this.nom = nom;
      this.params = params;
      this.corps = corps;
      this.portée = portée;
    }
  }

  class Natif {
    constructor(nom, appel) {
      this.nom = nom;
      this.appel = appel;
    }
  }

  /* 字素簇近似（与 Python 完全相同的范围） */
  function graphemes(t) {
    const out = [];
    for (const ch of t) {
      if (out.length) {
        const o = ch.codePointAt(0);
        if ((o >= 0x0300 && o <= 0x036f) || (o >= 0x1ab0 && o <= 0x1aff)
          || (o >= 0x20d0 && o <= 0x20ff) || (o >= 0xfe20 && o <= 0xfe2f)
          || o === 0xfe0f || o === 0x200d || (o >= 0x1f3fb && o <= 0x1f3ff)) {
          out[out.length - 1] += ch;
          continue;
        }
      }
      out.push(ch);
    }
    return out;
  }

  function texte_de(v) {
    if (v === null || v === undefined || v === RIEN) return "";
    if (v === true) return "oui";
    if (v === false) return "non";
    if (est_nombre(v)) return nombre_texte(v);
    if (typeof v === "string") return v;
    if (Array.isArray(v) || v instanceof Map) return json_compact(v);
    return "<geste>";
  }

  function json_compact(v) {
    const buf = [];
    const écrire = (x) => {
      if (x === null || x === undefined || x === RIEN) {
        buf.push("null");
      } else if (x === true) {
        buf.push("true");
      } else if (x === false) {
        buf.push("false");
      } else if (typeof x === "string") {
        buf.push(JSON.stringify(x));
      } else if (est_nombre(x)) {
        buf.push(nombre_texte(x));
      } else if (Array.isArray(x)) {
        buf.push("[");
        for (let k = 0; k < x.length; k++) {
          if (k) buf.push(",");
          écrire(x[k]);
        }
        buf.push("]");
      } else if (x instanceof Map) {
        buf.push("{");
        let premier = true;
        for (const [k, e] of x) {
          if (!premier) buf.push(",");
          premier = false;
          buf.push(JSON.stringify(String(k)));
          buf.push(":");
          écrire(e);
        }
        buf.push("}");
      } else {
        buf.push("null");
      }
    };
    écrire(v);
    return buf.join("");
  }

  function égal(a, b) {
    if (a === null || a === undefined || a === RIEN) {
      return b === null || b === undefined || b === RIEN;
    }
    if (typeof a === "boolean" || typeof b === "boolean") {
      return typeof a === "boolean" && typeof b === "boolean" && a === b;
    }
    if (est_nombre(a) && est_nombre(b)) return a === b;
    if (typeof a === "string" && typeof b === "string") return a === b;
    if (Array.isArray(a) && Array.isArray(b)) {
      if (a.length !== b.length) return false;
      for (let k = 0; k < a.length; k++) if (!égal(a[k], b[k])) return false;
      return true;
    }
    if (a instanceof Map && b instanceof Map) {
      if (a.size !== b.size) return false;
      for (const [k, v] of a) {
        if (!b.has(k) || !égal(v, b.get(k))) return false;
      }
      return true;
    }
    return a === b;
  }

  function vérité(v) {
    if (v === null || v === undefined || v === RIEN || v === false) return false;
    if (v === true) return true;
    if (est_nombre(v)) return v !== 0n;
    if (typeof v === "string") return v.length > 0;
    if (Array.isArray(v)) return v.length > 0;
    if (v instanceof Map) return v.size > 0;
    return true;
  }

  /* 按码点比较文本（Python 的字符串比较） */
  function comparaison_texte(a, b) {
    if (a === b) return 0;
    const A = Array.from(a);
    const B = Array.from(b);
    const n = Math.min(A.length, B.length);
    for (let i = 0; i < n; i++) {
      const x = A[i].codePointAt(0);
      const y = B[i].codePointAt(0);
      if (x !== y) return x < y ? -1 : 1;
    }
    if (A.length === B.length) return 0;
    return A.length < B.length ? -1 : 1;
  }

  /* ------------------------------------------------------------------ */
  /* 内置（介词式）                                                       */
  /* ------------------------------------------------------------------ */
  function _entier(v) {
    if (!est_nombre(v)) throw new Écart("forme", "indice entier attendu");
    if (v % ECHELLE !== 0n) throw new Écart("forme", "indice entier attendu");
    return v / ECHELLE;
  }

  function _b_mesure(x) {
    if (typeof x === "string") return BigInt(graphemes(x).length) * ECHELLE;
    if (Array.isArray(x)) return BigInt(x.length) * ECHELLE;
    if (x instanceof Map) return BigInt(x.size) * ECHELLE;
    throw new Écart("forme", "mesure attend un texte, une suite ou une fiche");
  }

  function _b_texte(x) {
    return texte_de(x);
  }

  function _b_nombre(x) {
    if (est_nombre(x)) return x;
    if (typeof x === "string") {
      const v = _nombre_depuis_texte(x);
      if (v === ABSENT) throw new Écart("calcul", "en-nombre: « " + x + " » n'est pas un nombre");
      return _bornes(v);
    }
    throw new Écart("calcul", "en-nombre attend un texte");
  }

  function _b_égalise(x) {
    return x;
  }

  function _b_abaissé(t) {
    if (typeof t !== "string") throw new Écart("forme", "abaissé attend un texte");
    return t.toLowerCase();
  }

  function _b_haussé(t) {
    if (typeof t !== "string") throw new Écart("forme", "haussé attend un texte");
    return t.toUpperCase();
  }

  function _b_découpe(t, sep) {
    if (typeof t !== "string") throw new Écart("forme", "découpe attend un texte");
    if (sep === null || sep === undefined) return graphemes(t);
    if (typeof sep !== "string") throw new Écart("forme", "découpe: séparateur texte attendu");
    if (sep === "") return graphemes(t);
    return t.split(sep);
  }

  function _b_recoud(suite, sep) {
    if (!Array.isArray(suite)) throw new Écart("forme", "recoud attend une suite");
    if (typeof sep !== "string") throw new Écart("forme", "recoud: séparateur texte attendu");
    return suite.map(texte_de).join(sep);
  }

  function _b_renferme(x, y) {
    if (typeof x === "string") {
      if (typeof y !== "string") throw new Écart("forme", "renferme: sous-texte attendu");
      return x.indexOf(y) >= 0;
    }
    if (Array.isArray(x)) {
      for (let k = 0; k < x.length; k++) if (égal(x[k], y)) return true;
      return false;
    }
    if (x instanceof Map) return typeof y === "string" && x.has(y);
    throw new Écart("forme", "renferme: valeur non contenante");
  }

  function _b_clefs(f) {
    if (!(f instanceof Map)) throw new Écart("forme", "clefs attend une fiche");
    return Array.from(f.keys());
  }

  function _b_teneurs(f) {
    if (!(f instanceof Map)) throw new Écart("forme", "teneurs attend une fiche");
    return Array.from(f.values());
  }

  function _b_ajoute_fin(suite, x) {
    if (!Array.isArray(suite)) throw new Écart("forme", "ajoute-fin attend une suite");
    return suite.concat([x]);
  }

  function _b_bribe(x, debut, fin) {
    if (!(typeof x === "string" || Array.isArray(x))) {
      throw new Écart("forme", "bribe attend un texte ou une suite");
    }
    const a0 = _entier(debut);
    const b0 = _entier(fin);
    let g;
    let n;
    if (typeof x === "string") {
      g = graphemes(x);
      n = g.length;
    } else {
      g = x;
      n = x.length;
    }
    const nB = BigInt(n);
    const un = 1n;
    const zero = 0n;
    let a = a0 < un ? un : (a0 > nB ? nB : a0);
    let b = b0 < zero ? zero : (b0 > nB ? nB : b0);
    if (a > b) return typeof x === "string" ? "" : [];
    const aa = Number(a);
    const bb = Number(b);
    return typeof x === "string" ? g.slice(aa - 1, bb).join("") : g.slice(aa - 1, bb);
  }

  function _b_ordonne(suite) {
    if (!Array.isArray(suite)) throw new Écart("forme", "ordonne attend une suite");

    const cle = (x) => {
      if (typeof x === "boolean") return [0, x ? 1n : 0n, ""];
      if (est_nombre(x)) return [0, x, ""];
      if (typeof x === "string") return [1, 0n, x];
      return [2, 0n, texte_de(x)];
    };
    const cmp = (x, y) => {
      const a = cle(x);
      const b = cle(y);
      if (a[0] !== b[0]) return a[0] < b[0] ? -1 : 1;
      if (a[1] !== b[1]) return a[1] < b[1] ? -1 : 1;
      return comparaison_texte(a[2], b[2]);
    };
    return suite.slice().sort(cmp);
  }

  function _b_total(suite) {
    if (!Array.isArray(suite)) throw new Écart("forme", "total attend une suite");
    let s = 0n;
    for (let k = 0; k < suite.length; k++) {
      if (!est_nombre(suite[k])) throw new Écart("forme", "total: suite de nombres attendue");
      s += suite[k];
    }
    return _bornes(s);
  }

  function _b_tordu(f, cle, val) {
    if (!(f instanceof Map)) throw new Écart("forme", "tordu attend une fiche");
    if (typeof cle !== "string") throw new Écart("forme", "tordu: clé texte attendue");
    const out = new Map(f);
    out.set(cle, val);
    return out;
  }

  function _b_absolu(x) {
    if (!est_nombre(x)) throw new Écart("forme", "absolu attend un nombre");
    return x < 0n ? -x : x;
  }

  function _b_fige(x) {
    return json_compact(x);
  }

  const NATIFS = {
    "mesure": _b_mesure,
    "en-texte": _b_texte,
    "en-nombre": _b_nombre,
    "égalise": _b_égalise,
    "abaissé": _b_abaissé,
    "haussé": _b_haussé,
    "découpe": _b_découpe,
    "recoud": _b_recoud,
    "renferme": _b_renferme,
    "clefs": _b_clefs,
    "teneurs": _b_teneurs,
    "ajoute-fin": _b_ajoute_fin,
    "bribe": _b_bribe,
    "ordonne": _b_ordonne,
    "total": _b_total,
    "tordu": _b_tordu,
    "fige": _b_fige,
    "absolu": _b_absolu
  };

  const HOSTE = {
    "carnet.lister": "store.list",
    "carnet.compter": "store.count",
    "carnet.lire": "store.get",
    "carnet.écrire": "store.put",
    "carnet.fusionner": "store.patch",
    "carnet.retirer": "store.remove",
    "carnet.vider": "store.clear",
    "horloge.maintenant": "clock.now",
    "voix.dire": "lib.say",
    "voix.signaler": "lib.emit",
    "atelier.évaluer": "lang.eval",
    "atelier.lancer": "lang.run",
    "atelier.noms": "lang.names"
  };

  /* ------------------------------------------------------------------ */
  /* 解释器                                                              */
  /* ------------------------------------------------------------------ */
  class Interpreter {
    constructor(host, sortie) {
      this.host = host || {};
      this.sortie = sortie || ((s) => {
        if (typeof console !== "undefined" && console.log) console.log(s);
      });
      this.global_env = new Portée();
      this.profondeur = 0;
      this.prints = [];
      this.dernier = RIEN;
    }

    /* 内部求值：返回内部值，RIEN 表示「没有结果」 */
    _eval(source, nom) {
      if (typeof source !== "string") throw new Écart("forme", "eval attend un texte");
      const stmts = analyser(source);
      this.global_env = new Portée();
      this.prints = [];
      let dernier = RIEN;
      let résultat = RIEN;
      for (let idx = 0; idx < stmts.length; idx++) {
        const st = stmts[idx];
        let v;
        try {
          v = this.exécuter(st, this.global_env);
        } catch (e) {
          if (e instanceof _Rends) {
            const val = e.valeur === RIEN ? RIEN : e.valeur;
            this.dernier = val;
            return val;
          }
          if (e instanceof _Brise) {
            throw new Écart("calcul", "arrête hors d'une boucle", st.length ? st[st.length - 1] : null);
          }
          if (e instanceof _Passe) {
            throw new Écart("calcul", "passe hors d'une boucle", st.length ? st[st.length - 1] : null);
          }
          throw e;
        }
        if (v !== RIEN) résultat = v;
        dernier = st;
      }
      if (dernier !== RIEN && dernier !== null && dernier[0] === "silence") {
        this.dernier = RIEN;
        return RIEN;
      }
      const r = résultat === RIEN ? RIEN : résultat;
      this.dernier = r;
      return r;
    }

    /* 公开 eval：返回模块结果；没有结果（VIDE）时返回解释器自身（可继续 .call）。 */
    eval(source, nom) {
      const v = this._eval(source, nom);
      if (v === RIEN) return this;
      return v;
    }

    call(nom, args) {
      const f = this.global_env.lire(nom);
      if (f === ABSENT) throw new Écart("nom", "geste inconnu: " + String(nom));
      const entrees = (args || []).map(entrer);
      return sortir(this.invoquer(f, entrees));
    }

    globals() {
      const out = {};
      for (const [k, v] of this.global_env.noms) out[k] = v;
      return out;
    }

    toJSON() {
      return sortir(this.dernier === undefined ? RIEN : this.dernier);
    }

    exécuter(st, portée) {
      const t = st[0];
      if (t === "expr") return this.évaluer(st[1], portée);
      if (t === "lier") {
        portée.définir(st[1], this.évaluer(st[2], portée));
        return RIEN;
      }
      if (t === "relier") {
        const val = this.évaluer(st[2], portée);
        if (!portée.affecter(st[1], val)) {
          throw new Écart("nom", "reliaison d'un nom inconnu: " + st[1], st[3]);
        }
        return RIEN;
      }
      if (t === "afficher") {
        const s = texte_de(this.évaluer(st[1], portée));
        this.prints.push(s);
        this.sortie(s);
        return RIEN;
      }
      if (t === "rends") throw new _Rends(this.évaluer(st[1], portée));
      if (t === "silence") return RIEN;
      if (t === "si") {
        for (let k = 0; k < st[1].length; k++) {
          const cond = st[1][k][0];
          const corps = st[1][k][1];
          if (vérité(this.évaluer(cond, portée))) {
            return this.exécuter_bloc(corps, new Portée(portée));
          }
        }
        if (st[2] !== null) return this.exécuter_bloc(st[2], new Portée(portée));
        return RIEN;
      }
      if (t === "tantque") {
        let tours = 0;
        while (vérité(this.évaluer(st[1], portée))) {
          tours += 1;
          if (tours > 2000000) throw new Écart("calcul", "boucle trop longue", st[3]);
          try {
            this.exécuter_bloc(st[2], new Portée(portée));
          } catch (e) {
            if (e instanceof _Brise) break;
            if (e instanceof _Passe) continue;
            throw e;
          }
        }
        return RIEN;
      }
      if (t === "pourchaque") {
        let seq = this.évaluer(st[2], portée);
        if (typeof seq === "string") seq = graphemes(seq);
        if (!Array.isArray(seq)) throw new Écart("forme", "pour chaque attend une suite", st[4]);
        for (let k = 0; k < seq.length; k++) {
          const p = new Portée(portée);
          p.définir(st[1], seq[k]);
          try {
            this.exécuter_bloc(st[3], p);
          } catch (e) {
            if (e instanceof _Brise) break;
            if (e instanceof _Passe) continue;
            throw e;
          }
        }
        return RIEN;
      }
      if (t === "definir") {
        portée.définir(st[1], new Geste(st[1], st[2], st[3], portée));
        return RIEN;
      }
      if (t === "brise") throw new _Brise();
      if (t === "passe") throw new _Passe();
      throw new Écart("calcul", "instruction inconnue: " + String(t));
    }

    exécuter_bloc(stmts, portée) {
      for (let k = 0; k < stmts.length; k++) this.exécuter(stmts[k], portée);
    }

    évaluer(e, portée) {
      const t = e[0];
      if (t === "nombre") return e[1];
      if (t === "texte") return e[1];
      if (t === "oui") return true;
      if (t === "non") return false;
      if (t === "vide") return null;
      if (t === "nom") {
        const nom = e[1];
        const v = portée.lire(nom);
        if (v !== ABSENT) return v;
        if (Object.prototype.hasOwnProperty.call(NATIFS, nom)) {
          return new Natif(nom, (args) => NATIFS[nom](...args));
        }
        if (Object.prototype.hasOwnProperty.call(this.host, nom)) {
          return new Natif(nom, (args) => this._hôte(nom, args));
        }
        throw new Écart("nom", "nom inconnu: " + nom);
      }
      if (t === "suite") {
        const out = [];
        for (let k = 0; k < e[1].length; k++) out.push(this.évaluer(e[1][k], portée));
        return out;
      }
      if (t === "fiche") {
        const out = new Map();
        for (let k = 0; k < e[1].length; k++) {
          out.set(e[1][k][0], this.évaluer(e[1][k][1], portée));
        }
        return out;
      }
      if (t === "element") {
        const coll = this.évaluer(e[2], portée);
        return this._indexer(coll, this.évaluer(e[1], portée));
      }
      if (t === "champ-de") {
        const f = this.évaluer(e[2], portée);
        const cle = this.évaluer(e[1], portée);
        if (!(f instanceof Map)) throw new Écart("forme", "champ de: fiche attendue");
        if (typeof cle !== "string") throw new Écart("forme", "champ de: clé texte attendue");
        if (!f.has(cle)) throw new Écart("forme", "champ absent: " + cle);
        return f.get(cle);
      }
      if (t === "un") {
        const v = this.évaluer(e[2], portée);
        if (e[1] === "non") return !vérité(v);
        if (!est_nombre(v)) throw new Écart("forme", "moins d'un non-nombre");
        return -v;
      }
      if (t === "et") {
        if (!vérité(this.évaluer(e[1], portée))) return false;
        return vérité(this.évaluer(e[2], portée));
      }
      if (t === "ou") {
        if (vérité(this.évaluer(e[1], portée))) return true;
        return vérité(this.évaluer(e[2], portée));
      }
      if (t === "bin") {
        return this._binaire(e[1], this.évaluer(e[2], portée), this.évaluer(e[3], portée));
      }
      if (t === "cmp") {
        return this._comparer(e[1], this.évaluer(e[2], portée), this.évaluer(e[3], portée));
      }
      if (t === "appel") {
        const nom = e[1];
        const args = [];
        for (let k = 0; k < e[2].length; k++) args.push(this.évaluer(e[2][k], portée));
        const f = portée.lire(nom);
        if (f === ABSENT) {
          if (Object.prototype.hasOwnProperty.call(NATIFS, nom)) {
            return this.invoquer(new Natif(nom, (a) => NATIFS[nom](...a)), args);
          }
          if (Object.prototype.hasOwnProperty.call(this.host, nom)) {
            return this.invoquer(new Natif(nom, (a) => this._hôte(nom, a)), args);
          }
          throw new Écart("nom", "nom inconnu: " + nom);
        }
        return this.invoquer(f, args);
      }
      throw new Écart("calcul", "expression inconnue: " + String(t));
    }

    _hôte(nom, args) {
      const fn = this.host[nom];
      if (typeof fn !== "function") throw new Écart("hôte", "host-not-found: " + nom);
      try {
        return entrer(fn(...args.map(sortir)));
      } catch (e) {
        if (e instanceof Écart) throw e;
        throw new Écart("hôte", nom + ": " + message_de(e));
      }
    }

    _indexer(coll, idx) {
      const i = _entier(idx);
      if (Array.isArray(coll)) {
        if (i < 1n || i > BigInt(coll.length)) throw new Écart("calcul", "indice hors limites");
        return coll[Number(i) - 1];
      }
      if (typeof coll === "string") {
        const g = graphemes(coll);
        if (i < 1n || i > BigInt(g.length)) throw new Écart("calcul", "indice hors limites");
        return g[Number(i) - 1];
      }
      throw new Écart("forme", "indexation impossible");
    }

    _binaire(op, a, b) {
      if (op === "+") {
        if (est_nombre(a) && est_nombre(b)) return _bornes(a + b);
        if (typeof a === "string" && typeof b === "string") return a + b;
        if (Array.isArray(a) && Array.isArray(b)) return a.concat(b);
        throw new Écart("forme", "addition impossible sur ces valeurs");
      }
      if (op === "-") {
        if (!(est_nombre(a) && est_nombre(b))) throw new Écart("forme", "soustraction de non-nombres");
        return _bornes(a - b);
      }
      if (op === "*") {
        if (!(est_nombre(a) && est_nombre(b))) throw new Écart("forme", "multiplication de non-nombres");
        return _bornes((a * b) / ECHELLE);
      }
      if (op === "/") {
        if (!(est_nombre(a) && est_nombre(b))) throw new Écart("forme", "division de non-nombres");
        if (b === 0n) throw new Écart("calcul", "division par zéro");
        return _bornes(div_plancher(a * ECHELLE, b));
      }
      if (op === "%") {
        if (!(est_nombre(a) && est_nombre(b))) throw new Écart("forme", "modulo de non-nombres");
        if (b === 0n) throw new Écart("calcul", "division par zéro");
        const q = quotient_tronque(a, b);
        return _bornes(a - b * q);
      }
      if (op === "**") {
        if (!(est_nombre(a) && est_nombre(b))) throw new Écart("forme", "puissance de non-nombres");
        if (b % ECHELLE !== 0n) throw new Écart("forme", "exposant entier attendu");
        const e = b / ECHELLE;
        if (e > 1000n || e < -1000n) throw new Écart("calcul", "débordement");
        let r;
        try {
          r = Math.pow(division_double(a, ECHELLE), Number(e));
        } catch (err) {
          throw new Écart("calcul", "débordement");
        }
        if (!isFinite(r)) throw new Écart("calcul", "débordement");
        const mise = r * ECHELLE_N;
        if (!isFinite(mise)) throw new Écart("calcul", "débordement");
        return _bornes(BigInt(arrondi_py(mise)));
      }
      throw new Écart("calcul", "opérateur inconnu: " + op);
    }

    _comparer(op, a, b) {
      if (op === "==") return égal(a, b);
      if (op === "!=") return !égal(a, b);
      let x;
      let y;
      if (est_nombre(a) && est_nombre(b)) {
        x = a;
        y = b;
      } else if (typeof a === "string" && typeof b === "string") {
        x = a;
        y = b;
      } else {
        throw new Écart("forme", "comparaison entre types incompatibles");
      }
      let c;
      if (typeof x === "string") c = comparaison_texte(x, y);
      else c = x < y ? -1 : (x > y ? 1 : 0);
      if (op === ">") return c > 0;
      if (op === "<") return c < 0;
      if (op === ">=") return c >= 0;
      if (op === "<=") return c <= 0;
      throw new Écart("calcul", "comparateur inconnu: " + op);
    }

    invoquer(f, args) {
      if (f instanceof Natif) {
        try {
          return f.appel(args);
        } catch (e) {
          if (e instanceof Écart) throw e;
          throw new Écart("hôte", f.nom + ": " + message_de(e));
        }
      }
      if (f instanceof Geste) {
        if (args.length !== f.params.length) {
          throw new Écart("forme", f.nom + " attend " + f.params.length + " argument(s), reçu " + args.length);
        }
        this.profondeur += 1;
        if (this.profondeur > 400) {
          this.profondeur -= 1;
          throw new Écart("calcul", "pile trop profonde");
        }
        const p = new Portée(f.portée);
        for (let k = 0; k < f.params.length; k++) p.définir(f.params[k], args[k]);
        try {
          this.exécuter_bloc(f.corps, p);
          return null;
        } catch (e) {
          if (e instanceof _Rends) return e.valeur;
          throw e;
        } finally {
          this.profondeur -= 1;
        }
      }
      throw new Écart("forme", "valeur non appelable");
    }
  }

  /* Python 的 //（向下取整） */
  function div_plancher(a, b) {
    const q = a / b;
    if (a % b !== 0n && (a < 0n) !== (b < 0n)) return q - 1n;
    return q;
  }

  /* Python 的 int(a / b)：a / b 是「精确商的正确舍入 double」，再向零取整。
     用 BigInt 取 200 位商再缩放到 double，避免 Number(a) 先丢精度。 */
  function division_double(a, b) {
    if (a === 0n) return 0;
    const negatif = (a < 0n) !== (b < 0n);
    const A = a < 0n ? -a : a;
    const B = b < 0n ? -b : b;
    const bitsA = A.toString(2).length;
    const bitsB = B.toString(2).length;
    const k = 200 + bitsB - bitsA;
    let num;
    let den;
    if (k >= 0) {
      num = A << BigInt(k);
      den = B;
    } else {
      num = A;
      den = B << BigInt(-k);
    }
    const Q = num / den;
    const d = Number(Q) / Math.pow(2, k);
    return negatif ? -d : d;
  }

  /* Python 的 int(a / b)（向零取整的商） */
  function quotient_tronque(a, b) {
    const d = division_double(a, b);
    if (!isFinite(d)) throw new Écart("calcul", "débordement");
    return BigInt(Math.trunc(d));
  }

  /* ------------------------------------------------------------------ */
  /* 宿主边界编组                                                        */
  /* ------------------------------------------------------------------ */
  /* 外部 JSON -> 语言值：数字变定点 Nombre。 */
  function entrer(v) {
    if (v === undefined || v === null) return null;
    if (typeof v === "boolean") return v;
    if (typeof v === "number") {
      if (Number.isInteger(v)) return BigInt(v) * ECHELLE;
      return _bornes(BigInt(arrondi_py(v * ECHELLE_N)));
    }
    if (typeof v === "bigint") return v * ECHELLE;
    if (typeof v === "string") return v;
    if (Array.isArray(v)) return v.map(entrer);
    if (v instanceof Map) {
      const out = new Map();
      for (const [k, x] of v) out.set(String(k), entrer(x));
      return out;
    }
    if (typeof v === "object") {
      const out = new Map();
      const cles = Object.keys(v);
      for (let k = 0; k < cles.length; k++) out.set(cles[k], entrer(v[cles[k]]));
      return out;
    }
    return v;
  }

  /* 语言值 -> 外部 JSON。 */
  function sortir(v) {
    if (v === null || v === undefined || v === RIEN) return null;
    if (typeof v === "boolean" || typeof v === "string") return v;
    if (est_nombre(v)) {
      const t = nombre_texte(v);
      if (t.indexOf(".") < 0) {
        const b = BigInt(t);
        if (b >= -MAX_SUR && b <= MAX_SUR) return Number(t);
        return t; /* 无法用 Number 精确表示时给字符串 */
      }
      return Number(t);
    }
    if (Array.isArray(v)) return v.map(sortir);
    if (v instanceof Map) {
      const out = {};
      for (const [k, x] of v) out[String(k)] = sortir(x);
      return out;
    }
    return null;
  }

  function create(host, sortie) {
    return new Interpreter(host, sortie);
  }

  /* ------------------------------------------------------------------ */
  /* 导出                                                                */
  /* ------------------------------------------------------------------ */
  const SILLAGE = {
    LANG: LANG,
    VERSION: VERSION,
    FILE_EXT: FILE_EXT,
    ENTREE: ENTREE,
    ERREUR: ERREUR,
    Ecart: Écart,
    "Écart": Écart,
    "Défaut": Écart,
    HOSTE: HOSTE,
    create: create,
    /* 便于宿主与等价性比对 */
    entrer: entrer,
    sortir: sortir,
    interne: entrer,
    externe: sortir,
    json: json_compact,
    texte: texte_de,
    egal: égal,
    verite: vérité,
    lexer: lexer,
    analyser: analyser,
    graphemes: graphemes,
    nombreTexte: nombre_texte,
    ECHELLE: ECHELLE,
    NOTE_MAX: NOTE_MAX
  };

  global.SILLAGE = SILLAGE;
  if (typeof module !== "undefined" && module && module.exports) module.exports = SILLAGE;
})(typeof globalThis !== "undefined" ? globalThis : this);
