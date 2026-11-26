/* ===========================================================================
 * sillage_c.c — SILLAGE 3.0 引擎（C11，单文件，仅 libc + Windows CRT）
 *
 * 语义以 sillage/sillage.py 与 sillage/README.md 为准：
 *   值和定点数、字素簇文本、不可变序列、保序记录、18 个内置、
 *   Défaut{étape,ligne,note}、模块结果三条规则 + silence、
 *   循环 200 万次 / 递归 400 层护栏。
 * 原生服务（取代 Python 宿主注入）：
 *   carnet.*（本地 JSON 存储）、horloge.maintenant、voix.*、atelier.*
 *
 * CLI: sillage_c.exe test | run <f> [JSON] | call <f> <fn> [JSON] | ast <f>
 *      sillage_c.exe --server --store <zici.json>
 * ======================================================================== */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <time.h>
#include <math.h>
#include <wchar.h>
#include <locale.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <windows.h>

typedef long long i64;
typedef __int128 i128;
typedef unsigned __int128 u128;

#define ECHELLE 1000000000LL
#define NOTE_MAX_I128 ((i128)1000000000000000LL * (i128)ECHELLE)

/* ------------------------------------------------------------------ */
/* 分配：本引擎不释放内存（进程级解释器；单文件实现，规模可控）。        */
/* ------------------------------------------------------------------ */
static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("sillage_c: out of memory\n", stderr);
        exit(70);
    }
    return p;
}
static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("sillage_c: out of memory\n", stderr);
        exit(70);
    }
    return q;
}
static void *xcalloc(size_t n, size_t m)
{
    void *p = xmalloc(n * m);
    memset(p, 0, n * m);
    return p;
}
static char *xstrndup(const char *s, int n)
{
    char *p = (char *)xmalloc((size_t)n + 1);
    memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}
static char *xstrdup_c(const char *s)
{
    return xstrndup(s, (int)strlen(s));
}

/* ------------------------------------------------------------------ */
/* 动态缓冲                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char *p;
    int len, cap;
} Buf;

static void buf_init(Buf *b)
{
    b->p = NULL;
    b->len = b->cap = 0;
}
static void buf_need(Buf *b, int extra)
{
    if (b->len + extra + 1 > b->cap) {
        int nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + extra + 1)
            nc *= 2;
        b->p = (char *)xrealloc(b->p, (size_t)nc);
        b->cap = nc;
    }
}
static void buf_putn(Buf *b, const char *s, int n)
{
    if (n <= 0)
        return;
    buf_need(b, n);
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
    b->p[b->len] = 0;
}
static void buf_puts(Buf *b, const char *s)
{
    buf_putn(b, s, (int)strlen(s));
}
static void buf_putc(Buf *b, char c)
{
    buf_need(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}
static void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    int need;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        return;
    }
    buf_need(b, need);
    vsnprintf(b->p + b->len, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    b->len += need;
}
static char *buf_take(Buf *b)
{
    if (!b->p)
        return xstrdup_c("");
    return b->p;
}
static const char *buf_c(Buf *b)
{
    if (!b->p)
        return "";
    return b->p;
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */
static int u8_decode(const unsigned char *s, int len, int *cp)
{
    unsigned char c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && len >= 2) {
        *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && len >= 3) {
        *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && len >= 4) {
        *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}
static int u8_encode(int cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
static void buf_putcp(Buf *b, int cp)
{
    char t[4];
    int n = u8_encode(cp, t);
    buf_putn(b, t, n);
}

/* Python str.isalpha 的近似：拉丁字母、希腊/西里尔、CJK、假名、谚文 */
static int cp_isalpha(int cp)
{
    if (cp >= 'a' && cp <= 'z')
        return 1;
    if (cp >= 'A' && cp <= 'Z')
        return 1;
    if (cp == '_')
        return 1;
    if (cp >= 0x00C0 && cp <= 0x00FF && cp != 0x00D7 && cp != 0x00F7)
        return 1;
    if (cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA)
        return 1;
    if (cp >= 0x0100 && cp <= 0x02AF)
        return 1;
    if (cp >= 0x0370 && cp <= 0x03FF && cp != 0x037E && cp != 0x0387)
        return 1;
    if (cp >= 0x0400 && cp <= 0x052F)
        return 1;
    if (cp >= 0x0531 && cp <= 0x0588)
        return 1;
    if (cp >= 0x05D0 && cp <= 0x05EA)
        return 1;
    if (cp >= 0x0620 && cp <= 0x064A)
        return 1;
    if (cp >= 0x0900 && cp <= 0x097F)
        return 1;
    if (cp >= 0x0E00 && cp <= 0x0E7F)
        return 1;
    if (cp >= 0x1E00 && cp <= 0x1EFF)
        return 1;
    if (cp >= 0x1F00 && cp <= 0x1FFF)
        return 1;
    if (cp >= 0x2C60 && cp <= 0x2C7F)
        return 1;
    if (cp >= 0x3040 && cp <= 0x30FF)
        return 1;
    if (cp >= 0x3400 && cp <= 0x4DBF)
        return 1;
    if (cp >= 0x4E00 && cp <= 0x9FFF)
        return 1;
    if (cp >= 0xA000 && cp <= 0xA48F)
        return 1;
    if (cp >= 0xAC00 && cp <= 0xD7A3)
        return 1;
    if (cp >= 0xF900 && cp <= 0xFAFF)
        return 1;
    if (cp >= 0xFF21 && cp <= 0xFF3A)
        return 1;
    if (cp >= 0xFF41 && cp <= 0xFF5A)
        return 1;
    if (cp >= 0x20000 && cp <= 0x2FA1F)
        return 1;
    return 0;
}
static int cp_isalnum(int cp)
{
    if (cp >= '0' && cp <= '9')
        return 1;
    return cp_isalpha(cp);
}
static int cp_is_combining(int cp)
{
    if (cp >= 0x0300 && cp <= 0x036F)
        return 1;
    if (cp >= 0x1AB0 && cp <= 0x1AFF)
        return 1;
    if (cp >= 0x20D0 && cp <= 0x20FF)
        return 1;
    if (cp >= 0xFE20 && cp <= 0xFE2F)
        return 1;
    if (cp == 0xFE0F || cp == 0x200D)
        return 1;
    if (cp >= 0x1F3FB && cp <= 0x1F3FF)
        return 1;
    return 0;
}

/* 字素簇：返回数量和每簇 (start,len) */
typedef struct {
    int start, len;
} Grapheme;

static int graphemes(const char *s, int len, Grapheme **out)
{
    int cap = 16, n = 0, i = 0;
    Grapheme *g = (Grapheme *)xmalloc(sizeof(Grapheme) * (size_t)cap);
    while (i < len) {
        int cp;
        int k = u8_decode((const unsigned char *)s + i, len - i, &cp);
        if (n > 0 && cp_is_combining(cp)) {
            g[n - 1].len += k;
        } else {
            if (n == cap) {
                cap *= 2;
                g = (Grapheme *)xrealloc(g, sizeof(Grapheme) * (size_t)cap);
            }
            g[n].start = i;
            g[n].len = k;
            n++;
        }
        i += k;
    }
    *out = g;
    return n;
}

/* ------------------------------------------------------------------ */
/* 定点数                                                              */
/* ------------------------------------------------------------------ */
static i128 i128_abs(i128 v) { return v < 0 ? -v : v; }

static i128 floor_div(i128 a, i128 b)
{
    i128 q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        q--;
    return q;
}
static i128 floor_mod(i128 a, i128 b)
{
    i128 r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        r += b;
    return r;
}


static int u128_digits(u128 u, char *out)
{
    char tmp[48];
    int n = 0, i;
    if (u == 0) {
        out[0] = '0';
        return 1;
    }
    while (u != 0) {
        tmp[n++] = (char)('0' + (int)(unsigned)(u % 10));
        u /= 10;
    }
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* 最短十进制：整值不带小数点，小数去尾零 */
static void nombre_texte(i128 n, Buf *b)
{
    char tmp[48], f[16];
    int l, fl;
    u128 u, ip, fr;
    if (n == 0) {
        buf_putc(b, '0');
        return;
    }
    if (n < 0) {
        buf_putc(b, '-');
        u = (u128)(-(n + 1)) + 1;
    } else {
        u = (u128)n;
    }
    ip = u / (u128)ECHELLE;
    fr = u % (u128)ECHELLE;
    l = u128_digits(ip, tmp);
    buf_putn(b, tmp, l);
    if (fr != 0) {
        snprintf(f, sizeof(f), "%09llu", (unsigned long long)fr);
        fl = 9;
        while (fl > 1 && f[fl - 1] == '0')
            fl--;
        buf_putc(b, '.');
        buf_putn(b, f, fl);
    }
}

/* Python float repr 近似（_sortir 之后 json.dumps 用到） */
static void nombre_float_repr(i128 n, Buf *b)
{
    u128 u, ip, fr;
    char tmp[48], f[16];
    int l, fl, i, first, e;
    char digs[64];
    int nd = 0;
    if (n == 0) {
        buf_putc(b, '0');
        return;
    }
    if (n < 0) {
        buf_putc(b, '-');
        u = (u128)(-(n + 1)) + 1;
    } else {
        u = (u128)n;
    }
    ip = u / (u128)ECHELLE;
    fr = u % (u128)ECHELLE;
    if (fr == 0) {
        l = u128_digits(ip, tmp);
        buf_putn(b, tmp, l);
        return;
    }
    l = u128_digits(ip, tmp);
    snprintf(f, sizeof(f), "%09llu", (unsigned long long)fr);
    fl = 9;
    while (fl > 1 && f[fl - 1] == '0')
        fl--;
    /* 指数：intpart 非零时 = 位数-1；否则看小数前导零 */
    if (ip != 0) {
        e = l - 1;
    } else {
        e = -1;
        for (i = 0; i < fl; i++) {
            if (f[i] != '0')
                break;
            e--;
        }
    }
    if (e < -4) {
        for (i = 0; i < l; i++)
            digs[nd++] = tmp[i];
        for (i = 0; i < fl; i++)
            digs[nd++] = f[i];
        first = 0;
        while (first < nd && digs[first] == '0')
            first++;
        while (nd > first + 1 && digs[nd - 1] == '0')
            nd--;
        buf_putc(b, digs[first]);
        if (nd - first - 1 > 0) {
            buf_putc(b, '.');
            buf_putn(b, digs + first + 1, nd - first - 1);
        }
        buf_printf(b, "e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else {
        buf_putn(b, tmp, l);
        buf_putc(b, '.');
        buf_putn(b, f, fl);
    }
}

/* 文本 -> 定点（与 Python _nombre_depuis_texte 等价）；返回 0 表示失败 */
static int nombre_depuis_texte(const char *p, int len, i128 *out)
{
    Buf b;
    char *s;
    int i, j, n, signe = 1, dotpos = -1, fracstart, nfrac, intdigits = 0;
    i128 base = 0;
    buf_init(&b);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
            continue;
        if (c == ',' ) {
            buf_putc(&b, '.');
            continue;
        }
        if (c == 0xC2 && i + 1 < len && (unsigned char)p[i + 1] == 0xA0) {
            i++;
            continue;
        }
        if (c == 0xE3 && i + 2 < len && (unsigned char)p[i + 1] == 0x80 && (unsigned char)p[i + 2] == 0x80) {
            i += 2;
            continue;
        }
        buf_putc(&b, (char)c);
    }
    s = buf_take(&b);
    n = b.len;
    if (n == 0)
        return 0;
    i = 0;
    if (s[0] == '+' || s[0] == '-') {
        if (s[0] == '-')
            signe = -1;
        i++;
    }
    if (i >= n)
        return 0;
    fracstart = n;
    for (j = i; j < n; j++) {
        if (s[j] == '.') {
            if (dotpos >= 0)
                return 0;
            dotpos = j;
            fracstart = j + 1;
        } else if (s[j] >= '0' && s[j] <= '9') {
            if (dotpos < 0)
                intdigits++;
        } else {
            return 0;
        }
    }
    nfrac = n - fracstart;
    if (dotpos < 0)
        nfrac = 0;
    if (intdigits == 0 && nfrac == 0)
        return 0;
    /* 整数部分（超过 1e15 一定越界；用哨兵让调用方报 calcul） */
    {
        int gros = 0;
        for (j = i; j < (dotpos < 0 ? n : dotpos); j++) {
            if (!gros) {
                base = base * 10 + (s[j] - '0');
                if (base > (i128)1000000000000000LL)
                    gros = 1;
            }
        }
        if (gros)
            base = (i128)1000000000000000LL + 1;
    }
    base *= (i128)ECHELLE;
    if (nfrac > 0) {
        int take = nfrac > 9 ? 9 : nfrac;
        i128 fr = 0;
        for (j = 0; j < take; j++)
            fr = fr * 10 + (s[fracstart + j] - '0');
        for (j = take; j < 9; j++)
            fr *= 10;
        base += fr;
        if (nfrac > 9 && s[fracstart + 9] >= '5')
            base += 1;
    }
    *out = signe < 0 ? -base : base;
    return 1;
}

/* JSON 数字（可带指数）-> 定点 */
static int json_num_to_fixed(const char *s, int len, i128 *out)
{
    int i, epos = -1;
    long expo = 0;
    i128 v;
    for (i = 0; i < len; i++) {
        if (s[i] == 'e' || s[i] == 'E') {
            epos = i;
            break;
        }
    }
    if (epos < 0)
        return nombre_depuis_texte(s, len, out);
    if (!nombre_depuis_texte(s, epos, &v))
        return 0;
    i = epos + 1;
    {
        int esign = 1;
        if (i < len && (s[i] == '+' || s[i] == '-')) {
            if (s[i] == '-')
                esign = -1;
            i++;
        }
        if (i >= len)
            return 0;
        for (; i < len; i++) {
            if (s[i] < '0' || s[i] > '9')
                return 0;
            if (expo < 100000)
                expo = expo * 10 + (s[i] - '0');
        }
        expo *= esign;
    }
    if (expo == 0) {
        *out = v;
        return 1;
    }
    if (expo > 0) {
        while (expo-- > 0) {
            if (i128_abs(v) > (i128)1000000000000000000LL)
                return 0;
            v *= 10;
        }
        *out = v;
        return 1;
    }
    expo = -expo;
    while (expo-- > 0) {
        i128 q = v / 10;
        i128 r = v % 10;
        if (r < 0)
            r = -r;
        v = q + (r >= 5 ? (v < 0 ? -1 : 1) : 0);
    }
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 错误 Défaut                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *etape;
    char *note;
    int has_ligne;
    int ligne;
} Defaut;

static Defaut g_err;
static jmp_buf g_jmp;
static int g_jmp_live = 0;

static void defaut_set(const char *etape, int ligne, const char *note)
{
    g_err.etape = etape;
    g_err.note = xstrdup_c(note);
    if (ligne >= 0) {
        g_err.has_ligne = 1;
        g_err.ligne = ligne;
    } else {
        g_err.has_ligne = 0;
        g_err.ligne = 0;
    }
}
#if defined(__GNUC__)
__attribute__((noreturn, format(printf, 3, 4)))
#endif
static void defaut(const char *etape, int ligne, const char *fmt, ...)
{
    Buf b;
    va_list ap, ap2;
    int need;
    buf_init(&b);
    va_start(ap, fmt);
    va_copy(ap2, ap);
    need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need >= 0) {
        buf_need(&b, need);
        vsnprintf(b.p + b.len, (size_t)need + 1, fmt, ap2);
        b.len += need;
    }
    va_end(ap2);
    defaut_set(etape, ligne, buf_c(&b));
    if (g_jmp_live)
        longjmp(g_jmp, 1);
    fflush(stdout);
    fprintf(stderr, "sillage_c: internal error without handler: %s\n", buf_c(&b));
    exit(70);
}

/* ------------------------------------------------------------------ */
/* 值                                                                  */
/* ------------------------------------------------------------------ */
enum { V_VIDE, V_OUI, V_NON, V_NOMBRE, V_TEXTE, V_SUITE, V_FICHE, V_GESTE, V_NATIF };

typedef struct V V;
typedef struct Geste Geste;
typedef struct Natif Natif;
typedef struct Scope Scope;
typedef struct Node Node;

struct Geste {
    char *nom;
    int nomlen;
    char **params;
    int *plens;
    int nparams;
    Node *corps;
    int ncorps;
    Scope *portee;
};

struct Natif {
    const char *nom;
    V *(*fn)(V **args, int n);
    int nargs; /* -1 表示可变 */
    const char *pyname;
};

struct V {
    int t;
    union {
        i128 n;
        struct {
            char *p;
            int len;
        } s;
        struct {
            V **it;
            int n;
        } su;
        struct {
            V **k;
            V **v;
            int n;
        } fi;
        Geste *g;
        Natif *nat;
    } u;
};

static V V_VIDE_S = {V_VIDE, {0}};
static V V_OUI_S = {V_OUI, {0}};
static V V_NON_S = {V_NON, {0}};

static V *v_vide(void) { return &V_VIDE_S; }
static V *v_oui(void) { return &V_OUI_S; }
static V *v_non(void) { return &V_NON_S; }

static V *v_new(void)
{
    V *v = (V *)xcalloc(1, sizeof(V));
    return v;
}
static V *v_nombre(i128 n)
{
    V *v = v_new();
    v->t = V_NOMBRE;
    v->u.n = n;
    return v;
}
static V *v_texte_n(const char *p, int len)
{
    V *v = v_new();
    v->t = V_TEXTE;
    v->u.s.p = xstrndup(p ? p : "", len);
    v->u.s.len = len;
    return v;
}
static V *v_texte_c(const char *c) { return v_texte_n(c, (int)strlen(c)); }
static V *v_suite(V **it, int n)
{
    V *v = v_new();
    v->t = V_SUITE;
    v->u.su.it = it;
    v->u.su.n = n;
    return v;
}
static V *v_suite_copie(V **it, int n)
{
    V **copy = (V **)xmalloc(sizeof(V *) * (size_t)(n > 0 ? n : 1));
    if (n > 0)
        memcpy(copy, it, sizeof(V *) * (size_t)n);
    return v_suite(copy, n);
}
static V *v_fiche(void)
{
    V *v = v_new();
    v->t = V_FICHE;
    v->u.fi.k = NULL;
    v->u.fi.v = NULL;
    v->u.fi.n = 0;
    return v;
}
static int texte_egal(V *a, V *b)
{
    return a->u.s.len == b->u.s.len && memcmp(a->u.s.p, b->u.s.p, (size_t)a->u.s.len) == 0;
}
static int fiche_index(V *f, const char *key, int klen)
{
    int i;
    for (i = 0; i < f->u.fi.n; i++) {
        V *k = f->u.fi.k[i];
        if (k->u.s.len == klen && memcmp(k->u.s.p, key, (size_t)klen) == 0)
            return i;
    }
    return -1;
}
/* 就地设值（构造期用）：已有键保位置，否则追加 */
static void fiche_set(V *f, const char *key, int klen, V *val)
{
    int i = fiche_index(f, key, klen);
    if (i >= 0) {
        f->u.fi.v[i] = val;
        return;
    }
    f->u.fi.k = (V **)xrealloc(f->u.fi.k, sizeof(V *) * (size_t)(f->u.fi.n + 1));
    f->u.fi.v = (V **)xrealloc(f->u.fi.v, sizeof(V *) * (size_t)(f->u.fi.n + 1));
    f->u.fi.k[f->u.fi.n] = v_texte_n(key, klen);
    f->u.fi.v[f->u.fi.n] = val;
    f->u.fi.n++;
}
static V *v_fiche_copie(V *f)
{
    V *o = v_fiche();
    int i;
    if (f->u.fi.n > 0) {
        o->u.fi.k = (V **)xmalloc(sizeof(V *) * (size_t)f->u.fi.n);
        o->u.fi.v = (V **)xmalloc(sizeof(V *) * (size_t)f->u.fi.n);
        o->u.fi.n = f->u.fi.n;
        for (i = 0; i < f->u.fi.n; i++) {
            o->u.fi.k[i] = f->u.fi.k[i];
            o->u.fi.v[i] = f->u.fi.v[i];
        }
    }
    return o;
}

static int est_nombre(V *v) { return v && v->t == V_NOMBRE; }
static int est_texte(V *v) { return v && v->t == V_TEXTE; }

/* 相等（Python égal） */
static int egal(V *a, V *b)
{
    int i;
    if (!a || !b)
        return a == b;
    if (a->t == V_VIDE || b->t == V_VIDE)
        return a->t == V_VIDE && b->t == V_VIDE;
    if (a->t == V_OUI || a->t == V_NON || b->t == V_OUI || b->t == V_NON)
        return a->t == b->t && a->t != V_VIDE;
    if (a->t == V_NOMBRE && b->t == V_NOMBRE)
        return a->u.n == b->u.n;
    if (a->t == V_TEXTE && b->t == V_TEXTE)
        return texte_egal(a, b);
    if (a->t == V_SUITE && b->t == V_SUITE) {
        if (a->u.su.n != b->u.su.n)
            return 0;
        for (i = 0; i < a->u.su.n; i++)
            if (!egal(a->u.su.it[i], b->u.su.it[i]))
                return 0;
        return 1;
    }
    if (a->t == V_FICHE && b->t == V_FICHE) {
        if (a->u.fi.n != b->u.fi.n)
            return 0;
        for (i = 0; i < a->u.fi.n; i++) {
            V *k = a->u.fi.k[i];
            int idx = fiche_index(b, k->u.s.p, k->u.s.len);
            if (idx < 0)
                return 0;
            if (!egal(a->u.fi.v[i], b->u.fi.v[idx]))
                return 0;
        }
        return 1;
    }
    return a == b;
}

/* 真假（Python vérité） */
static int verite(V *v)
{
    if (!v)
        return 0;
    switch (v->t) {
    case V_VIDE:
    case V_NON:
        return 0;
    case V_OUI:
        return 1;
    case V_NOMBRE:
        return v->u.n != 0;
    case V_TEXTE:
        return v->u.s.len > 0;
    case V_SUITE:
        return v->u.su.n > 0;
    case V_FICHE:
        return v->u.fi.n > 0;
    default:
        return 1;
    }
}

/* JSON 序列化（内部形态：数字用最短十进制） */
static void json_escape_into(Buf *b, const char *p, int len)
{
    int i;
    buf_putc(b, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        switch (c) {
        case '"':
            buf_puts(b, "\\\"");
            break;
        case '\\':
            buf_puts(b, "\\\\");
            break;
        case '\n':
            buf_puts(b, "\\n");
            break;
        case '\t':
            buf_puts(b, "\\t");
            break;
        case '\r':
            buf_puts(b, "\\r");
            break;
        case '\b':
            buf_puts(b, "\\b");
            break;
        case '\f':
            buf_puts(b, "\\f");
            break;
        default:
            if (c < 0x20)
                buf_printf(b, "\\u%04x", (int)c);
            else
                buf_putc(b, (char)c);
        }
    }
    buf_putc(b, '"');
}

/* indent < 0 紧凑；>=0 用 Python json.dumps(indent=n) 风格 */
static void json_dump(V *v, Buf *b, int indent, int prof, int externe)
{
    int i;
    if (!v) {
        buf_puts(b, "null");
        return;
    }
    switch (v->t) {
    case V_VIDE:
        buf_puts(b, "null");
        return;
    case V_OUI:
        buf_puts(b, "true");
        return;
    case V_NON:
        buf_puts(b, "false");
        return;
    case V_NOMBRE:
        if (externe)
            nombre_float_repr(v->u.n, b);
        else
            nombre_texte(v->u.n, b);
        return;
    case V_TEXTE:
        json_escape_into(b, v->u.s.p, v->u.s.len);
        return;
    case V_SUITE:
        if (v->u.su.n == 0) {
            buf_puts(b, "[]");
            return;
        }
        buf_putc(b, '[');
        for (i = 0; i < v->u.su.n; i++) {
            if (i)
                buf_putc(b, ',');
            if (indent >= 0) {
                int k;
                buf_putc(b, '\n');
                for (k = 0; k < (prof + 1) * indent; k++)
                    buf_putc(b, ' ');
            }
            json_dump(v->u.su.it[i], b, indent, prof + 1, externe);
        }
        if (indent >= 0) {
            int k;
            buf_putc(b, '\n');
            for (k = 0; k < prof * indent; k++)
                buf_putc(b, ' ');
        }
        buf_putc(b, ']');
        return;
    case V_FICHE:
        if (v->u.fi.n == 0) {
            buf_puts(b, "{}");
            return;
        }
        buf_putc(b, '{');
        for (i = 0; i < v->u.fi.n; i++) {
            if (i)
                buf_putc(b, ',');
            if (indent >= 0) {
                int k;
                buf_putc(b, '\n');
                for (k = 0; k < (prof + 1) * indent; k++)
                    buf_putc(b, ' ');
            }
            json_escape_into(b, v->u.fi.k[i]->u.s.p, v->u.fi.k[i]->u.s.len);
            buf_putc(b, ':');
            if (indent >= 0)
                buf_putc(b, ' ');
            json_dump(v->u.fi.v[i], b, indent, prof + 1, externe);
        }
        if (indent >= 0) {
            int k;
            buf_putc(b, '\n');
            for (k = 0; k < prof * indent; k++)
                buf_putc(b, ' ');
        }
        buf_putc(b, '}');
        return;
    default:
        buf_puts(b, "null");
        return;
    }
}

/* a -> texte（Python texte_de） */
static void texte_de(V *v, Buf *b, int prof);
static void texte_de_impl(V *v, Buf *b, int prof)
{
    if (!v || v->t == V_VIDE) {
        return;
    }
    if (v->t == V_OUI) {
        buf_puts(b, "oui");
        return;
    }
    if (v->t == V_NON) {
        buf_puts(b, "non");
        return;
    }
    if (v->t == V_NOMBRE) {
        nombre_texte(v->u.n, b);
        return;
    }
    if (v->t == V_TEXTE) {
        buf_putn(b, v->u.s.p, v->u.s.len);
        return;
    }
    if (v->t == V_SUITE || v->t == V_FICHE) {
        json_dump(v, b, -1, prof, 0);
        return;
    }
    buf_puts(b, "<geste>");
}
static void texte_de(V *v, Buf *b, int prof)
{
    (void)prof;
    texte_de_impl(v, b, 0);
}

/* ================================================================== */
/* 词法                                                                */
/* ================================================================== */
enum { J_MOT, J_NOMBRE, J_TEXTE, J_SYM, J_FIN, J_EOF };

typedef struct {
    int genre;
    char *txt;
    int tlen;
    i128 num;
    int ligne;
} Jeton;

static const char *SYMBOLES[] = {
    "☑", "☒", "∅", "⟦", "⟧", "‹", "›", "⟬", "⟭", "⟼", "≠", "≢", "≤", "≥", "⩽", "⩾", ">=", "<=",
    "==", "≟", "∧", "∨", "&&", "||", "¬", "!", "×", "÷", "−", "⊕", "⊖", "⊗", "⊘",
    "⊞", "↑", "⪢", "⪡", "+", "-", "*", "/", "%", "^", "(", ")", "[", "]", "{", "}", ",", ":", "=", "<", ">",
    NULL};

static int debut_avec(const char *s, int len, int i, const char *p)
{
    int n = (int)strlen(p);
    if (i + n > len)
        return 0;
    return memcmp(s + i, p, (size_t)n) == 0;
}

static Jeton *lexer(const char *src, int len, int *nb)
{
    int i = 0, ligne = 1, cap = 256, n = 0;
    Jeton *t = (Jeton *)xmalloc(sizeof(Jeton) * (size_t)cap);
    if (len >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
        i = 3;
    }
#define PUSH_J(g) do { if (n == cap) { cap *= 2; t = (Jeton *)xrealloc(t, sizeof(Jeton) * (size_t)cap); } \
        memset(&t[n], 0, sizeof(Jeton)); t[n].genre = (g); t[n].ligne = ligne; n++; } while (0)
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        int cp, cl;
        if (c == '\n') {
            PUSH_J(J_FIN);
            ligne++;
            i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r') {
            i++;
            continue;
        }
        /* 注释 */
        if (c == '#') {
            while (i < len && src[i] != '\n')
                i++;
            continue;
        }
        if (debut_avec(src, len, i, "‖")) {
            while (i < len && src[i] != '\n')
                i++;
            continue;
        }
        /* 字符串 */
        if (debut_avec(src, len, i, "\"")) {
            Buf b;
            buf_init(&b);
            i++;
            for (;;) {
                if (i >= len) {
                    defaut("lexique", ligne, "texte non terminé");
                }
                if (src[i] == '\\') {
                    int e, el;
                    if (i + 1 >= len)
                        defaut("lexique", ligne, "échappement incomplet");
                    el = u8_decode((const unsigned char *)src + i + 1, len - i - 1, &e);
                    switch (e) {
                    case 'n': buf_putc(&b, '\n'); break;
                    case 't': buf_putc(&b, '\t'); break;
                    case 'r': buf_putc(&b, '\r'); break;
                    case '"': buf_putc(&b, '"'); break;
                    case '\\': buf_putc(&b, '\\'); break;
                    default: {
                        int q;
                        for (q = 0; q < el; q++)
                            buf_putc(&b, src[i + 1 + q]);
                        break;
                    }
                    }
                    i += 1 + el;
                    continue;
                }
                if (src[i] == '"') {
                    i++;
                    break;
                }
                if (src[i] == '\n')
                    defaut("lexique", ligne, "texte non terminé");
                buf_putc(&b, src[i]);
                i++;
            }
            PUSH_J(J_TEXTE);
            t[n - 1].txt = buf_take(&b);
            t[n - 1].tlen = b.len;
            continue;
        }
        if (debut_avec(src, len, i, "«")) {
            Buf b;
            buf_init(&b);
            i += 2;
            for (;;) {
                if (i >= len)
                    defaut("lexique", ligne, "texte non terminé");
                if (debut_avec(src, len, i, "»")) {
                    i += 2;
                    break;
                }
                if (src[i] == '\n')
                    defaut("lexique", ligne, "texte non terminé");
                buf_putc(&b, src[i]);
                i++;
            }
            PUSH_J(J_TEXTE);
            t[n - 1].txt = buf_take(&b);
            t[n - 1].tlen = b.len;
            continue;
        }
        /* 数字 */
        if (c >= '0' && c <= '9') {
            int j = i;
            i128 val;
            while (j < len && ((src[j] >= '0' && src[j] <= '9') ||
                               (src[j] == '.' && j + 1 < len && src[j + 1] >= '0' && src[j + 1] <= '9')))
                j++;
            if (!nombre_depuis_texte(src + i, j - i, &val))
                defaut("lexique", ligne, "nombre illisible: %.*s", j - i, src + i);
            if (i128_abs(val) > NOTE_MAX_I128)
                defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
            PUSH_J(J_NOMBRE);
            t[n - 1].num = val;
            i = j;
            continue;
        }
        /* 标识符（含点分名） */
        cl = u8_decode((const unsigned char *)src + i, len - i, &cp);
        if (cp_isalpha(cp)) {
            int j = i + cl;
            int start = i;
            while (j < len) {
                int c2, k2 = u8_decode((const unsigned char *)src + j, len - j, &c2);
                if (cp_isalnum(c2) || c2 == '-' || c2 == '\'') {
                    j += k2;
                    continue;
                }
                break;
            }
            while (j + 1 < len && src[j] == '.') {
                int c2, k2 = u8_decode((const unsigned char *)src + j + 1, len - j - 1, &c2);
                int k;
                if (!cp_isalpha(c2))
                    break;
                k = j + 1 + k2;
                while (k < len) {
                    int c3, k3 = u8_decode((const unsigned char *)src + k, len - k, &c3);
                    if (cp_isalnum(c3) || c3 == '-' || c3 == '\'') {
                        k += k3;
                        continue;
                    }
                    break;
                }
                j = k;
            }
            PUSH_J(J_MOT);
            t[n - 1].txt = xstrndup(src + start, j - start);
            t[n - 1].tlen = j - start;
            i = j;
            continue;
        }
        /* 符号 */
        {
            int k, trouve = 0;
            for (k = 0; SYMBOLES[k]; k++) {
                if (debut_avec(src, len, i, SYMBOLES[k])) {
                    int ln = (int)strlen(SYMBOLES[k]);
                    PUSH_J(J_SYM);
                    t[n - 1].txt = xstrndup(SYMBOLES[k], ln);
                    t[n - 1].tlen = ln;
                    i += ln;
                    trouve = 1;
                    break;
                }
            }
            if (trouve)
                continue;
        }
        {
            char chbuf[8];
            int ln = u8_encode(cp, chbuf);
            chbuf[ln] = 0;
            defaut("lexique", ligne, "caractère inattendu: '%s'", chbuf);
        }
    }
    PUSH_J(J_EOF);
#undef PUSH_J
    *nb = n;
    return t;
}

/* ------------------------------------------------------------------ */
/* 记号格式化（错误提示用，尽量与 Python %r 一致）                       */
/* ------------------------------------------------------------------ */
static void repr_str(Buf *b, const char *s, int len)
{
    int i, has_sq = 0, has_dq = 0;
    for (i = 0; i < len; i++) {
        if (s[i] == '\'')
            has_sq = 1;
        if (s[i] == '"')
            has_dq = 1;
    }
    if (has_sq && !has_dq) {
        buf_putc(b, '"');
        for (i = 0; i < len; i++) {
            if (s[i] == '"')
                buf_puts(b, "\\\"");
            else
                buf_putc(b, s[i]);
        }
        buf_putc(b, '"');
        return;
    }
    buf_putc(b, '\'');
    for (i = 0; i < len; i++) {
        if (s[i] == '\'')
            buf_puts(b, "\\'");
        else if (s[i] == '\\')
            buf_puts(b, "\\\\");
        else
            buf_putc(b, s[i]);
    }
    buf_putc(b, '\'');
}

/* token 的「显示值」写入 b；eof 时返回 0 */
static int jeton_montre(Jeton *j, Buf *b, int fin_mot)
{
    if (j->genre == J_MOT || j->genre == J_SYM) {
        repr_str(b, j->txt, j->tlen);
        return 1;
    }
    if (j->genre == J_NOMBRE) {
        char tmp[48];
        int ln;
        u128 u;
        if (j->num < 0) {
            buf_putc(b, '-');
            u = (u128)(-(j->num + 1)) + 1;
        } else {
            u = (u128)j->num;
        }
        ln = u128_digits(u, tmp);
        buf_putn(b, tmp, ln);
        return 1;
    }
    if (j->genre == J_TEXTE) {
        repr_str(b, j->txt, j->tlen);
        return 1;
    }
    if (j->genre == J_FIN) {
        const char *t = fin_mot ? "fin d'instruction" : "fin de ligne";
        repr_str(b, t, (int)strlen(t));
    } else {
        const char *t = "fin de fichier";
        repr_str(b, t, (int)strlen(t));
    }
    return 0;
}

/* ================================================================== */
/* 语法树                                                              */
/* ================================================================== */
enum {
    N_NOMBRE, N_TEXTE, N_OUI, N_NON, N_VIDE, N_NOM, N_SUITE, N_FICHE,
    N_ELEMENT, N_CHAMP, N_UN, N_BIN, N_CMP, N_ET, N_OU, N_APPEL, N_BLOC,
    S_EXPR, S_LIER, S_RELIER, S_AFFICHER, S_RENDS, S_SILENCE, S_SI,
    S_TANTQUE, S_POURCHAQUE, S_DEFINIR, S_BRISE, S_PASSE
};

struct Node {
    int kind;
    int line;
    i128 num;
    char *s;
    int slen;
    char op; /* '-', '+', ... ; 'n' = non */
    Node *x, *y, *z;
    Node **items;
    int nitems;
    char **keys;
    int *keylens;
    Node **vals;
    int npairs;
    V *cache;
};

static Node *nd_new(int kind, int line)
{
    Node *n = (Node *)xcalloc(1, sizeof(Node));
    n->kind = kind;
    n->line = line;
    return n;
}

typedef struct {
    Jeton *t;
    int n;
    int i;
} Parseur;

static Jeton *P_pic(Parseur *p, int k)
{
    int j = p->i + k;
    return j < p->n ? &p->t[j] : &p->t[p->n - 1];
}
static int P_genre(Parseur *p, int k) { return P_pic(p, k)->genre; }
static const char *P_txt(Parseur *p, int k, int *len)
{
    Jeton *j = P_pic(p, k);
    if (j->genre == J_MOT || j->genre == J_SYM) {
        if (len)
            *len = j->tlen;
        return j->txt;
    }
    if (len)
        *len = 0;
    return NULL;
}
static int P_est(Parseur *p, const char *v, int k)
{
    int l;
    const char *s = P_txt(p, k, &l);
    int n = (int)strlen(v);
    return s && l == n && memcmp(s, v, (size_t)n) == 0;
}
static Jeton P_avale(Parseur *p)
{
    Jeton j = p->t[p->i];
    if (j.genre != J_EOF)
        p->i++;
    return j;
}
static void P_erreur_attendu(Parseur *p, const char *quoi)
{
    Buf b;
    buf_init(&b);
    buf_printf(&b, "attendu %s, trouvé ", quoi);
    jeton_montre(P_pic(p, 0), &b, 0);
    defaut("syntaxe", P_pic(p, 0)->ligne, "%s", buf_c(&b));
}
static Jeton P_attendre(Parseur *p, const char *v, const char *quoi)
{
    if (!P_est(p, v, 0))
        P_erreur_attendu(p, quoi);
    return P_avale(p);
}
static char *P_attendre_nom(Parseur *p, const char *quoi, int *len)
{
    Jeton *j = P_pic(p, 0);
    if (j->genre != J_MOT) {
        Buf b;
        buf_init(&b);
        buf_printf(&b, "attendu %s, trouvé ", quoi);
        jeton_montre(j, &b, 1);
        defaut("syntaxe", j->ligne, "%s", buf_c(&b));
    }
    P_avale(p);
    if (len)
        *len = j->tlen;
    return xstrndup(j->txt, j->tlen);
}

static Node *P_expression(Parseur *p);
static Node *P_bloc(Parseur *p);
static Node *P_sequence(Parseur *p, int dans_bloc, int *n);

/* -- 表达式 -- */
static Node *P_ou(Parseur *p);
static Node *P_et(Parseur *p);
static Node *P_comparaison(Parseur *p);
static Node *P_somme(Parseur *p);
static Node *P_produit(Parseur *p);
static Node *P_puissance(Parseur *p);
static Node *P_unaire(Parseur *p);
static Node *P_primaire(Parseur *p);
static Node *P_simple(Parseur *p);
static Node *P_application(Parseur *p, char *nom, int nomlen);

static int P_debut_expression(Parseur *p, int k)
{
    Jeton *j = P_pic(p, k);
    if (j->genre == J_NOMBRE || j->genre == J_TEXTE || j->genre == J_MOT)
        return 1;
    if (j->genre == J_SYM && (strcmp(j->txt, "‹") == 0 || strcmp(j->txt, "⟬") == 0 ||
                              strcmp(j->txt, "(") == 0 || strcmp(j->txt, "¬") == 0 ||
                              strcmp(j->txt, "−") == 0 || strcmp(j->txt, "-") == 0))
        return 1;
    return 0;
}

static Node *P_ou(Parseur *p)
{
    Node *g = P_et(p);
    while (P_est(p, "ou", 0) || P_est(p, "∨", 0) || P_est(p, "||", 0)) {
        Node *n;
        P_avale(p);
        n = nd_new(N_OU, g->line);
        n->x = g;
        n->y = P_et(p);
        g = n;
    }
    return g;
}
static Node *P_et(Parseur *p)
{
    Node *g = P_comparaison(p);
    while (P_est(p, "et", 0) || P_est(p, "∧", 0) || P_est(p, "&&", 0)) {
        Node *n;
        P_avale(p);
        n = nd_new(N_ET, g->line);
        n->x = g;
        n->y = P_comparaison(p);
        g = n;
    }
    return g;
}
static Node *P_comparaison(Parseur *p)
{
    Node *g = P_somme(p);
    for (;;) {
        char op = 0;
        const char *v = P_txt(p, 0, NULL);
        if (P_est(p, "plus", 0) && P_est(p, "grand", 1) && P_est(p, "que", 2)) {
            P_avale(p);
            P_avale(p);
            P_avale(p);
            op = '>';
        } else if (P_est(p, "plus", 0) && P_est(p, "petit", 1) && P_est(p, "que", 2)) {
            P_avale(p);
            P_avale(p);
            P_avale(p);
            op = '<';
        } else if (P_est(p, "au", 0) && P_est(p, "moins", 1)) {
            P_avale(p);
            P_avale(p);
            op = 'G'; /* >= */
        } else if (P_est(p, "au", 0) && P_est(p, "plus", 1)) {
            P_avale(p);
            P_avale(p);
            op = 'L'; /* <= */
        } else if (P_est(p, "égal", 0) && P_est(p, "à", 1)) {
            P_avale(p);
            P_avale(p);
            op = '=';
        } else if (P_est(p, "différent", 0) && P_est(p, "de", 1)) {
            P_avale(p);
            P_avale(p);
            op = '!';
        } else if (v) {
            if (strcmp(v, ">") == 0 || strcmp(v, "⪢") == 0) { P_avale(p); op = '>'; }
            else if (strcmp(v, "<") == 0 || strcmp(v, "⪡") == 0) { P_avale(p); op = '<'; }
            else if (strcmp(v, ">=") == 0 || strcmp(v, "≥") == 0 || strcmp(v, "⩾") == 0) { P_avale(p); op = 'G'; }
            else if (strcmp(v, "<=") == 0 || strcmp(v, "≤") == 0 || strcmp(v, "⩽") == 0) { P_avale(p); op = 'L'; }
            else if (strcmp(v, "=") == 0 || strcmp(v, "==") == 0 || strcmp(v, "≟") == 0) { P_avale(p); op = '='; }
            else if (strcmp(v, "≠") == 0 || strcmp(v, "≢") == 0) { P_avale(p); op = '!'; }
            else if (strcmp(v, "égal") == 0) { P_avale(p); op = '='; }
            else if (strcmp(v, "différent") == 0) { P_avale(p); op = '!'; }
        }
        if (!op)
            break;
        {
            Node *n = nd_new(N_CMP, g->line);
            n->op = op;
            n->x = g;
            n->y = P_somme(p);
            g = n;
        }
    }
    return g;
}
static Node *P_somme(Parseur *p)
{
    Node *g = P_produit(p);
    for (;;) {
        const char *v = P_txt(p, 0, NULL);
        char op;
        if (!v)
            break;
        if ((strcmp(v, "plus") == 0 || strcmp(v, "+") == 0) &&
            (P_est(p, "petit", 1) || P_est(p, "grand", 1)))
            break;
        if (strcmp(v, "plus") == 0 || strcmp(v, "+") == 0 || strcmp(v, "⊕") == 0)
            op = '+';
        else if (strcmp(v, "moins") == 0 || strcmp(v, "−") == 0 || strcmp(v, "-") == 0 || strcmp(v, "⊖") == 0)
            op = '-';
        else
            break;
        P_avale(p);
        {
            Node *n = nd_new(N_BIN, g->line);
            n->op = op;
            n->x = g;
            n->y = P_produit(p);
            g = n;
        }
    }
    return g;
}
static Node *P_produit(Parseur *p)
{
    Node *g = P_puissance(p);
    for (;;) {
        const char *v = P_txt(p, 0, NULL);
        char op;
        if (!v)
            break;
        if (strcmp(v, "fois") == 0 || strcmp(v, "×") == 0 || strcmp(v, "*") == 0 || strcmp(v, "⊗") == 0)
            op = '*';
        else if (strcmp(v, "sur") == 0 || strcmp(v, "÷") == 0 || strcmp(v, "/") == 0 || strcmp(v, "⊘") == 0)
            op = '/';
        else if (strcmp(v, "modulo") == 0 || strcmp(v, "%") == 0 || strcmp(v, "⊞") == 0)
            op = '%';
        else
            break;
        P_avale(p);
        {
            Node *n = nd_new(N_BIN, g->line);
            n->op = op;
            n->x = g;
            n->y = P_puissance(p);
            g = n;
        }
    }
    return g;
}
static Node *P_puissance(Parseur *p)
{
    Node *g = P_unaire(p);
    if (P_est(p, "puissance", 0) || P_est(p, "^", 0) || P_est(p, "↑", 0)) {
        Node *n;
        P_avale(p);
        n = nd_new(N_BIN, g->line);
        n->op = '^';
        n->x = g;
        n->y = P_puissance(p);
        return n;
    }
    return g;
}
static Node *P_unaire(Parseur *p)
{
    if (P_est(p, "non", 0) || P_est(p, "¬", 0) || P_est(p, "!", 0)) {
        Node *n;
        P_avale(p);
        n = nd_new(N_UN, P_pic(p, 0)->ligne);
        n->op = 'n';
        n->x = P_unaire(p);
        return n;
    }
    if (P_est(p, "moins", 0) || P_est(p, "−", 0) || P_est(p, "-", 0)) {
        Node *n;
        P_avale(p);
        n = nd_new(N_UN, P_pic(p, 0)->ligne);
        n->op = '-';
        n->x = P_unaire(p);
        return n;
    }
    return P_primaire(p);
}

static Node *P_primaire(Parseur *p)
{
    Jeton *j = P_pic(p, 0);
    int genre = j->genre, ligne = j->ligne;
    if (genre == J_NOMBRE) {
        Node *n = nd_new(N_NOMBRE, ligne);
        n->num = j->num;
        P_avale(p);
        return n;
    }
    if (genre == J_TEXTE) {
        Node *n = nd_new(N_TEXTE, ligne);
        n->s = xstrndup(j->txt, j->tlen);
        n->slen = j->tlen;
        P_avale(p);
        return n;
    }
    if (genre == J_MOT) {
        if (strcmp(j->txt, "OUI") == 0 || strcmp(j->txt, "oui") == 0 ||
            strcmp(j->txt, "vrai") == 0) {
            P_avale(p);
            return nd_new(N_OUI, ligne);
        }
        if (strcmp(j->txt, "NON") == 0 || strcmp(j->txt, "faux") == 0) {
            P_avale(p);
            return nd_new(N_NON, ligne);
        }
        if (strcmp(j->txt, "VIDE") == 0 || strcmp(j->txt, "vide") == 0 ||
            strcmp(j->txt, "rien") == 0) {
            P_avale(p);
            return nd_new(N_VIDE, ligne);
        }
        if (strcmp(j->txt, "élément") == 0) {
            Node *n;
            P_avale(p);
            n = nd_new(N_ELEMENT, ligne);
            n->x = P_simple(p);
            P_attendre(p, "de", "« de »");
            n->y = P_primaire(p);
            return n;
        }
        if (strcmp(j->txt, "champ") == 0) {
            Node *n;
            P_avale(p);
            n = nd_new(N_CHAMP, ligne);
            n->x = P_simple(p);
            P_attendre(p, "de", "« de »");
            n->y = P_primaire(p);
            return n;
        }
        {
            char *nom = xstrndup(j->txt, j->tlen);
            int nl = j->tlen;
            P_avale(p);
            return P_application(p, nom, nl);
        }
    }
    if (genre == J_SYM) {
        if (strcmp(j->txt, "‹") == 0) {
            Node *n;
            int cap = 4;
            Node **items = (Node **)xmalloc(sizeof(Node *) * (size_t)cap);
            int cnt = 0;
            P_avale(p);
            /* 跳过行尾 */
            while (P_genre(p, 0) == J_FIN)
                P_avale(p);
            if (!P_est(p, "›", 0)) {
                for (;;) {
                    while (P_genre(p, 0) == J_FIN)
                        P_avale(p);
                    if (cnt == cap) {
                        cap *= 2;
                        items = (Node **)xrealloc(items, sizeof(Node *) * (size_t)cap);
                    }
                    items[cnt++] = P_expression(p);
                    while (P_genre(p, 0) == J_FIN)
                        P_avale(p);
                    if (P_est(p, ",", 0)) {
                        P_avale(p);
                        continue;
                    }
                    break;
                }
            }
            while (P_genre(p, 0) == J_FIN)
                P_avale(p);
            P_attendre(p, "›", "›");
            n = nd_new(N_SUITE, ligne);
            n->items = items;
            n->nitems = cnt;
            return n;
        }
        if (strcmp(j->txt, "⟬") == 0) {
            Node *n;
            int cap = 4, cnt = 0;
            char **keys = (char **)xmalloc(sizeof(char *) * (size_t)cap);
            int *klens = (int *)xmalloc(sizeof(int) * (size_t)cap);
            Node **vals = (Node **)xmalloc(sizeof(Node *) * (size_t)cap);
            P_avale(p);
            while (P_genre(p, 0) == J_FIN)
                P_avale(p);
            if (!P_est(p, "⟭", 0)) {
                for (;;) {
                    Jeton *kt;
                    while (P_genre(p, 0) == J_FIN)
                        P_avale(p);
                    kt = P_pic(p, 0);
                    if (cnt == cap) {
                        cap *= 2;
                        keys = (char **)xrealloc(keys, sizeof(char *) * (size_t)cap);
                        klens = (int *)xrealloc(klens, sizeof(int) * (size_t)cap);
                        vals = (Node **)xrealloc(vals, sizeof(Node *) * (size_t)cap);
                    }
                    if (kt->genre == J_MOT || kt->genre == J_TEXTE) {
                        keys[cnt] = xstrndup(kt->txt, kt->tlen);
                        klens[cnt] = kt->tlen;
                        P_avale(p);
                    } else {
                        defaut("syntaxe", kt->ligne, "clé de fiche attendue");
                    }
                    P_attendre(p, "⟼", "⟼");
                    vals[cnt] = P_expression(p);
                    cnt++;
                    while (P_genre(p, 0) == J_FIN)
                        P_avale(p);
                    if (P_est(p, ",", 0)) {
                        P_avale(p);
                        continue;
                    }
                    break;
                }
            }
            while (P_genre(p, 0) == J_FIN)
                P_avale(p);
            P_attendre(p, "⟭", "⟭");
            n = nd_new(N_FICHE, ligne);
            n->keys = keys;
            n->keylens = klens;
            n->vals = vals;
            n->npairs = cnt;
            return n;
        }
        if (strcmp(j->txt, "(") == 0) {
            Node *e;
            P_avale(p);
            e = P_expression(p);
            P_attendre(p, ")", ")");
            return e;
        }
        if (strcmp(j->txt, "☑") == 0) {
            P_avale(p);
            return nd_new(N_OUI, ligne);
        }
        if (strcmp(j->txt, "☒") == 0) {
            P_avale(p);
            return nd_new(N_NON, ligne);
        }
        if (strcmp(j->txt, "∅") == 0) {
            P_avale(p);
            return nd_new(N_VIDE, ligne);
        }
    }
    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "expression attendue, trouvé ");
        jeton_montre(j, &b, 0);
        defaut("syntaxe", ligne, "%s", buf_c(&b));
    }
    return NULL;
}

static Node *P_simple(Parseur *p)
{
    Jeton *j = P_pic(p, 0);
    if (j->genre == J_NOMBRE) {
        Node *n = nd_new(N_NOMBRE, j->ligne);
        n->num = j->num;
        P_avale(p);
        return n;
    }
    if (j->genre == J_TEXTE) {
        Node *n = nd_new(N_TEXTE, j->ligne);
        n->s = xstrndup(j->txt, j->tlen);
        n->slen = j->tlen;
        P_avale(p);
        return n;
    }
    if (j->genre == J_MOT) {
        Node *n;
        P_avale(p);
        if (strcmp(j->txt, "OUI") == 0 || strcmp(j->txt, "oui") == 0 || strcmp(j->txt, "vrai") == 0)
            return nd_new(N_OUI, j->ligne);
        if (strcmp(j->txt, "NON") == 0 || strcmp(j->txt, "faux") == 0)
            return nd_new(N_NON, j->ligne);
        if (strcmp(j->txt, "VIDE") == 0 || strcmp(j->txt, "vide") == 0 || strcmp(j->txt, "rien") == 0)
            return nd_new(N_VIDE, j->ligne);
        n = nd_new(N_NOM, j->ligne);
        n->s = xstrndup(j->txt, j->tlen);
        n->slen = j->tlen;
        return n;
    }
    if (j->genre == J_SYM && strcmp(j->txt, "(") == 0) {
        Node *e;
        P_avale(p);
        e = P_expression(p);
        P_attendre(p, ")", ")");
        return e;
    }
    if (j->genre == J_SYM && (strcmp(j->txt, "‹") == 0 || strcmp(j->txt, "⟬") == 0))
        return P_primaire(p);
    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "expression attendue, trouvé ");
        jeton_montre(j, &b, 0);
        defaut("syntaxe", j->ligne, "%s", buf_c(&b));
    }
    return NULL;
}

static Node *P_application(Parseur *p, char *nom, int nomlen)
{
    (void)nomlen;
    if (strchr(nom, '.')) {
        int cap = 2, cnt = 0;
        Node **args = (Node **)xmalloc(sizeof(Node *) * (size_t)cap);
        Node *n;
        while (P_debut_expression(p, 0)) {
            if (cnt == cap) {
                cap *= 2;
                args = (Node **)xrealloc(args, sizeof(Node *) * (size_t)cap);
            }
            args[cnt++] = P_somme(p);
        }
        n = nd_new(N_APPEL, P_pic(p, 0)->ligne);
        n->s = nom;
        n->slen = (int)strlen(nom);
        n->items = args;
        n->nitems = cnt;
        return n;
    }
    if (P_est(p, "de", 0)) {
        int cap = 2, cnt = 0;
        Node **args;
        Node *n;
        P_avale(p);
        args = (Node **)xmalloc(sizeof(Node *) * (size_t)cap);
        args[cnt++] = P_somme(p);
        for (;;) {
            if (P_est(p, "et", 0) || P_est(p, "par", 0) || P_est(p, "avec", 0) || P_est(p, "dans", 0)) {
                P_avale(p);
            } else if (P_est(p, "depuis", 0)) {
                Node *debut, *fin;
                P_avale(p);
                debut = P_somme(p);
                P_attendre(p, "jusqu'à", "« jusqu'à »");
                fin = P_somme(p);
                if (cnt + 2 > cap) {
                    cap = (cnt + 2) * 2;
                    args = (Node **)xrealloc(args, sizeof(Node *) * (size_t)cap);
                }
                args[cnt++] = debut;
                args[cnt++] = fin;
                continue;
            } else {
                break;
            }
            if (cnt == cap) {
                cap *= 2;
                args = (Node **)xrealloc(args, sizeof(Node *) * (size_t)cap);
            }
            args[cnt++] = P_somme(p);
        }
        n = nd_new(N_APPEL, P_pic(p, 0)->ligne);
        n->s = nom;
        n->slen = (int)strlen(nom);
        n->items = args;
        n->nitems = cnt;
        return n;
    }
    if (P_est(p, "(", 0)) {
        int cap = 2, cnt = 0;
        Node **args;
        Node *n;
        P_avale(p);
        args = (Node **)xmalloc(sizeof(Node *) * (size_t)cap);
        if (!P_est(p, ")", 0)) {
            for (;;) {
                if (cnt == cap) {
                    cap *= 2;
                    args = (Node **)xrealloc(args, sizeof(Node *) * (size_t)cap);
                }
                args[cnt++] = P_expression(p);
                if (P_est(p, ",", 0)) {
                    P_avale(p);
                    continue;
                }
                break;
            }
        }
        P_attendre(p, ")", ")");
        n = nd_new(N_APPEL, P_pic(p, 0)->ligne);
        n->s = nom;
        n->slen = (int)strlen(nom);
        n->items = args;
        n->nitems = cnt;
        return n;
    }
    {
        Node *n = nd_new(N_NOM, P_pic(p, 0)->ligne);
        n->s = nom;
        n->slen = (int)strlen(nom);
        return n;
    }
}

/* -- 语句 -- */
static Node *P_expression(Parseur *p) { return P_ou(p); }

static Node *P_params(Parseur *p, int *nparams)
{
    int cap = 4, cnt = 0;
    char **names = (char **)xmalloc(sizeof(char *) * (size_t)cap);
    int *lens = (int *)xmalloc(sizeof(int) * (size_t)cap);
    if (P_est(p, "(", 0)) {
        P_avale(p);
        if (!P_est(p, ")", 0)) {
            for (;;) {
                if (cnt == cap) {
                    cap *= 2;
                    names = (char **)xrealloc(names, sizeof(char *) * (size_t)cap);
                    lens = (int *)xrealloc(lens, sizeof(int) * (size_t)cap);
                }
                names[cnt] = P_attendre_nom(p, "un paramètre", &lens[cnt]);
                cnt++;
                if (P_est(p, ",", 0)) {
                    P_avale(p);
                    continue;
                }
                break;
            }
        }
        P_attendre(p, ")", ")");
    } else {
        names[cnt] = P_attendre_nom(p, "un paramètre", &lens[cnt]);
        cnt++;
        while (P_est(p, "et", 0)) {
            P_avale(p);
            if (cnt == cap) {
                cap *= 2;
                names = (char **)xrealloc(names, sizeof(char *) * (size_t)cap);
                lens = (int *)xrealloc(lens, sizeof(int) * (size_t)cap);
            }
            names[cnt] = P_attendre_nom(p, "un paramètre", &lens[cnt]);
            cnt++;
        }
    }
    {
        Node *n = nd_new(N_BLOC, P_pic(p, 0)->ligne);
        n->keys = names;
        n->keylens = lens;
        n->npairs = cnt;
        *nparams = cnt;
        return n;
    }
}

static Node *P_chaine_si(Parseur *p, int ligne)
{
    Node *n = nd_new(S_SI, ligne);
    int cap = 2, cnt = 0;
    Node **conds = (Node **)xmalloc(sizeof(Node *) * (size_t)cap);
    Node **blocs = (Node **)xmalloc(sizeof(Node *) * (size_t)cap);
    conds[cnt] = P_expression(p);
    P_attendre(p, "alors", "« alors »");
    blocs[cnt] = P_bloc(p);
    cnt++;
    n->z = NULL;
    for (;;) {
        int sauve = p->i;
        while (P_genre(p, 0) == J_FIN)
            P_avale(p);
        if (P_est(p, "sinonsi", 0) || P_est(p, "oualors", 0)) {
            P_avale(p);
            if (cnt == cap) {
                cap *= 2;
                conds = (Node **)xrealloc(conds, sizeof(Node *) * (size_t)cap);
                blocs = (Node **)xrealloc(blocs, sizeof(Node *) * (size_t)cap);
            }
            conds[cnt] = P_expression(p);
            P_attendre(p, "alors", "« alors »");
            blocs[cnt] = P_bloc(p);
            cnt++;
            continue;
        }
        if (P_est(p, "sinon", 0) || P_est(p, "autrement", 0)) {
            P_avale(p);
            n->z = P_bloc(p);
            break;
        }
        p->i = sauve;
        break;
    }
    n->items = conds;
    n->vals = blocs;
    n->nitems = cnt;
    return n;
}

static Node *P_instruction(Parseur *p)
{
    Jeton *j = P_pic(p, 0);
    int ligne = j->ligne;
    const char *v = (j->genre == J_MOT) ? j->txt : NULL;

    if (v && (strcmp(v, "que") == 0 || strcmp(v, "soit") == 0)) {
        char *nom;
        int nl;
        Node *n;
        P_avale(p);
        nom = P_attendre_nom(p, "un nom", &nl);
        if (P_est(p, "vaille", 0)) {
            P_avale(p);
            n = nd_new(S_LIER, ligne);
            n->s = nom;
            n->slen = nl;
            n->x = P_expression(p);
            return n;
        }
        if (P_est(p, "devienne", 0) || P_est(p, "remue", 0)) {
            P_avale(p);
            n = nd_new(S_RELIER, ligne);
            n->s = nom;
            n->slen = nl;
            n->x = P_expression(p);
            return n;
        }
        defaut("syntaxe", ligne, "attendu « vaille » ou « devienne »");
    }
    if (v && strcmp(v, "donne") == 0) {
        Node *n, *val;
        char *nom;
        int nl;
        P_avale(p);
        val = P_expression(p);
        P_attendre(p, "à", "« à »");
        nom = P_attendre_nom(p, "un nom", &nl);
        n = nd_new(S_LIER, ligne);
        n->s = nom;
        n->slen = nl;
        n->x = val;
        return n;
    }
    if (v && (strcmp(v, "affiche") == 0 || strcmp(v, "dis") == 0 || strcmp(v, "trace") == 0)) {
        Node *n;
        P_avale(p);
        n = nd_new(S_AFFICHER, ligne);
        n->x = P_expression(p);
        return n;
    }
    if (v && strcmp(v, "rends") == 0) {
        Node *n;
        P_avale(p);
        n = nd_new(S_RENDS, ligne);
        n->x = P_expression(p);
        return n;
    }
    if (v && strcmp(v, "silence") == 0) {
        P_avale(p);
        return nd_new(S_SILENCE, ligne);
    }
    if (v && (strcmp(v, "si") == 0 || strcmp(v, "quand") == 0 || strcmp(v, "lorsque") == 0)) {
        P_avale(p);
        return P_chaine_si(p, ligne);
    }
    if (v && (strcmp(v, "tantque") == 0 || strcmp(v, "boucle") == 0)) {
        Node *n;
        P_avale(p);
        n = nd_new(S_TANTQUE, ligne);
        n->x = P_expression(p);
        n->y = P_bloc(p);
        return n;
    }
    if (v && strcmp(v, "tant") == 0) {
        Node *n;
        P_avale(p);
        P_attendre(p, "que", "« que »");
        n = nd_new(S_TANTQUE, ligne);
        n->x = P_expression(p);
        n->y = P_bloc(p);
        return n;
    }
    if (v && strcmp(v, "pour") == 0) {
        Node *n;
        char *nom;
        int nl;
        P_avale(p);
        P_attendre(p, "chaque", "« chaque »");
        nom = P_attendre_nom(p, "un nom", &nl);
        P_attendre(p, "dans", "« dans »");
        n = nd_new(S_POURCHAQUE, ligne);
        n->s = nom;
        n->slen = nl;
        n->x = P_expression(p);
        n->y = P_bloc(p);
        return n;
    }
    if (v && strcmp(v, "on") == 0) {
        Node *n, *params;
        char *nom;
        int nl, nparams = 0;
        P_avale(p);
        P_attendre(p, "définit", "« définit »");
        nom = P_attendre_nom(p, "un nom de geste", &nl);
        P_attendre(p, "de", "« de »");
        params = P_params(p, &nparams);
        P_attendre(p, "comme", "« comme »");
        n = nd_new(S_DEFINIR, ligne);
        n->s = nom;
        n->slen = nl;
        n->keys = params->keys;
        n->keylens = params->keylens;
        n->npairs = nparams;
        n->y = P_bloc(p);
        return n;
    }
    if (v && (strcmp(v, "arrête") == 0 || strcmp(v, "casse") == 0)) {
        P_avale(p);
        if (!(P_genre(p, 0) == J_FIN || P_est(p, "⟧", 0) || P_genre(p, 0) == J_EOF))
            defaut("calcul", ligne, "arrête hors d'une boucle");
        return nd_new(S_BRISE, ligne);
    }
    if (v && (strcmp(v, "continue") == 0 || strcmp(v, "passe") == 0)) {
        P_avale(p);
        if (!(P_genre(p, 0) == J_FIN || P_est(p, "⟧", 0) || P_genre(p, 0) == J_EOF))
            defaut("calcul", ligne, "passe hors d'une boucle");
        return nd_new(S_PASSE, ligne);
    }
    {
        Node *n = nd_new(S_EXPR, ligne);
        n->x = P_expression(p);
        return n;
    }
}

static Node *P_sequence(Parseur *p, int dans_bloc, int *nb)
{
    int cap = 8, cnt = 0;
    Node **items = (Node **)xmalloc(sizeof(Node *) * (size_t)cap);
    for (;;) {
        while (P_genre(p, 0) == J_FIN)
            P_avale(p);
        if (P_genre(p, 0) == J_EOF)
            break;
        if (dans_bloc && P_est(p, "⟧", 0))
            break;
        if (cnt == cap) {
            cap *= 2;
            items = (Node **)xrealloc(items, sizeof(Node *) * (size_t)cap);
        }
        items[cnt++] = P_instruction(p);
    }
    *nb = cnt;
    {
        Node *n = nd_new(N_BLOC, 0);
        n->items = items;
        n->nitems = cnt;
        return n;
    }
}

static Node *P_bloc(Parseur *p)
{
    Node *n;
    int nb = 0;
    P_attendre(p, "⟦", "⟦ (ouverture de bloc)");
    n = P_sequence(p, 1, &nb);
    P_attendre(p, "⟧", "⟧ (fermeture de bloc)");
    n->nitems = nb;
    return n;
}

static Node *analyser(const char *src, int len)
{
    int nj = 0, nb = 0;
    Parseur p;
    Jeton *t = lexer(src, len, &nj);
    Node *prog;
    p.t = t;
    p.n = nj;
    p.i = 0;
    prog = P_sequence(&p, 0, &nb);
    if (P_genre(&p, 0) != J_EOF) {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "entrée en trop: ");
        if (P_pic(&p, 0)->genre == J_FIN || P_pic(&p, 0)->genre == J_EOF)
            buf_puts(&b, "None");
        else
            jeton_montre(P_pic(&p, 0), &b, 0);
        defaut("syntaxe", P_pic(&p, 0)->ligne, "%s", buf_c(&b));
    }
    prog->nitems = nb;
    return prog;
}

/* ------------------------------------------------------------------ */
/* ast JSON（内部表示，数字为 1e-9 单位整数）                           */
/* ------------------------------------------------------------------ */
static void ast_node(Node *n, Buf *b);
static void ast_bloc(Node *n, Buf *b)
{
    int i;
    buf_putc(b, '[');
    for (i = 0; i < n->nitems; i++) {
        if (i)
            buf_puts(b, ", ");
        ast_node(n->items[i], b);
    }
    buf_putc(b, ']');
}
static void ast_expr(Node *n, Buf *b)
{
    int i;
    switch (n->kind) {
    case N_NOMBRE:
        buf_puts(b, "[\"nombre\", ");
        {
            char tmp[48];
            int ln;
            u128 u;
            if (n->num < 0) {
                buf_putc(b, '-');
                u = (u128)(-(n->num + 1)) + 1;
            } else {
                u = (u128)n->num;
            }
            ln = u128_digits(u, tmp);
            buf_putn(b, tmp, ln);
        }
        buf_putc(b, ']');
        return;
    case N_TEXTE:
        buf_puts(b, "[\"texte\", ");
        json_escape_into(b, n->s, n->slen);
        buf_putc(b, ']');
        return;
    case N_OUI: buf_puts(b, "[\"oui\"]"); return;
    case N_NON: buf_puts(b, "[\"non\"]"); return;
    case N_VIDE: buf_puts(b, "[\"vide\"]"); return;
    case N_NOM:
        buf_puts(b, "[\"nom\", ");
        json_escape_into(b, n->s, n->slen);
        buf_putc(b, ']');
        return;
    case N_SUITE:
        buf_puts(b, "[\"suite\", [");
        for (i = 0; i < n->nitems; i++) {
            if (i)
                buf_puts(b, ", ");
            ast_expr(n->items[i], b);
        }
        buf_puts(b, "]]");
        return;
    case N_FICHE:
        buf_puts(b, "[\"fiche\", [");
        for (i = 0; i < n->npairs; i++) {
            if (i)
                buf_puts(b, ", ");
            buf_putc(b, '[');
            json_escape_into(b, n->keys[i], n->keylens[i]);
            buf_puts(b, ", ");
            ast_expr(n->vals[i], b);
            buf_putc(b, ']');
        }
        buf_puts(b, "]]");
        return;
    case N_ELEMENT:
        buf_puts(b, "[\"element\", ");
        ast_expr(n->x, b);
        buf_puts(b, ", ");
        ast_expr(n->y, b);
        buf_putc(b, ']');
        return;
    case N_CHAMP:
        buf_puts(b, "[\"champ-de\", ");
        ast_expr(n->x, b);
        buf_puts(b, ", ");
        ast_expr(n->y, b);
        buf_putc(b, ']');
        return;
    case N_UN:
        buf_puts(b, n->op == 'n' ? "[\"un\", \"non\", " : "[\"un\", \"-\", ");
        ast_expr(n->x, b);
        buf_putc(b, ']');
        return;
    case N_BIN:
        buf_puts(b, "[\"bin\", ");
        {
            char o[3] = {n->op, 0, 0};
            if (n->op == '^') {
                o[0] = '*';
                o[1] = '*';
                json_escape_into(b, o, 2);
            } else {
                json_escape_into(b, o, 1);
            }
        }
        buf_puts(b, ", ");
        ast_expr(n->x, b);
        buf_puts(b, ", ");
        ast_expr(n->y, b);
        buf_putc(b, ']');
        return;
    case N_CMP: {
        const char *o = "==";
        char oo[3] = {0, 0, 0};
        if (n->op == '>') o = ">";
        else if (n->op == '<') o = "<";
        else if (n->op == 'G') o = ">=";
        else if (n->op == 'L') o = "<=";
        else if (n->op == '!') o = "!=";
        buf_puts(b, "[\"cmp\", ");
        oo[0] = o[0];
        oo[1] = o[1];
        json_escape_into(b, oo, (int)strlen(o));
        buf_puts(b, ", ");
        ast_expr(n->x, b);
        buf_puts(b, ", ");
        ast_expr(n->y, b);
        buf_putc(b, ']');
        return;
    }
    case N_ET:
        buf_puts(b, "[\"et\", ");
        ast_expr(n->x, b);
        buf_puts(b, ", ");
        ast_expr(n->y, b);
        buf_putc(b, ']');
        return;
    case N_OU:
        buf_puts(b, "[\"ou\", ");
        ast_expr(n->x, b);
        buf_puts(b, ", ");
        ast_expr(n->y, b);
        buf_putc(b, ']');
        return;
    case N_APPEL:
        buf_puts(b, "[\"appel\", ");
        json_escape_into(b, n->s, n->slen);
        buf_puts(b, ", [");
        for (i = 0; i < n->nitems; i++) {
            if (i)
                buf_puts(b, ", ");
            ast_expr(n->items[i], b);
        }
        buf_puts(b, "]]");
        return;
    default:
        buf_puts(b, "null");
        return;
    }
}
static void ast_node(Node *n, Buf *b)
{
    int i;
    switch (n->kind) {
    case S_EXPR:
        buf_puts(b, "[\"expr\", ");
        ast_expr(n->x, b);
        buf_printf(b, ", %d]", n->line);
        return;
    case S_LIER:
        buf_puts(b, "[\"lier\", ");
        json_escape_into(b, n->s, n->slen);
        buf_puts(b, ", ");
        ast_expr(n->x, b);
        buf_printf(b, ", %d]", n->line);
        return;
    case S_RELIER:
        buf_puts(b, "[\"relier\", ");
        json_escape_into(b, n->s, n->slen);
        buf_puts(b, ", ");
        ast_expr(n->x, b);
        buf_printf(b, ", %d]", n->line);
        return;
    case S_AFFICHER:
        buf_puts(b, "[\"afficher\", ");
        ast_expr(n->x, b);
        buf_printf(b, ", %d]", n->line);
        return;
    case S_RENDS:
        buf_puts(b, "[\"rends\", ");
        ast_expr(n->x, b);
        buf_printf(b, ", %d]", n->line);
        return;
    case S_SILENCE:
        buf_printf(b, "[\"silence\", %d]", n->line);
        return;
    case S_SI:
        buf_puts(b, "[\"si\", [");
        for (i = 0; i < n->nitems; i++) {
            if (i)
                buf_puts(b, ", ");
            buf_putc(b, '[');
            ast_expr(n->items[i], b);
            buf_puts(b, ", ");
            ast_bloc(n->vals[i], b);
            buf_putc(b, ']');
        }
        buf_puts(b, "], ");
        if (n->z)
            ast_bloc(n->z, b);
        else
            buf_puts(b, "null");
        buf_printf(b, ", %d]", n->line);
        return;
    case S_TANTQUE:
        buf_puts(b, "[\"tantque\", ");
        ast_expr(n->x, b);
        buf_puts(b, ", ");
        ast_bloc(n->y, b);
        buf_printf(b, ", %d]", n->line);
        return;
    case S_POURCHAQUE:
        buf_puts(b, "[\"pourchaque\", ");
        json_escape_into(b, n->s, n->slen);
        buf_puts(b, ", ");
        ast_expr(n->x, b);
        buf_puts(b, ", ");
        ast_bloc(n->y, b);
        buf_printf(b, ", %d]", n->line);
        return;
    case S_DEFINIR:
        buf_puts(b, "[\"definir\", ");
        json_escape_into(b, n->s, n->slen);
        buf_puts(b, ", [");
        for (i = 0; i < n->npairs; i++) {
            if (i)
                buf_puts(b, ", ");
            json_escape_into(b, n->keys[i], n->keylens[i]);
        }
        buf_puts(b, "], ");
        ast_bloc(n->y, b);
        buf_printf(b, ", %d]", n->line);
        return;
    case S_BRISE:
        buf_printf(b, "[\"brise\", %d]", n->line);
        return;
    case S_PASSE:
        buf_printf(b, "[\"passe\", %d]", n->line);
        return;
    default:
        buf_puts(b, "null");
        return;
    }
}

/* ================================================================== */
/* 运行期                                                              */
/* ================================================================== */
struct Scope {
    Scope *parent;
    int n, cap;
    char **names;
    int *lens;
    V **vals;
};

static Scope *scope_new(Scope *parent)
{
    Scope *s = (Scope *)xcalloc(1, sizeof(Scope));
    s->parent = parent;
    return s;
}
static void scope_def(Scope *s, const char *name, int len, V *val)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (s->lens[i] == len && memcmp(s->names[i], name, (size_t)len) == 0) {
            s->vals[i] = val;
            return;
        }
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->names = (char **)xrealloc(s->names, sizeof(char *) * (size_t)s->cap);
        s->lens = (int *)xrealloc(s->lens, sizeof(int) * (size_t)s->cap);
        s->vals = (V **)xrealloc(s->vals, sizeof(V *) * (size_t)s->cap);
    }
    s->names[s->n] = xstrndup(name, len);
    s->lens[s->n] = len;
    s->vals[s->n] = val;
    s->n++;
}
static V **scope_trouver(Scope *s, const char *name, int len)
{
    while (s) {
        int i;
        for (i = 0; i < s->n; i++)
            if (s->lens[i] == len && memcmp(s->names[i], name, (size_t)len) == 0)
                return &s->vals[i];
        s = s->parent;
    }
    return NULL;
}
static V *scope_lire(Scope *s, const char *name, int len)
{
    V **p = scope_trouver(s, name, len);
    return p ? *p : NULL;
}

static Natif *natifs_lookup(const char *nom, int len);

static int g_muet = 0;
static jmp_buf g_jmp;
static long long g_profondeur = 0;
static V *g_retour = NULL;

typedef enum { ST_OK = 0, ST_RENDS, ST_BRISE, ST_PASSE } XS;

typedef struct CtrlJmp {
    jmp_buf jb;
    struct CtrlJmp *prev;
} CtrlJmp;
static CtrlJmp *g_ctrl_top = NULL;

static void sortie_ecrire(const char *p, int len)
{
    if (g_muet)
        return;
    if (len > 0)
        fwrite(p, 1, (size_t)len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static V *eval_expr(Node *e, Scope *sc);
static XS exec_stmt(Node *st, Scope *sc, V **out);
static XS exec_bloc(Node *bloc, Scope *sc);
static V *invoquer(V *f, V **args, int nargs);
static Natif *hote_lookup(const char *nom, int len);

static V *eval_natif_ou_hote(const char *nom, int len)
{
    Natif *nd = natifs_lookup(nom, len);
    if (!nd)
        nd = hote_lookup(nom, len);
    if (!nd)
        return NULL;
    {
        V *v = v_new();
        v->t = V_NATIF;
        v->u.nat = nd;
        return v;
    }
}

/* -- 索引 / 字段 -- */
static long long entier_requis(V *v, const char *note)
{
    if (!est_nombre(v))
        defaut("forme", -1, "%s", note);
    if (floor_mod(v->u.n, (i128)ECHELLE) != 0)
        defaut("forme", -1, "%s", note);
    return (long long)floor_div(v->u.n, (i128)ECHELLE);
}

static V *indexer(V *coll, V *idx)
{
    long long i = entier_requis(idx, "indice entier attendu");
    if (coll->t == V_SUITE) {
        if (i < 1 || i > coll->u.su.n)
            defaut("calcul", -1, "indice hors limites");
        return coll->u.su.it[i - 1];
    }
    if (coll->t == V_TEXTE) {
        Grapheme *g;
        int n = graphemes(coll->u.s.p, coll->u.s.len, &g);
        if (i < 1 || i > n)
            defaut("calcul", -1, "indice hors limites");
        return v_texte_n(coll->u.s.p + g[i - 1].start, g[i - 1].len);
    }
    defaut("forme", -1, "indexation impossible");
    return NULL;
}

/* -- 二元运算 -- */
static V *binaire(char op, V *a, V *b)
{
    switch (op) {
    case '+':
        if (a->t == V_NOMBRE && b->t == V_NOMBRE) {
            i128 r;
            if (__builtin_add_overflow(a->u.n, b->u.n, &r) || i128_abs(r) > NOTE_MAX_I128)
                defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
            return v_nombre(r);
        }
        if (a->t == V_TEXTE && b->t == V_TEXTE) {
            Buf bf;
            buf_init(&bf);
            buf_putn(&bf, a->u.s.p, a->u.s.len);
            buf_putn(&bf, b->u.s.p, b->u.s.len);
            {
                V *v = v_texte_n(buf_c(&bf), bf.len);
                return v;
            }
        }
        if (a->t == V_SUITE && b->t == V_SUITE) {
            int n = a->u.su.n + b->u.su.n;
            V **it = (V **)xmalloc(sizeof(V *) * (size_t)(n > 0 ? n : 1));
            int i;
            for (i = 0; i < a->u.su.n; i++)
                it[i] = a->u.su.it[i];
            for (i = 0; i < b->u.su.n; i++)
                it[a->u.su.n + i] = b->u.su.it[i];
            return v_suite(it, n);
        }
        defaut("forme", -1, "addition impossible sur ces valeurs");
        break;
    case '-':
        if (a->t != V_NOMBRE || b->t != V_NOMBRE)
            defaut("forme", -1, "soustraction de non-nombres");
        {
            i128 r;
            if (__builtin_sub_overflow(a->u.n, b->u.n, &r) || i128_abs(r) > NOTE_MAX_I128)
                defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
            return v_nombre(r);
        }
    case '*':
        if (a->t != V_NOMBRE || b->t != V_NOMBRE)
            defaut("forme", -1, "multiplication de non-nombres");
        {
            i128 p, r;
            if (__builtin_mul_overflow(a->u.n, b->u.n, &p))
                defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
            r = floor_div(p, (i128)ECHELLE);
            if (i128_abs(r) > NOTE_MAX_I128)
                defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
            return v_nombre(r);
        }
    case '/':
        if (a->t != V_NOMBRE || b->t != V_NOMBRE)
            defaut("forme", -1, "division de non-nombres");
        if (b->u.n == 0)
            defaut("calcul", -1, "division par zéro");
        {
            i128 p, r;
            if (__builtin_mul_overflow(a->u.n, (i128)ECHELLE, &p))
                defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
            r = floor_div(p, b->u.n);
            if (i128_abs(r) > NOTE_MAX_I128)
                defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
            return v_nombre(r);
        }
    case '%':
        if (a->t != V_NOMBRE || b->t != V_NOMBRE)
            defaut("forme", -1, "modulo de non-nombres");
        if (b->u.n == 0)
            defaut("calcul", -1, "division par zéro");
        {
            i128 q = a->u.n / b->u.n;
            i128 r = a->u.n - b->u.n * q;
            if (i128_abs(r) > NOTE_MAX_I128)
                defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
            return v_nombre(r);
        }
    case '^':
        if (a->t != V_NOMBRE || b->t != V_NOMBRE)
            defaut("forme", -1, "puissance de non-nombres");
        if (floor_mod(b->u.n, (i128)ECHELLE) != 0)
            defaut("forme", -1, "exposant entier attendu");
        {
            long long e = (long long)floor_div(b->u.n, (i128)ECHELLE);
            double x, r;
            i128 ri;
            if (e > 1000 || e < -1000)
                defaut("calcul", -1, "débordement");
            x = (double)a->u.n / 1e9;
            r = pow(x, (double)e);
            if (!(r == r) || r > 1e300 || r < -1e300)
                defaut("calcul", -1, "débordement");
            ri = (i128)nearbyint(r * 1e9);
            if (i128_abs(ri) > NOTE_MAX_I128)
                defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
            return v_nombre(ri);
        }
    }
    defaut("calcul", -1, "opérateur inconnu");
    return NULL;
}

static V *comparer(char op, V *a, V *b)
{
    if (op == '=')
        return egal(a, b) ? v_oui() : v_non();
    if (op == '!')
        return egal(a, b) ? v_non() : v_oui();
    if (a->t == V_NOMBRE && b->t == V_NOMBRE) {
        i128 x = a->u.n, y = b->u.n;
        if (op == '>') return x > y ? v_oui() : v_non();
        if (op == '<') return x < y ? v_oui() : v_non();
        if (op == 'G') return x >= y ? v_oui() : v_non();
        if (op == 'L') return x <= y ? v_oui() : v_non();
    } else if (a->t == V_TEXTE && b->t == V_TEXTE) {
        int n = a->u.s.len < b->u.s.len ? a->u.s.len : b->u.s.len;
        int c = memcmp(a->u.s.p, b->u.s.p, (size_t)n);
        if (c == 0)
            c = a->u.s.len - b->u.s.len;
        if (op == '>') return c > 0 ? v_oui() : v_non();
        if (op == '<') return c < 0 ? v_oui() : v_non();
        if (op == 'G') return c >= 0 ? v_oui() : v_non();
        if (op == 'L') return c <= 0 ? v_oui() : v_non();
    } else {
        defaut("forme", -1, "comparaison entre types incompatibles");
    }
    defaut("calcul", -1, "comparateur inconnu");
    return NULL;
}

/* -- 语句/表达式求值 -- */
static V *eval_expr(Node *e, Scope *sc)
{
    switch (e->kind) {
    case N_NOMBRE:
        if (!e->cache)
            e->cache = v_nombre(e->num);
        return e->cache;
    case N_TEXTE:
        if (!e->cache)
            e->cache = v_texte_n(e->s, e->slen);
        return e->cache;
    case N_OUI: return v_oui();
    case N_NON: return v_non();
    case N_VIDE: return v_vide();
    case N_NOM: {
        V *v = scope_lire(sc, e->s, e->slen);
        if (v)
            return v;
        v = eval_natif_ou_hote(e->s, e->slen);
        if (v)
            return v;
        defaut("nom", -1, "nom inconnu: %s", e->s);
    }
    case N_SUITE: {
        int i;
        V **it = (V **)xmalloc(sizeof(V *) * (size_t)(e->nitems > 0 ? e->nitems : 1));
        for (i = 0; i < e->nitems; i++)
            it[i] = eval_expr(e->items[i], sc);
        return v_suite(it, e->nitems);
    }
    case N_FICHE: {
        V *f = v_fiche();
        int i;
        for (i = 0; i < e->npairs; i++)
            fiche_set(f, e->keys[i], e->keylens[i], eval_expr(e->vals[i], sc));
        return f;
    }
    case N_ELEMENT: {
        V *coll = eval_expr(e->y, sc);
        V *idx = eval_expr(e->x, sc);
        return indexer(coll, idx);
    }
    case N_CHAMP: {
        V *f = eval_expr(e->y, sc);
        V *cle = eval_expr(e->x, sc);
        if (f->t != V_FICHE)
            defaut("forme", -1, "champ de: fiche attendue");
        if (cle->t != V_TEXTE)
            defaut("forme", -1, "champ de: clé texte attendue");
        {
            int i = fiche_index(f, cle->u.s.p, cle->u.s.len);
            if (i < 0)
                defaut("forme", -1, "champ absent: %s", cle->u.s.p);
            return f->u.fi.v[i];
        }
    }
    case N_UN: {
        V *v = eval_expr(e->x, sc);
        if (e->op == 'n')
            return verite(v) ? v_non() : v_oui();
        if (v->t != V_NOMBRE)
            defaut("forme", -1, "moins d'un non-nombre");
        return v_nombre(-v->u.n);
    }
    case N_ET: {
        V *a = eval_expr(e->x, sc);
        if (!verite(a))
            return v_non();
        return verite(eval_expr(e->y, sc)) ? v_oui() : v_non();
    }
    case N_OU: {
        V *a = eval_expr(e->x, sc);
        if (verite(a))
            return v_oui();
        return verite(eval_expr(e->y, sc)) ? v_oui() : v_non();
    }
    case N_BIN: {
        V *a = eval_expr(e->x, sc);
        V *b = eval_expr(e->y, sc);
        return binaire(e->op, a, b);
    }
    case N_CMP: {
        V *a = eval_expr(e->x, sc);
        V *b = eval_expr(e->y, sc);
        return comparer(e->op, a, b);
    }
    case N_APPEL: {
        int i;
        V **args = (V **)xmalloc(sizeof(V *) * (size_t)(e->nitems > 0 ? e->nitems : 1));
        V *f;
        for (i = 0; i < e->nitems; i++)
            args[i] = eval_expr(e->items[i], sc);
        f = scope_lire(sc, e->s, e->slen);
        if (!f) {
            f = eval_natif_ou_hote(e->s, e->slen);
            if (!f)
                defaut("nom", -1, "nom inconnu: %s", e->s);
        }
        return invoquer(f, args, e->nitems);
    }
    }
    defaut("calcul", -1, "expression inconnue");
    return NULL;
}

static V *invoquer(V *f, V **args, int nargs)
{
    if (f->t == V_NATIF) {
        Natif *nd = f->u.nat;
        if (nd->nargs >= 0 && nd->nargs != nargs)
            defaut("hôte", -1, "%s: %s() takes %d positional argument%s but %d were given",
                   nd->nom, nd->pyname ? nd->pyname : nd->nom, nd->nargs,
                   nd->nargs == 1 ? "" : "s", nargs);
        return nd->fn(args, nargs);
    }
    if (f->t == V_GESTE) {
        Geste *g = f->u.g;
        Scope *p;
        XS s;
        int i;
        if (nargs != g->nparams)
            defaut("forme", -1, "%s attend %d argument(s), reçu %d", g->nom, g->nparams, nargs);
        g_profondeur++;
        if (g_profondeur > 400) {
            g_profondeur--;
            defaut("calcul", -1, "pile trop profonde");
        }
        p = scope_new(g->portee);
        for (i = 0; i < nargs; i++)
            scope_def(p, g->params[i], g->plens[i], args[i]);
        s = exec_bloc(g->corps, p);
        g_profondeur--;
        if (s == ST_RENDS)
            return g_retour;
        return v_vide();
    }
    defaut("forme", -1, "valeur non appelable");
    return NULL;
}

static XS exec_bloc(Node *bloc, Scope *sc)
{
    int i;
    for (i = 0; i < bloc->nitems; i++) {
        V *v = NULL;
        XS s = exec_stmt(bloc->items[i], sc, &v);
        if (s != ST_OK)
            return s;
    }
    return ST_OK;
}
static XS exec_stmt(Node *st, Scope *sc, V **out)
{
    if (out)
        *out = NULL;
    switch (st->kind) {
    case S_EXPR: {
        V *v = eval_expr(st->x, sc);
        if (out)
            *out = v;
        return ST_OK;
    }
    case S_LIER:
        scope_def(sc, st->s, st->slen, eval_expr(st->x, sc));
        return ST_OK;
    case S_RELIER: {
        V *v = eval_expr(st->x, sc);
        V **slot = scope_trouver(sc, st->s, st->slen);
        if (!slot)
            defaut("nom", st->line, "reliaison d'un nom inconnu: %s", st->s);
        *slot = v;
        return ST_OK;
    }
    case S_AFFICHER: {
        Buf b;
        buf_init(&b);
        texte_de(eval_expr(st->x, sc), &b, 0);
        sortie_ecrire(buf_c(&b), b.len);
        return ST_OK;
    }
    case S_RENDS:
        g_retour = eval_expr(st->x, sc);
        return ST_RENDS;
    case S_SILENCE:
        return ST_OK;
    case S_SI: {
        int i;
        for (i = 0; i < st->nitems; i++) {
            if (verite(eval_expr(st->items[i], sc)))
                return exec_bloc(st->vals[i], scope_new(sc));
        }
        if (st->z)
            return exec_bloc(st->z, scope_new(sc));
        return ST_OK;
    }
    case S_TANTQUE: {
        long long tours = 0;
        for (;;) {
            CtrlJmp cj;
            volatile long long sp;
            volatile int sig;
            volatile int s = ST_OK;
            if (!verite(eval_expr(st->x, sc)))
                break;
            tours++;
            if (tours > 2000000)
                defaut("calcul", st->line, "boucle trop longue");
            cj.prev = g_ctrl_top;
            g_ctrl_top = &cj;
            sp = g_profondeur;
            sig = setjmp(cj.jb);
            if (sig == 0)
                s = (int)exec_bloc(st->y, scope_new(sc));
            g_ctrl_top = cj.prev;
            g_profondeur = sp;
            if (sig == 1)
                break;
            if (sig == 2)
                continue;
            if (s != ST_OK)
                return (XS)s;
        }
        return ST_OK;
    }
    case S_POURCHAQUE: {
        V *seq = eval_expr(st->x, sc);
        if (seq->t == V_TEXTE) {
            Grapheme *g;
            int n = graphemes(seq->u.s.p, seq->u.s.len, &g);
            V **it = (V **)xmalloc(sizeof(V *) * (size_t)(n > 0 ? n : 1));
            int i;
            for (i = 0; i < n; i++)
                it[i] = v_texte_n(seq->u.s.p + g[i].start, g[i].len);
            seq = v_suite(it, n);
        }
        if (seq->t != V_SUITE)
            defaut("forme", st->line, "pour chaque attend une suite");
        {
            int i;
            for (i = 0; i < seq->u.su.n; i++) {
                Scope *p = scope_new(sc);
                CtrlJmp cj;
                volatile long long sp;
                volatile int sig;
                volatile int s = ST_OK;
                scope_def(p, st->s, st->slen, seq->u.su.it[i]);
                cj.prev = g_ctrl_top;
                g_ctrl_top = &cj;
                sp = g_profondeur;
                sig = setjmp(cj.jb);
                if (sig == 0)
                    s = (int)exec_bloc(st->y, p);
                g_ctrl_top = cj.prev;
                g_profondeur = sp;
                if (sig == 1)
                    break;
                if (sig == 2)
                    continue;
                if (s != ST_OK)
                    return (XS)s;
            }
        }
        return ST_OK;
    }
    case S_DEFINIR: {
        Geste *g = (Geste *)xcalloc(1, sizeof(Geste));
        g->nom = xstrndup(st->s, st->slen);
        g->nomlen = st->slen;
        g->params = st->keys;
        g->plens = st->keylens;
        g->nparams = st->npairs;
        g->corps = st->y;
        g->ncorps = st->y ? st->y->nitems : 0;
        g->portee = sc;
        {
            V *v = v_new();
            v->t = V_GESTE;
            v->u.g = g;
            scope_def(sc, st->s, st->slen, v);
        }
        return ST_OK;
    }
    case S_BRISE:
        if (g_ctrl_top)
            longjmp(g_ctrl_top->jb, 1);
        return ST_BRISE;
    case S_PASSE:
        if (g_ctrl_top)
            longjmp(g_ctrl_top->jb, 2);
        return ST_PASSE;
    }
    defaut("calcul", st->line, "instruction inconnue");
    return ST_OK;
}

/* 模块求值：返回模块结果，并把全局作用域带出 */
static V *eval_programme_dans(Node *prog, Scope **out_scope, Scope *base)
{
    Scope *g = base ? base : scope_new(NULL);
    V *resultat = NULL;
    Node *dernier = NULL;
    int i;
    g_profondeur = 0;
    for (i = 0; i < prog->nitems; i++) {
        Node *st = prog->items[i];
        V *v = NULL;
        XS s = exec_stmt(st, g, &v);
        if (s == ST_RENDS) {
            V *r = g_retour ? g_retour : v_vide();
            if (out_scope)
                *out_scope = g;
            return r;
        }
        if (s == ST_BRISE)
            defaut("calcul", st->line, "arrête hors d'une boucle");
        if (s == ST_PASSE)
            defaut("calcul", st->line, "passe hors d'une boucle");
        if (v)
            resultat = v;
        dernier = st;
    }
    if (dernier && dernier->kind == S_SILENCE)
        resultat = NULL;
    if (out_scope)
        *out_scope = g;
    return resultat ? resultat : v_vide();
}

static V *eval_programme(Node *prog, Scope **out_scope)
{
    return eval_programme_dans(prog, out_scope, NULL);
}

/* 在已有全局作用域上执行，供交互式解释器逐句累积定义 */
static V *eval_source_scope(const char *src, int len, Scope **scope)
{
    Node *prog = analyser(src, len);
    return eval_programme_dans(prog, scope, *scope);
}

/* 调函数（RPC call / atelier.lancer）：返回值内部形态 */
static V *appeler_fonction(Scope *g, const char *nom, V **args, int nargs)
{
    int len = (int)strlen(nom);
    V *f = scope_lire(g, nom, len);
    long long save = g_profondeur;
    V *r;
    if (!f)
        defaut("nom", -1, "geste inconnu: %s", nom);
    g_profondeur = 0;
    r = invoquer(f, args, nargs);
    g_profondeur = save;
    return r;
}

/* ================================================================== */
/* 内置函数（18 个，介词式）                                            */
/* ================================================================== */
static int cp_lower(int cp)
{
    if (cp >= 'A' && cp <= 'Z')
        return cp + 32;
    if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7)
        return cp + 0x20;
    if (cp == 0x0178)
        return 0x00FF;
    if (cp >= 0x0100 && cp <= 0x012F && (cp % 2 == 0))
        return cp + 1;
    if (cp >= 0x01CD && cp <= 0x01DC && (cp % 2 == 1))
        return cp + 1;
    if (cp >= 0x01DE && cp <= 0x01EE && (cp % 2 == 0))
        return cp + 1;
    if (cp == 0x01F4)
        return 0x01F5;
    if (cp >= 0x01F8 && cp <= 0x021F && (cp % 2 == 0))
        return cp + 1;
    if (cp >= 0x0222 && cp <= 0x0233 && (cp % 2 == 0))
        return cp + 1;
    if (cp >= 0x0132 && cp <= 0x0137 && (cp % 2 == 0))
        return cp + 1;
    if (cp >= 0x0139 && cp <= 0x0148 && (cp % 2 == 1))
        return cp + 1;
    if (cp >= 0x014A && cp <= 0x0177 && (cp % 2 == 0))
        return cp + 1;
    if (cp >= 0x0179 && cp <= 0x017E && (cp % 2 == 1))
        return cp + 1;
    if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2)
        return cp + 0x20;
    if (cp == 0x03C2)
        return 0x03C2;
    if (cp >= 0x0410 && cp <= 0x042F)
        return cp + 0x20;
    if (cp >= 0x0400 && cp <= 0x040F)
        return cp + 0x50;
    return cp;
}
static int cp_upper(int cp)
{
    if (cp >= 'a' && cp <= 'z')
        return cp - 32;
    if (cp >= 0x00E0 && cp <= 0x00FE && cp != 0x00F7)
        return cp - 0x20;
    if (cp == 0x00FF)
        return 0x0178;
    if (cp >= 0x0101 && cp <= 0x012F && (cp % 2 == 1))
        return cp - 1;
    if (cp >= 0x01CE && cp <= 0x01DC && (cp % 2 == 0))
        return cp - 1;
    if (cp >= 0x01DF && cp <= 0x01EF && (cp % 2 == 1))
        return cp - 1;
    if (cp == 0x01F5)
        return 0x01F4;
    if (cp >= 0x01F9 && cp <= 0x021F && (cp % 2 == 1))
        return cp - 1;
    if (cp >= 0x0223 && cp <= 0x0233 && (cp % 2 == 1))
        return cp - 1;
    if (cp >= 0x0133 && cp <= 0x0137 && (cp % 2 == 1))
        return cp - 1;
    if (cp >= 0x013A && cp <= 0x0148 && (cp % 2 == 0))
        return cp - 1;
    if (cp >= 0x014B && cp <= 0x0177 && (cp % 2 == 1))
        return cp - 1;
    if (cp >= 0x017A && cp <= 0x017E && (cp % 2 == 0))
        return cp - 1;
    if (cp >= 0x03B1 && cp <= 0x03C9)
        return cp - 0x20;
    if (cp == 0x03C2)
        return 0x03A3;
    if (cp >= 0x0430 && cp <= 0x044F)
        return cp - 0x20;
    if (cp >= 0x0450 && cp <= 0x045F)
        return cp - 0x50;
    return cp;
}
static V *texte_case(V *t, int maj)
{
    Buf b;
    int i = 0;
    if (!est_texte(t))
        defaut("forme", -1, "%s attend un texte", maj ? "haussé" : "abaissé");
    buf_init(&b);
    while (i < t->u.s.len) {
        int cp, k = u8_decode((const unsigned char *)t->u.s.p + i, t->u.s.len - i, &cp);
        if (maj && cp == 0x00DF) {
            buf_puts(&b, "SS");
        } else if (maj && cp == 0x017F) {
            buf_putc(&b, 'S');
        } else {
            buf_putcp(&b, maj ? cp_upper(cp) : cp_lower(cp));
        }
        i += k;
    }
    {
        V *v = v_texte_n(buf_c(&b), b.len);
        return v;
    }
}

static V *b_mesure(V **a, int n)
{
    (void)n;
    if (a[0]->t == V_TEXTE) {
        Grapheme *g;
        int k = graphemes(a[0]->u.s.p, a[0]->u.s.len, &g);
        return v_nombre((i128)k * ECHELLE);
    }
    if (a[0]->t == V_SUITE)
        return v_nombre((i128)a[0]->u.su.n * ECHELLE);
    if (a[0]->t == V_FICHE)
        return v_nombre((i128)a[0]->u.fi.n * ECHELLE);
    defaut("forme", -1, "mesure attend un texte, une suite ou une fiche");
    return NULL;
}
static V *b_texte(V **a, int n)
{
    Buf b;
    (void)n;
    buf_init(&b);
    texte_de(a[0], &b, 0);
    {
        V *v = v_texte_n(buf_c(&b), b.len);
        return v;
    }
}
static V *b_nombre(V **a, int n)
{
    i128 v;
    (void)n;
    if (est_nombre(a[0]))
        return a[0];
    if (est_texte(a[0])) {
        if (!nombre_depuis_texte(a[0]->u.s.p, a[0]->u.s.len, &v))
            defaut("calcul", -1, "en-nombre: « %s » n'est pas un nombre", a[0]->u.s.p);
        if (i128_abs(v) > NOTE_MAX_I128)
            defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
        return v_nombre(v);
    }
    defaut("calcul", -1, "en-nombre attend un texte");
    return NULL;
}
static V *b_egalise(V **a, int n)
{
    (void)n;
    return a[0];
}
static V *b_abaisse(V **a, int n)
{
    (void)n;
    return texte_case(a[0], 0);
}
static V *b_hausse(V **a, int n)
{
    (void)n;
    return texte_case(a[0], 1);
}
static V *b_decoupe(V **a, int n)
{
    (void)n;
    if (!est_texte(a[0]))
        defaut("forme", -1, "découpe attend un texte");
    if (a[1]->t == V_VIDE) {
        Grapheme *g;
        int k = graphemes(a[0]->u.s.p, a[0]->u.s.len, &g);
        V **it = (V **)xmalloc(sizeof(V *) * (size_t)(k > 0 ? k : 1));
        int i;
        for (i = 0; i < k; i++)
            it[i] = v_texte_n(a[0]->u.s.p + g[i].start, g[i].len);
        return v_suite(it, k);
    }
    if (!est_texte(a[1]))
        defaut("forme", -1, "découpe: séparateur texte attendu");
    if (a[1]->u.s.len == 0) {
        Grapheme *g;
        int k = graphemes(a[0]->u.s.p, a[0]->u.s.len, &g);
        V **it = (V **)xmalloc(sizeof(V *) * (size_t)(k > 0 ? k : 1));
        int i;
        for (i = 0; i < k; i++)
            it[i] = v_texte_n(a[0]->u.s.p + g[i].start, g[i].len);
        return v_suite(it, k);
    }
    /* 按子串切分（保留空段） */
    {
        const char *s = a[0]->u.s.p;
        int slen = a[0]->u.s.len;
        const char *sep = a[1]->u.s.p;
        int seplen = a[1]->u.s.len;
        int i = 0, start = 0, cap = 8, cnt = 0;
        V **it = (V **)xmalloc(sizeof(V *) * (size_t)cap);
        while (i + seplen <= slen) {
            if (memcmp(s + i, sep, (size_t)seplen) == 0) {
                if (cnt == cap) {
                    cap *= 2;
                    it = (V **)xrealloc(it, sizeof(V *) * (size_t)cap);
                }
                it[cnt++] = v_texte_n(s + start, i - start);
                i += seplen;
                start = i;
                continue;
            }
            i++;
        }
        if (cnt == cap) {
            cap *= 2;
            it = (V **)xrealloc(it, sizeof(V *) * (size_t)cap);
        }
        it[cnt++] = v_texte_n(s + start, slen - start);
        return v_suite(it, cnt);
    }
}
static V *b_recoud(V **a, int n)
{
    Buf b;
    int i;
    (void)n;
    if (a[0]->t != V_SUITE)
        defaut("forme", -1, "recoud attend une suite");
    if (!est_texte(a[1]))
        defaut("forme", -1, "recoud: séparateur texte attendu");
    buf_init(&b);
    for (i = 0; i < a[0]->u.su.n; i++) {
        if (i)
            buf_putn(&b, a[1]->u.s.p, a[1]->u.s.len);
        texte_de(a[0]->u.su.it[i], &b, 0);
    }
    {
        V *v = v_texte_n(buf_c(&b), b.len);
        return v;
    }
}
static V *b_renferme(V **a, int n)
{
    (void)n;
    if (a[0]->t == V_TEXTE) {
        if (a[1]->t != V_TEXTE)
            defaut("forme", -1, "renferme: sous-texte attendu");
        if (a[1]->u.s.len == 0)
            return v_oui();
        {
            int i;
            for (i = 0; i + a[1]->u.s.len <= a[0]->u.s.len; i++)
                if (memcmp(a[0]->u.s.p + i, a[1]->u.s.p, (size_t)a[1]->u.s.len) == 0)
                    return v_oui();
            return v_non();
        }
    }
    if (a[0]->t == V_SUITE) {
        int i;
        for (i = 0; i < a[0]->u.su.n; i++)
            if (egal(a[0]->u.su.it[i], a[1]))
                return v_oui();
        return v_non();
    }
    if (a[0]->t == V_FICHE) {
        if (a[1]->t == V_TEXTE && fiche_index(a[0], a[1]->u.s.p, a[1]->u.s.len) >= 0)
            return v_oui();
        return v_non();
    }
    defaut("forme", -1, "renferme: valeur non contenante");
    return NULL;
}
static V *b_clefs(V **a, int n)
{
    int i;
    (void)n;
    if (a[0]->t != V_FICHE)
        defaut("forme", -1, "clefs attend une fiche");
    {
        V **it = (V **)xmalloc(sizeof(V *) * (size_t)(a[0]->u.fi.n > 0 ? a[0]->u.fi.n : 1));
        for (i = 0; i < a[0]->u.fi.n; i++)
            it[i] = a[0]->u.fi.k[i];
        return v_suite(it, a[0]->u.fi.n);
    }
}
static V *b_teneurs(V **a, int n)
{
    int i;
    (void)n;
    if (a[0]->t != V_FICHE)
        defaut("forme", -1, "teneurs attend une fiche");
    {
        V **it = (V **)xmalloc(sizeof(V *) * (size_t)(a[0]->u.fi.n > 0 ? a[0]->u.fi.n : 1));
        for (i = 0; i < a[0]->u.fi.n; i++)
            it[i] = a[0]->u.fi.v[i];
        return v_suite(it, a[0]->u.fi.n);
    }
}
static V *b_ajoute_fin(V **a, int n)
{
    (void)n;
    if (a[0]->t != V_SUITE)
        defaut("forme", -1, "ajoute-fin attend une suite");
    {
        int k = a[0]->u.su.n;
        V **it = (V **)xmalloc(sizeof(V *) * (size_t)(k + 1));
        int i;
        for (i = 0; i < k; i++)
            it[i] = a[0]->u.su.it[i];
        it[k] = a[1];
        return v_suite(it, k + 1);
    }
}
static V *b_bribe(V **a, int n)
{
    long long de, fi, nn;
    (void)n;
    if (a[0]->t != V_TEXTE && a[0]->t != V_SUITE)
        defaut("forme", -1, "bribe attend un texte ou une suite");
    de = entier_requis(a[1], "indice entier attendu");
    fi = entier_requis(a[2], "indice entier attendu");
    if (a[0]->t == V_TEXTE) {
        Grapheme *g;
        int k = graphemes(a[0]->u.s.p, a[0]->u.s.len, &g);
        Buf b;
        nn = k;
        de = de > nn ? nn : de;
        if (de < 1)
            de = 1;
        fi = fi < 0 ? 0 : (fi > nn ? nn : fi);
        if (de > fi)
            return v_texte_n("", 0);
        buf_init(&b);
        {
            int i;
            for (i = (int)de - 1; i < (int)fi; i++)
                buf_putn(&b, a[0]->u.s.p + g[i].start, g[i].len);
        }
        {
            V *v = v_texte_n(buf_c(&b), b.len);
            return v;
        }
    }
    nn = a[0]->u.su.n;
    de = de > nn ? nn : de;
    if (de < 1)
        de = 1;
    fi = fi < 0 ? 0 : (fi > nn ? nn : fi);
    if (de > fi)
        return v_suite(NULL, 0);
    {
        int cnt = (int)(fi - de + 1), i;
        V **it = (V **)xmalloc(sizeof(V *) * (size_t)(cnt > 0 ? cnt : 1));
        for (i = 0; i < cnt; i++)
            it[i] = a[0]->u.su.it[(int)de - 1 + i];
        return v_suite(it, cnt);
    }
}
static int cmp_ordonne(V *a, V *b)
{
    int ca, cb;
    if (a->t == V_OUI || a->t == V_NON)
        ca = 0;
    else if (a->t == V_NOMBRE)
        ca = 0;
    else if (a->t == V_TEXTE)
        ca = 1;
    else
        ca = 2;
    if (b->t == V_OUI || b->t == V_NON)
        cb = 0;
    else if (b->t == V_NOMBRE)
        cb = 0;
    else if (b->t == V_TEXTE)
        cb = 1;
    else
        cb = 2;
    if (ca != cb)
        return ca < cb ? -1 : 1;
    if (ca == 0) {
        i128 x, y;
        if (a->t == V_NOMBRE)
            x = a->u.n;
        else
            x = (a->t == V_OUI) ? 1 : 0;
        if (b->t == V_NOMBRE)
            y = b->u.n;
        else
            y = (b->t == V_OUI) ? 1 : 0;
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (ca == 1) {
        int nn = a->u.s.len < b->u.s.len ? a->u.s.len : b->u.s.len;
        int c = memcmp(a->u.s.p, b->u.s.p, (size_t)nn);
        if (c)
            return c < 0 ? -1 : 1;
        return a->u.s.len < b->u.s.len ? -1 : (a->u.s.len > b->u.s.len ? 1 : 0);
    }
    {
        Buf ba, bb;
        int c;
        buf_init(&ba);
        buf_init(&bb);
        texte_de(a, &ba, 0);
        texte_de(b, &bb, 0);
        {
            int nn = ba.len < bb.len ? ba.len : bb.len;
            c = memcmp(buf_c(&ba), buf_c(&bb), (size_t)nn);
            if (c)
                return c < 0 ? -1 : 1;
            return ba.len < bb.len ? -1 : (ba.len > bb.len ? 1 : 0);
        }
    }
}
static V *b_ordonne(V **a, int n)
{
    int i, j;
    (void)n;
    if (a[0]->t != V_SUITE)
        defaut("forme", -1, "ordonne attend une suite");
    {
        int k = a[0]->u.su.n;
        V **it = (V **)xmalloc(sizeof(V *) * (size_t)(k > 0 ? k : 1));
        for (i = 0; i < k; i++)
            it[i] = a[0]->u.su.it[i];
        for (i = 1; i < k; i++) {
            V *x = it[i];
            j = i - 1;
            while (j >= 0 && cmp_ordonne(it[j], x) > 0) {
                it[j + 1] = it[j];
                j--;
            }
            it[j + 1] = x;
        }
        return v_suite(it, k);
    }
}
static V *b_total(V **a, int n)
{
    i128 s = 0;
    int i;
    (void)n;
    if (a[0]->t != V_SUITE)
        defaut("forme", -1, "total attend une suite");
    for (i = 0; i < a[0]->u.su.n; i++) {
        V *x = a[0]->u.su.it[i];
        i128 r;
        if (!est_nombre(x))
            defaut("forme", -1, "total: suite de nombres attendue");
        if (__builtin_add_overflow(s, x->u.n, &r))
            defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
        s = r;
    }
    if (i128_abs(s) > NOTE_MAX_I128)
        defaut("calcul", -1, "nombre hors limites (|x| > 1e15)");
    return v_nombre(s);
}
static V *b_tordu(V **a, int n)
{
    V *o;
    (void)n;
    if (a[0]->t != V_FICHE)
        defaut("forme", -1, "tordu attend une fiche");
    if (a[1]->t != V_TEXTE)
        defaut("forme", -1, "tordu: clé texte attendue");
    o = v_fiche_copie(a[0]);
    fiche_set(o, a[1]->u.s.p, a[1]->u.s.len, a[2]);
    return o;
}
static V *b_fige(V **a, int n)
{
    Buf b;
    (void)n;
    buf_init(&b);
    json_dump(a[0], &b, -1, 0, 0);
    {
        V *v = v_texte_n(buf_c(&b), b.len);
        return v;
    }
}
static V *b_absolu(V **a, int n)
{
    (void)n;
    if (!est_nombre(a[0]))
        defaut("forme", -1, "absolu attend un nombre");
    return a[0]->u.n < 0 ? v_nombre(-a[0]->u.n) : a[0];
}

static Natif NATIFS[] = {
    {"mesure", b_mesure, 1, "_b_mesure"},
    {"en-texte", b_texte, 1, "_b_texte"},
    {"en-nombre", b_nombre, 1, "_b_nombre"},
    {"égalise", b_egalise, 1, "_b_égalise"},
    {"abaissé", b_abaisse, 1, "_b_abaissé"},
    {"haussé", b_hausse, 1, "_b_haussé"},
    {"découpe", b_decoupe, 2, "_b_découpe"},
    {"recoud", b_recoud, 2, "_b_recoud"},
    {"renferme", b_renferme, 2, "_b_renferme"},
    {"clefs", b_clefs, 1, "_b_clefs"},
    {"teneurs", b_teneurs, 1, "_b_teneurs"},
    {"ajoute-fin", b_ajoute_fin, 2, "_b_ajoute_fin"},
    {"bribe", b_bribe, 3, "_b_bribe"},
    {"ordonne", b_ordonne, 1, "_b_ordonne"},
    {"total", b_total, 1, "_b_total"},
    {"tordu", b_tordu, 3, "_b_tordu"},
    {"fige", b_fige, 1, "_b_fige"},
    {"absolu", b_absolu, 1, "_b_absolu"},
    {NULL, NULL, 0, NULL}};

static Natif *natifs_lookup(const char *nom, int len)
{
    int i;
    for (i = 0; NATIFS[i].nom; i++)
        if ((int)strlen(NATIFS[i].nom) == len && memcmp(NATIFS[i].nom, nom, (size_t)len) == 0)
            return &NATIFS[i];
    return NULL;
}

/* ================================================================== */
/* JSON 解析（存储 / RPC 用）                                           */
/* ================================================================== */
typedef struct {
    const char *s;
    int n, i;
} JP;

static void jp_ws(JP *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->i++;
        else
            break;
    }
}
static void utf8_put_cp(Buf *b, int cp)
{
    buf_putcp(b, cp);
}
static V *jp_string(JP *p, int *ok)
{
    Buf b;
    buf_init(&b);
    if (p->i >= p->n || p->s[p->i] != '"') {
        *ok = 0;
        return NULL;
    }
    p->i++;
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') {
            p->i++;
            return v_texte_n(buf_c(&b), b.len);
        }
        if (c == '\\') {
            char e;
            p->i++;
            if (p->i >= p->n) {
                *ok = 0;
                return NULL;
            }
            e = p->s[p->i++];
            switch (e) {
            case '"': buf_putc(&b, '"'); break;
            case '\\': buf_putc(&b, '\\'); break;
            case '/': buf_putc(&b, '/'); break;
            case 'b': buf_putc(&b, '\b'); break;
            case 'f': buf_putc(&b, '\f'); break;
            case 'n': buf_putc(&b, '\n'); break;
            case 'r': buf_putc(&b, '\r'); break;
            case 't': buf_putc(&b, '\t'); break;
            case 'u': {
                unsigned cp = 0;
                int k;
                for (k = 0; k < 4; k++) {
                    char h;
                    if (p->i >= p->n) {
                        *ok = 0;
                        return NULL;
                    }
                    h = p->s[p->i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else { *ok = 0; return NULL; }
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 5 < p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                    unsigned lo = 0;
                    int k2, good = 1;
                    int save = p->i;
                    p->i += 2;
                    for (k2 = 0; k2 < 4; k2++) {
                        char h = p->s[p->i++];
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                        else { good = 0; break; }
                    }
                    if (good && lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        p->i = save;
                }
                utf8_put_cp(&b, (int)cp);
                break;
            }
            default:
                *ok = 0;
                return NULL;
            }
            continue;
        }
        buf_putc(&b, (char)c);
        p->i++;
    }
    *ok = 0;
    return NULL;
}
static V *jp_value(JP *p, int *ok)
{
    jp_ws(p);
    if (p->i >= p->n) {
        *ok = 0;
        return NULL;
    }
    {
        char c = p->s[p->i];
        if (c == '{') {
            V *f = v_fiche();
            p->i++;
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == '}') {
                p->i++;
                return f;
            }
            for (;;) {
                V *k, *v;
                jp_ws(p);
                k = jp_string(p, ok);
                if (!*ok)
                    return NULL;
                jp_ws(p);
                if (p->i >= p->n || p->s[p->i] != ':') {
                    *ok = 0;
                    return NULL;
                }
                p->i++;
                v = jp_value(p, ok);
                if (!*ok)
                    return NULL;
                fiche_set(f, k->u.s.p, k->u.s.len, v);
                jp_ws(p);
                if (p->i < p->n && p->s[p->i] == ',') {
                    p->i++;
                    continue;
                }
                if (p->i < p->n && p->s[p->i] == '}') {
                    p->i++;
                    return f;
                }
                *ok = 0;
                return NULL;
            }
        }
        if (c == '[') {
            int cap = 8, cnt = 0;
            V **it = (V **)xmalloc(sizeof(V *) * (size_t)cap);
            p->i++;
            jp_ws(p);
            if (p->i < p->n && p->s[p->i] == ']') {
                p->i++;
                return v_suite(it, 0);
            }
            for (;;) {
                V *v = jp_value(p, ok);
                if (!*ok)
                    return NULL;
                if (cnt == cap) {
                    cap *= 2;
                    it = (V **)xrealloc(it, sizeof(V *) * (size_t)cap);
                }
                it[cnt++] = v;
                jp_ws(p);
                if (p->i < p->n && p->s[p->i] == ',') {
                    p->i++;
                    continue;
                }
                if (p->i < p->n && p->s[p->i] == ']') {
                    p->i++;
                    return v_suite(it, cnt);
                }
                *ok = 0;
                return NULL;
            }
        }
        if (c == '"')
            return jp_string(p, ok);
        if (c == 't' && p->i + 4 <= p->n && memcmp(p->s + p->i, "true", 4) == 0) {
            p->i += 4;
            return v_oui();
        }
        if (c == 'f' && p->i + 5 <= p->n && memcmp(p->s + p->i, "false", 5) == 0) {
            p->i += 5;
            return v_non();
        }
        if (c == 'n' && p->i + 4 <= p->n && memcmp(p->s + p->i, "null", 4) == 0) {
            p->i += 4;
            return v_vide();
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            int start = p->i;
            i128 v;
            if (c == '-')
                p->i++;
            while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
                p->i++;
            if (p->i < p->n && p->s[p->i] == '.') {
                p->i++;
                while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
                    p->i++;
            }
            if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
                p->i++;
                if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-'))
                    p->i++;
                while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
                    p->i++;
            }
            if (!json_num_to_fixed(p->s + start, p->i - start, &v)) {
                *ok = 0;
                return NULL;
            }
            if (i128_abs(v) > NOTE_MAX_I128) {
                *ok = 0;
                return NULL;
            }
            return v_nombre(v);
        }
    }
    *ok = 0;
    return NULL;
}
/* 解析完整 JSON 文本；失败返回 NULL */
static V *json_parse(const char *s, int len)
{
    JP p;
    int ok = 1;
    V *v;
    p.s = s;
    p.n = len;
    p.i = 0;
    v = jp_value(&p, &ok);
    if (!ok)
        return NULL;
    jp_ws(&p);
    if (p.i != p.n)
        return NULL;
    return v;
}

/* ================================================================== */
/* 文件系统（UTF-8 路径 -> 宽字符）                                     */
/* ================================================================== */
static wchar_t *u8_to_w(const char *s, int len)
{
    int n;
    wchar_t *w;
    if (len < 0)
        len = (int)strlen(s);
    n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    w = (wchar_t *)xmalloc(sizeof(wchar_t) * (size_t)(n + 1));
    MultiByteToWideChar(CP_UTF8, 0, s, len, w, n);
    w[n] = 0;
    return w;
}
static char *w_to_u8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0)
        return xstrdup_c("");
    s = (char *)xmalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    s[n - 1] = 0;
    return s;
}
static int fichier_lire(const char *path_utf8, char **out, int *outlen)
{
    wchar_t *w = u8_to_w(path_utf8, -1);
    FILE *f = _wfopen(w, L"rb");
    long sz;
    char *buf;
    int n, i, j;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0)
        sz = 0;
    buf = (char *)xmalloc((size_t)sz + 1);
    n = (int)fread(buf, 1, (size_t)sz, f);
    fclose(f);
    /* BOM */
    i = 0;
    if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF)
        i = 3;
    /* 换行归一 */
    {
        char *dst = (char *)xmalloc((size_t)(n - i) + 1);
        j = 0;
        for (; i < n; i++) {
            if (buf[i] == '\r') {
                dst[j++] = '\n';
                if (i + 1 < n && buf[i + 1] == '\n')
                    i++;
            } else {
                dst[j++] = buf[i];
            }
        }
        dst[j] = 0;
        *out = dst;
        *outlen = j;
    }
    return 1;
}
static int fichier_existe(const char *path_utf8)
{
    wchar_t *w = u8_to_w(path_utf8, -1);
    struct _stat64 st;
    return _wstat64(w, &st) == 0;
}
static long long fichier_mtime(const char *path_utf8)
{
    wchar_t *w = u8_to_w(path_utf8, -1);
    struct _stat64 st;
    if (_wstat64(w, &st) != 0)
        return -1;
    return (long long)st.st_mtime;
}
static int fichier_ecrire(const char *path_utf8, const char *data, int len)
{
    wchar_t *w = u8_to_w(path_utf8, -1);
    wchar_t *wtmp;
    FILE *f;
    Buf tmp;
    buf_init(&tmp);
    buf_printf(&tmp, "%s.%lu.tmp", path_utf8, (unsigned long)GetCurrentProcessId());
    wtmp = u8_to_w(buf_c(&tmp), -1);
    f = _wfopen(wtmp, L"wb");
    if (!f)
        return 0;
    if (len > 0 && fwrite(data, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return 0;
    }
    fclose(f);
    if (!MoveFileExW(wtmp, w, MOVEFILE_REPLACE_EXISTING)) {
        _wremove(wtmp);
        return 0;
    }
    return 1;
}
static char *path_dirname(const char *p)
{
    int i, n = (int)strlen(p);
    for (i = n - 1; i >= 0; i--)
        if (p[i] == '/' || p[i] == '\\')
            return xstrndup(p, i == 0 ? 1 : i);
    return xstrdup_c(".");
}
static char *path_join(const char *a, const char *b)
{
    Buf buf;
    int alen = (int)strlen(a);
    buf_init(&buf);
    buf_putn(&buf, a, alen);
    if (alen > 0 && a[alen - 1] != '/' && a[alen - 1] != '\\')
        buf_putc(&buf, '/');
    buf_puts(&buf, b);
    return buf_take(&buf);
}
static char *path_abs(const char *p)
{
    wchar_t *w = u8_to_w(p, -1);
    wchar_t full[4096];
    if (!_wfullpath(full, w, 4096))
        return xstrdup_c(p);
    return w_to_u8(full);
}

/* ================================================================== */
/* 原生服务：存储 / 时间 / 声音 / 跨程序调用                             */
/* ================================================================== */
static V *g_store = NULL;
static char *g_store_path = NULL;   /* UTF-8 绝对路径 */
static char *g_root = NULL;         /* 项目根 */
static char *g_exe_dir = NULL;

static void store_charger(void)
{
    char *txt = NULL;
    int len = 0;
    V *v;
    g_store = v_fiche();
    if (!g_store_path)
        return;
    if (!fichier_lire(g_store_path, &txt, &len))
        return;
    v = json_parse(txt, len);
    if (v && v->t == V_FICHE)
        g_store = v;
}
static void creer_repertoires(const char *path);

static void store_sauver(void)
{
    Buf b;
    if (!g_store_path)
        return;
    creer_repertoires(path_dirname(g_store_path));
    buf_init(&b);
    json_dump(g_store, &b, -1, 0, 0);
    fichier_ecrire(g_store_path, buf_c(&b), b.len);
}
static V *store_coll(const char *nom, int len)
{
    int i;
    for (i = 0; i < g_store->u.fi.n; i++) {
        V *k = g_store->u.fi.k[i];
        if (k->u.s.len == len && memcmp(k->u.s.p, nom, (size_t)len) == 0)
            return g_store->u.fi.v[i];
    }
    return NULL;
}
/* 数字文本 -> 整数（serve.py 的 _as_int 语义） */
static int as_int(V *v, long long *out)
{
    if (v->t == V_NOMBRE) {
        if (floor_mod(v->u.n, (i128)ECHELLE) != 0)
            return 0;
        *out = (long long)floor_div(v->u.n, (i128)ECHELLE);
        return 1;
    }
    if (v->t == V_TEXTE) {
        i128 x;
        if (!nombre_depuis_texte(v->u.s.p, v->u.s.len, &x))
            return 0;
        if (floor_mod(x, (i128)ECHELLE) != 0)
            return 0;
        *out = (long long)floor_div(x, (i128)ECHELLE);
        return 1;
    }
    return 0;
}
static int fiche_id(V *f, long long *out)
{
    int i;
    if (f->t != V_FICHE)
        return 0;
    i = fiche_index(f, "id", 2);
    if (i < 0)
        return 0;
    return as_int(f->u.fi.v[i], out);
}
static void store_definir(const char *nom, int len, V *val)
{
    int i;
    for (i = 0; i < g_store->u.fi.n; i++) {
        V *k = g_store->u.fi.k[i];
        if (k->u.s.len == len && memcmp(k->u.s.p, nom, (size_t)len) == 0) {
            g_store->u.fi.v[i] = val;
            return;
        }
    }
    fiche_set(g_store, nom, len, val);
}

static V *b_carnet_lister(V **a, int n)
{
    V *c;
    (void)n;
    if (a[0]->t != V_TEXTE)
        defaut("hôte", -1, "carnet.lister: nom de collection texte attendu");
    c = store_coll(a[0]->u.s.p, a[0]->u.s.len);
    if (c && c->t == V_SUITE)
        return v_suite_copie(c->u.su.it, c->u.su.n);
    return v_suite(NULL, 0);
}
static V *b_carnet_lire(V **a, int n)
{
    V *c;
    if (a[0]->t != V_TEXTE)
        defaut("hôte", -1, "carnet.lire: nom de collection texte attendu");
    c = store_coll(a[0]->u.s.p, a[0]->u.s.len);
    if (n >= 2) {
        long long id;
        int i;
        if (!as_int(a[1], &id))
            return v_vide();
        if (!c || c->t != V_SUITE)
            return v_vide();
        for (i = 0; i < c->u.su.n; i++) {
            long long rid;
            if (fiche_id(c->u.su.it[i], &rid) && rid == id)
                return c->u.su.it[i];
        }
        return v_vide();
    }
    return c ? c : v_vide();
}
static V *b_carnet_ecrire(V **a, int n)
{
    V *coll;
    (void)n;
    if (a[0]->t != V_TEXTE)
        defaut("hôte", -1, "carnet.écrire: nom de collection texte attendu");
    coll = store_coll(a[0]->u.s.p, a[0]->u.s.len);
    if (a[1]->t == V_FICHE && coll && coll->t == V_SUITE) {
        long long rid;
        if (fiche_id(a[1], &rid)) {
            int i;
            for (i = 0; i < coll->u.su.n; i++) {
                long long old;
                if (fiche_id(coll->u.su.it[i], &old) && old == rid) {
                    coll->u.su.it[i] = a[1];
                    store_sauver();
                    return a[1];
                }
            }
            {
                int k = coll->u.su.n;
                V **it = (V **)xmalloc(sizeof(V *) * (size_t)(k + 1));
                for (i = 0; i < k; i++)
                    it[i] = coll->u.su.it[i];
                it[k] = a[1];
                coll->u.su.it = it;
                coll->u.su.n = k + 1;
            }
            store_sauver();
            return a[1];
        }
    }
    store_definir(a[0]->u.s.p, a[0]->u.s.len, a[1]);
    store_sauver();
    return a[1];
}
static V *b_carnet_ajouter(V **a, int n)
{
    V *coll;
    int k, i;
    (void)n;
    if (a[0]->t != V_TEXTE)
        defaut("hôte", -1, "carnet.ajouter: nom de collection texte attendu");
    coll = store_coll(a[0]->u.s.p, a[0]->u.s.len);
    if (!coll || coll->t != V_SUITE) {
        V **it = (V **)xmalloc(sizeof(V *));
        it[0] = a[1];
        store_definir(a[0]->u.s.p, a[0]->u.s.len, v_suite(it, 1));
        store_sauver();
        return a[1];
    }
    k = coll->u.su.n;
    {
        V **it = (V **)xmalloc(sizeof(V *) * (size_t)(k + 1));
        for (i = 0; i < k; i++)
            it[i] = coll->u.su.it[i];
        it[k] = a[1];
        coll->u.su.it = it;
        coll->u.su.n = k + 1;
    }
    store_sauver();
    return a[1];
}
static V *b_carnet_compter(V **a, int n)
{
    V *c;
    (void)n;
    if (a[0]->t != V_TEXTE)
        defaut("hôte", -1, "carnet.compter: nom de collection texte attendu");
    c = store_coll(a[0]->u.s.p, a[0]->u.s.len);
    if (c && c->t == V_SUITE)
        return v_nombre((i128)c->u.su.n * ECHELLE);
    return v_nombre(0);
}
static V *b_carnet_vider(V **a, int n)
{
    (void)n;
    if (a[0]->t != V_TEXTE)
        defaut("hôte", -1, "carnet.vider: nom de collection texte attendu");
    store_definir(a[0]->u.s.p, a[0]->u.s.len, v_suite(NULL, 0));
    store_sauver();
    return v_nombre(0);
}
static V *b_carnet_retirer(V **a, int n)
{
    V *coll;
    long long id;
    int i;
    (void)n;
    if (a[0]->t != V_TEXTE)
        defaut("hôte", -1, "carnet.retirer: nom de collection texte attendu");
    if (!as_int(a[1], &id))
        return v_non();
    coll = store_coll(a[0]->u.s.p, a[0]->u.s.len);
    if (!coll || coll->t != V_SUITE)
        return v_non();
    for (i = 0; i < coll->u.su.n; i++) {
        long long rid;
        if (fiche_id(coll->u.su.it[i], &rid) && rid == id) {
            int k = coll->u.su.n;
            V **it = (V **)xmalloc(sizeof(V *) * (size_t)(k > 0 ? k : 1));
            int j = 0, m;
            for (m = 0; m < k; m++)
                if (m != i)
                    it[j++] = coll->u.su.it[m];
            coll->u.su.it = it;
            coll->u.su.n = j;
            store_sauver();
            return v_oui();
        }
    }
    return v_non();
}
static V *b_carnet_fusionner(V **a, int n)
{
    V *coll;
    long long id;
    int i;
    (void)n;
    if (a[0]->t != V_TEXTE)
        defaut("hôte", -1, "carnet.fusionner: nom de collection texte attendu");
    if (!as_int(a[1], &id))
        return v_vide();
    if (a[2]->t != V_FICHE)
        defaut("hôte", -1, "carnet.fusionner: record attendu");
    coll = store_coll(a[0]->u.s.p, a[0]->u.s.len);
    if (!coll || coll->t != V_SUITE)
        return v_vide();
    for (i = 0; i < coll->u.su.n; i++) {
        long long rid;
        if (fiche_id(coll->u.su.it[i], &rid) && rid == id) {
            V *merged = v_fiche_copie(coll->u.su.it[i]);
            int j;
            for (j = 0; j < a[2]->u.fi.n; j++) {
                V *k = a[2]->u.fi.k[j];
                fiche_set(merged, k->u.s.p, k->u.s.len, a[2]->u.fi.v[j]);
            }
            fiche_set(merged, "id", 2, v_nombre((i128)id * ECHELLE));
            coll->u.su.it[i] = merged;
            store_sauver();
            return merged;
        }
    }
    return v_vide();
}

static V *b_horloge(V **a, int n)
{
    time_t t;
    struct tm tmv;
    char buf[64];
    (void)a;
    (void)n;
    t = time(NULL);
    localtime_s(&tmv, &t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return v_texte_c(buf);
}
static V *b_voix(V **a, int n)
{
    Buf b;
    (void)n;
    buf_init(&b);
    texte_de(a[0], &b, 0);
    {
        V *v = v_texte_n(buf_c(&b), b.len);
        return v;
    }
}

/* -- 解释器缓存 + atelier -- */
typedef struct Prog {
    char *path;
    long long mtime;
    Scope *glob;
    Node *prog;
    struct Prog *next;
} Prog;
static Prog *g_progs = NULL;

static int se_termine_par(const char *s, const char *suf)
{
    int n = (int)strlen(s), m = (int)strlen(suf);
    return n >= m && memcmp(s + n - m, suf, (size_t)m) == 0;
}
static char *base_sans_ext(const char *path)
{
    int n = (int)strlen(path), i;
    for (i = n - 1; i >= 0; i--)
        if (path[i] == '/' || path[i] == '\\')
            break;
    i++;
    if (se_termine_par(path + i, ".sil"))
        return xstrndup(path + i, (int)strlen(path + i) - 4);
    return xstrdup_c(path + i);
}
static int nom_fichier_egal(const char *base, const char *nom)
{
    return _stricmp(base, nom) == 0;
}
/* 在目录里递归找 <nom>.sil；找到返回路径（UTF-8），否则 NULL */
static char *chercher_recursif(const char *dir, const char *nom)
{
    wchar_t *wd = u8_to_w(dir, -1);
    wchar_t pat[4096];
    struct _wfinddata64_t fd;
    intptr_t h;
    swprintf(pat, 4096, L"%ls\\*", wd);
    h = _wfindfirst64(pat, &fd);
    if (h < 0)
        return NULL;
    do {
        char *name = w_to_u8(fd.name);
        char *full;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        full = path_join(dir, name);
        if (fd.attrib & _A_SUBDIR) {
            char *r = chercher_recursif(full, nom);
            if (r)
                return r;
            continue;
        }
        {
            char *base = base_sans_ext(name);
            if (nom_fichier_egal(base, nom) && se_termine_par(name, ".sil"))
                return full;
        }
    } while (_wfindnext64(h, &fd) == 0);
    _findclose(h);
    return NULL;
}
static char *resoudre_programme(const char *nom)
{
    char *fname;
    char *cand;
    if (strchr(nom, '/') || strchr(nom, '\\')) {
        if (se_termine_par(nom, ".sil"))
            fname = xstrdup_c(nom);
        else {
            Buf b;
            buf_init(&b);
            buf_puts(&b, nom);
            buf_puts(&b, ".sil");
            fname = buf_take(&b);
        }
        cand = path_join(g_root, fname);
        if (fichier_existe(cand))
            return path_abs(cand);
        return NULL;
    }
    if (se_termine_par(nom, ".sil")) {
        Buf b;
        buf_init(&b);
        buf_putn(&b, nom, (int)strlen(nom) - 4);
        fname = buf_take(&b);
    } else {
        fname = xstrdup_c(nom);
    }
    {
        char *sub = path_join(g_root, "juanzong");
        char *r = chercher_recursif(sub, fname);
        if (r)
            return path_abs(r);
    }
    {
        char *sub = path_join(g_root, "huapu");
        char *r = chercher_recursif(sub, fname);
        if (r)
            return path_abs(r);
    }
    {
        char *sub = path_join(g_root, "程序");
        char *r = chercher_recursif(sub, fname);
        if (r)
            return path_abs(r);
    }
    {
        char *sub = path_join(g_root, "示例");
        char *r = chercher_recursif(sub, fname);
        if (r)
            return path_abs(r);
    }
    cand = path_join(g_root, fname);
    {
        Buf b;
        buf_init(&b);
        buf_puts(&b, cand);
        buf_puts(&b, ".sil");
        cand = buf_take(&b);
    }
    if (fichier_existe(cand))
        return path_abs(cand);
    return NULL;
}
static Prog *prog_obtenir(const char *path)
{
    long long mt = fichier_mtime(path);
    Prog *p;
    char *txt = NULL;
    int len = 0;
    Node *prog;
    Scope *glob = NULL;
    for (p = g_progs; p; p = p->next)
        if (strcmp(p->path, path) == 0 && p->mtime == mt)
            return p;
    if (!fichier_lire(path, &txt, &len))
        return NULL;
    prog = analyser(txt, len);
    {
        long long save = g_profondeur;
        eval_programme(prog, &glob);
        g_profondeur = save;
    }
    p = (Prog *)xcalloc(1, sizeof(Prog));
    p->path = xstrdup_c(path);
    p->mtime = mt;
    p->glob = glob;
    p->prog = prog;
    p->next = g_progs;
    g_progs = p;
    return p;
}
static V *b_atelier_lancer(V **a, int n)
{
    char *path;
    Prog *p;
    (void)n;
    if (a[0]->t != V_TEXTE || a[1]->t != V_TEXTE)
        defaut("hôte", -1, "atelier.lancer: fichier et fonction texte attendus");
    path = resoudre_programme(a[0]->u.s.p);
    if (!path)
        defaut("hôte", -1, "atelier.lancer: program-not-found: %s", a[0]->u.s.p);
    p = prog_obtenir(path);
    if (!p)
        defaut("hôte", -1, "atelier.lancer: program-not-found: %s", a[0]->u.s.p);
    {
        V *arg[1];
        arg[0] = a[2];
        return appeler_fonction(p->glob, a[1]->u.s.p, arg, 1);
    }
}
static V *b_atelier_evaluer(V **a, int n)
{
    Node *prog;
    Scope *glob = NULL;
    V *r;
    long long save;
    (void)n;
    if (a[0]->t != V_TEXTE)
        defaut("hôte", -1, "atelier.évaluer: source texte attendue");
    prog = analyser(a[0]->u.s.p, a[0]->u.s.len);
    save = g_profondeur;
    r = eval_programme(prog, &glob);
    g_profondeur = save;
    return r;
}
static V *b_atelier_noms(V **a, int n)
{
    int cap = 16, cnt = 0;
    V **it = (V **)xmalloc(sizeof(V *) * (size_t)cap);
    (void)a;
    (void)n;
    {
        const char *dirs[4];
        int d;
        dirs[0] = "juanzong";
        dirs[1] = "huapu";
        dirs[2] = "程序";
        dirs[3] = "示例";
        for (d = 0; d < 4; d++) {
            char *sub = path_join(g_root, dirs[d]);
            /* 一层目录 + 文件名 */
            wchar_t *wd = u8_to_w(sub, -1);
            wchar_t pat[4096];
            struct _wfinddata64_t fd;
            intptr_t h;
            swprintf(pat, 4096, L"%ls\\*", wd);
            h = _wfindfirst64(pat, &fd);
            if (h < 0)
                continue;
            do {
                char *name = w_to_u8(fd.name);
                if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                    continue;
                if (fd.attrib & _A_SUBDIR) {
                    char *sub2 = path_join(sub, name);
                    wchar_t *wd2 = u8_to_w(sub2, -1);
                    wchar_t pat2[4096];
                    struct _wfinddata64_t fd2;
                    intptr_t h2;
                    swprintf(pat2, 4096, L"%ls\\*.sil", wd2);
                    h2 = _wfindfirst64(pat2, &fd2);
                    if (h2 >= 0) {
                        do {
                            char *n2 = w_to_u8(fd2.name);
                            char *base = base_sans_ext(n2);
                            if (cnt == cap) {
                                cap *= 2;
                                it = (V **)xrealloc(it, sizeof(V *) * (size_t)cap);
                            }
                            it[cnt++] = v_texte_c(base);
                        } while (_wfindnext64(h2, &fd2) == 0);
                        _findclose(h2);
                    }
                }
            } while (_wfindnext64(h, &fd) == 0);
            _findclose(h);
        }
    }
    return v_suite(it, cnt);
}

static Natif HOTES[] = {
    {"carnet.lister", b_carnet_lister, 1, "lister"},
    {"carnet.lire", b_carnet_lire, -1, "lire"},
    {"carnet.écrire", b_carnet_ecrire, 2, "ecrire"},
    {"carnet.ajouter", b_carnet_ajouter, 2, "ajouter"},
    {"carnet.compter", b_carnet_compter, 1, "compter"},
    {"carnet.vider", b_carnet_vider, 1, "vider"},
    {"carnet.retirer", b_carnet_retirer, 2, "retirer"},
    {"carnet.fusionner", b_carnet_fusionner, 3, "fusionner"},
    {"horloge.maintenant", b_horloge, 0, "maintenant"},
    {"voix.dire", b_voix, 1, "dire"},
    {"voix.signaler", b_voix, 1, "dire"},
    {"atelier.lancer", b_atelier_lancer, 3, "lancer"},
    {"atelier.évaluer", b_atelier_evaluer, 1, "evaluer"},
    {"atelier.noms", b_atelier_noms, 0, "noms"},
    {NULL, NULL, 0, NULL}};

static Natif *hote_lookup(const char *nom, int len)
{
    int i;
    for (i = 0; HOTES[i].nom; i++)
        if ((int)strlen(HOTES[i].nom) == len && memcmp(HOTES[i].nom, nom, (size_t)len) == 0)
            return &HOTES[i];
    return NULL;
}

/* ================================================================== */
/* CLI / RPC / 自测                                                    */
/* ================================================================== */
static void json_defaut(Buf *b, int espaces)
{
    (void)espaces;
    buf_puts(b, "{\"défaut\": {\"étape\": ");
    json_escape_into(b, g_err.etape ? g_err.etape : "calcul",
                     (int)strlen(g_err.etape ? g_err.etape : "calcul"));
    buf_puts(b, ", \"ligne\": ");
    if (g_err.has_ligne)
        buf_printf(b, "%d", g_err.ligne);
    else
        buf_puts(b, "null");
    buf_puts(b, ", \"note\": ");
    json_escape_into(b, g_err.note ? g_err.note : "", (int)strlen(g_err.note ? g_err.note : ""));
    buf_puts(b, "}}");
}
static void imprimer_defaut_cli(void)
{
    Buf b;
    buf_init(&b);
    json_defaut(&b, 1);
    fputs(buf_c(&b), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static V *eval_source(const char *src, int len, Scope **glob)
{
    Node *prog = analyser(src, len);
    return eval_programme(prog, glob);
}

/* ------------------------------------------------------------------ */
/* 自测                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *src;
    V *want;
} CasOk;
typedef struct {
    const char *src;
    const char *etape;
} CasMal;

static V *N(long long x) { return v_nombre((i128)x * ECHELLE); }
static V *TX(const char *s) { return v_texte_c(s); }
static V *SU(int n, ...)
{
    V **it = (V **)xmalloc(sizeof(V *) * (size_t)(n > 0 ? n : 1));
    va_list ap;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++)
        it[i] = va_arg(ap, V *);
    va_end(ap);
    return v_suite(it, n);
}
static V *FI(int n, ...)
{
    V *f = v_fiche();
    va_list ap;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        const char *k = va_arg(ap, const char *);
        V *v = va_arg(ap, V *);
        fiche_set(f, k, (int)strlen(k), v);
    }
    va_end(ap);
    return f;
}

static int executer_autotest(void)
{
    static CasOk ok[512];
    static CasMal mal[64];
    int nok = 0, nmal = 0;
    int i, passed = 0, failed = 0;
    long long a, b;

#define ADD(s_, w_) (ok[nok].src = (s_), ok[nok].want = (w_), nok++)
#define ADDM(s_, et_) (mal[nmal].src = (s_), mal[nmal].etape = (et_), nmal++)

    ADD("0", N(0));
    ADD("1", N(1));
    ADD("42", N(42));
    ADD("3.5", v_nombre(3500000000LL));
    ADD("0.000000001", v_nombre(1));
    ADD("1.000000001", v_nombre(1000000001LL));
    ADD("-7", N(-7));
    ADD("\"bonjour\"", TX("bonjour"));
    ADD("«bonjour»", TX("bonjour"));
    ADD("OUI", v_oui());
    ADD("NON", v_non());
    ADD("VIDE", v_vide());
    ADD("‹1, 2, 3›", SU(3, N(1), N(2), N(3)));
    ADD("‹›", SU(0));
    ADD("⟬⟭", v_fiche());
    ADD("⟬nom ⟼ \"ada\"⟭", FI(1, "nom", TX("ada")));
    ADD("1 plus 2 fois 3", N(7));
    ADD("(1 plus 2) fois 3", N(9));
    ADD("7 sur 2", v_nombre(3500000000LL));
    ADD("6 sur 2", N(3));
    ADD("7 modulo 3", N(1));
    ADD("2 puissance 10", N(1024));
    ADD("2 fois 1.5", N(3));
    ADD("10 sur 4", v_nombre(2500000000LL));
    ADD("moins 5 plus 3", N(-2));
    ADD("1 plus 2 plus 3", N(6));
    ADD("10 moins 3 moins 2", N(5));
    ADD("2 puissance 3 puissance 2", N(512));
    ADD("1 plus petit que 2", v_oui());
    ADD("2 plus grand que 1", v_oui());
    ADD("1 égal à 1", v_oui());
    ADD("1 différent de 2", v_oui());
    ADD("5 au moins 3", v_oui());
    ADD("2 au plus 2", v_oui());
    ADD("OUI et NON", v_non());
    ADD("NON ou OUI", v_oui());
    ADD("non OUI", v_non());
    ADD("\"a\" plus \"b\"", TX("ab"));
    ADD("mesure de \"salut\"", N(5));
    ADD("mesure de \"é\"", N(1));
    ADD("en-texte de 3.5", TX("3.5"));
    ADD("en-texte de 3", TX("3"));
    ADD("abaissé de \"ABC\"", TX("abc"));
    ADD("haussé de \"abc\"", TX("ABC"));
    ADD("découpe de \"a,b,c\" par \",\"", SU(3, TX("a"), TX("b"), TX("c")));
    ADD("découpe de \"abc\" par \"\"", SU(3, TX("a"), TX("b"), TX("c")));
    ADD("recoud de ‹\"a\", \"b\"› avec \"-\"", TX("a-b"));
    ADD("renferme de \"salut\" dans \"alu\"", v_oui());
    ADD("renferme de \"salut\" dans \"zzz\"", v_non());
    ADD("bribe de \"abcdef\" depuis 2 jusqu'à 4", TX("bcd"));
    ADD("en-nombre de \"42\"", N(42));
    ADD("égalise de \"x\"", TX("x"));
    ADD("élément 1 de ‹10, 20›", N(10));
    ADD("élément 2 de ‹10, 20›", N(20));
    ADD("mesure de ‹1, 2, 3›", N(3));
    ADD("total de ‹1, 2, 3›", N(6));
    ADD("total de ‹1.5, 2.5›", N(4));
    ADD("ordonne de ‹3, 1, 2›", SU(3, N(1), N(2), N(3)));
    ADD("ordonne de ‹\"b\", \"a\"›", SU(2, TX("a"), TX("b")));
    ADD("ajoute-fin de ‹1› avec 2", SU(2, N(1), N(2)));
    ADD("bribe de ‹1, 2, 3, 4› depuis 2 jusqu'à 3", SU(2, N(2), N(3)));
    ADD("renferme de ‹1, 2› dans 2", v_oui());
    ADD("champ \"score\" de ⟬nom ⟼ \"ada\", score ⟼ 5⟭", N(5));
    ADD("clefs de ⟬a ⟼ 1, b ⟼ 2⟭", SU(2, TX("a"), TX("b")));
    ADD("teneurs de ⟬a ⟼ 1, b ⟼ 2⟭", SU(2, N(1), N(2)));
    ADD("tordu de ⟬a ⟼ 1⟭ avec \"b\" et 2", FI(2, "a", N(1), "b", N(2)));
    ADD("mesure de ⟬a ⟼ 1⟭", N(1));
    ADD("fige de ⟬a ⟼ 1⟭", TX("{\"a\":1}"));
    ADD("fige de ‹1, 2›", TX("[1,2]"));
    ADD("fige de OUI", TX("true"));
    ADD("fige de VIDE", TX("null"));
    ADD("que x vaille 5\nx", N(5));
    ADD("que x vaille 1\nque x devienne 9\nx", N(9));
    ADD("donne 5 à y\ny", N(5));
    ADD("que n vaille 0\ntant que n plus petit que 3 ⟦\n  que n devienne n plus 1\n⟧\nn", N(3));
    ADD("que s vaille 0\npour chaque x dans ‹1, 2, 3› ⟦\n  que s devienne s plus x\n⟧\ns", N(6));
    ADD("si 1 plus petit que 2 alors ⟦ rends 10 ⟧\nrends 20", N(10));
    ADD("si 2 plus petit que 1 alors ⟦ rends 10 ⟧ sinon ⟦ rends 20 ⟧", N(20));
    ADD("si NON alors ⟦ rends 1 ⟧ sinonsi OUI alors ⟦ rends 2 ⟧\nrends 3", N(2));
    ADD("on définit doubler de x comme ⟦\n  rends x fois 2\n⟧\ndoubler de 21", N(42));
    ADD("on définit fib de n comme ⟦\n  si n plus petit que 2 alors ⟦ rends n ⟧\n"
        "  rends (fib de n moins 1) plus (fib de n moins 2)\n⟧\nfib de 10",
        N(55));
    ADD("on définit somme de a et b comme ⟦ rends a plus b ⟧\nsomme de 3 et 4", N(7));
    ADD("que a vaille ‹1, 2›\nque b vaille ajoute-fin de a avec 3\nb", SU(3, N(1), N(2), N(3)));
    ADD("affiche \"bonjour\"\nVIDE", v_vide());
    ADD("silence", v_vide());
    ADD("que t vaille 0\npour chaque c dans \"abc\" ⟦\n  que t devienne t plus 1\n⟧\nt", N(3));

    ADDM("1 sur 0", "calcul");
    ADDM("inconnu", "nom");
    ADDM("1 plus \"a\"", "forme");
    ADDM("que 5 vaille 1", "syntaxe");
    ADDM("\"abc", "lexique");
    ADDM("bribe de 5 depuis 1 jusqu'à 2", "forme");
    ADDM("que x devienne 1", "nom");
    ADDM("en-nombre de \"abc\"", "calcul");
    ADDM("mesure de 5", "forme");
    ADDM("arrête\n", "calcul");
    ADDM("(1 plus 2", "syntaxe");
    ADDM("1 plus", "syntaxe");
    ADDM("élément 9 de ‹1, 2›", "calcul");
    ADDM("champ \"zzz\" de ⟬a ⟼ 1⟭", "forme");
    ADDM("que x vaille 1;\naffiche x", "lexique");

    /* 生成用例 */
    {
        static const long long couples[][2] = {{0, 1}, {1, 2}, {2, 3}, {5, 7}, {10, 4}, {100, 25},
                                               {3, 3}, {1, 10}, {12, 5}, {9, 2}};
        int c;
        char buf[256];
        for (c = 0; c < (int)(sizeof(couples) / sizeof(couples[0])); c++) {
            long long x = couples[c][0], y = couples[c][1];
            snprintf(buf, sizeof(buf), "%lld plus %lld", x, y);
            ADD(xstrdup_c(buf), N(x + y));
            snprintf(buf, sizeof(buf), "%lld moins %lld", x, y);
            ADD(xstrdup_c(buf), N(x - y));
            snprintf(buf, sizeof(buf), "%lld fois %lld", x, y);
            ADD(xstrdup_c(buf), N(x * y));
            if (y != 0) {
                snprintf(buf, sizeof(buf), "%lld sur %lld", x, y);
                ADD(xstrdup_c(buf), v_nombre(((i128)x * ECHELLE) / y));
                snprintf(buf, sizeof(buf), "%lld modulo %lld", x, y);
                ADD(xstrdup_c(buf), N(x % y));
            }
            snprintf(buf, sizeof(buf), "%lld plus grand que %lld", x, y);
            ADD(xstrdup_c(buf), x > y ? v_oui() : v_non());
            snprintf(buf, sizeof(buf), "%lld plus petit que %lld", x, y);
            ADD(xstrdup_c(buf), x < y ? v_oui() : v_non());
            snprintf(buf, sizeof(buf), "%lld égal à %lld", x, y);
            ADD(xstrdup_c(buf), x == y ? v_oui() : v_non());
            snprintf(buf, sizeof(buf), "%lld différent de %lld", x, y);
            ADD(xstrdup_c(buf), x != y ? v_oui() : v_non());
            snprintf(buf, sizeof(buf), "%lld au moins %lld", x, y);
            ADD(xstrdup_c(buf), x >= y ? v_oui() : v_non());
            snprintf(buf, sizeof(buf), "%lld au plus %lld", x, y);
            ADD(xstrdup_c(buf), x <= y ? v_oui() : v_non());
            snprintf(buf, sizeof(buf), "élément 1 de ‹%lld, %lld›", x, y);
            ADD(xstrdup_c(buf), N(x));
            snprintf(buf, sizeof(buf), "total de ‹%lld, %lld›", x, y);
            ADD(xstrdup_c(buf), N(x + y));
            snprintf(buf, sizeof(buf), "ordonne de ‹%lld, %lld›", y, x);
            ADD(xstrdup_c(buf), SU(2, N(x < y ? x : y), N(x < y ? y : x)));
            snprintf(buf, sizeof(buf), "ajoute-fin de ‹%lld› avec %lld", x, y);
            ADD(xstrdup_c(buf), SU(2, N(x), N(y)));
            snprintf(buf, sizeof(buf), "bribe de ‹%lld, %lld› depuis 1 jusqu'à 1", x, y);
            ADD(xstrdup_c(buf), SU(1, N(x)));
            snprintf(buf, sizeof(buf), "clefs de ⟬a ⟼ %lld, b ⟼ %lld⟭", x, y);
            ADD(xstrdup_c(buf), SU(2, TX("a"), TX("b")));
            snprintf(buf, sizeof(buf), "teneurs de ⟬a ⟼ %lld, b ⟼ %lld⟭", x, y);
            ADD(xstrdup_c(buf), SU(2, N(x), N(y)));
            snprintf(buf, sizeof(buf), "champ \"b\" de ⟬a ⟼ %lld, b ⟼ %lld⟭", x, y);
            ADD(xstrdup_c(buf), N(y));
            snprintf(buf, sizeof(buf), "mesure de ⟬a ⟼ %lld, b ⟼ %lld⟭", x, y);
            ADD(xstrdup_c(buf), N(2));
        }
    }
    {
        static const char *textes[] = {"", "a", "abc", "é", "e\u0301", "ab cd"};
        static const long long tailles[] = {0, 1, 3, 1, 1, 5};
        char buf[256];
        int c;
        for (c = 0; c < 6; c++) {
            snprintf(buf, sizeof(buf), "mesure de \"%s\"", textes[c]);
            ADD(xstrdup_c(buf), N(tailles[c]));
            snprintf(buf, sizeof(buf), "en-texte de \"%s\"", textes[c]);
            ADD(xstrdup_c(buf), TX(textes[c]));
        }
    }
    {
        static const char *textes[] = {"AbC", "salut", "ÉCOLE", "x"};
        static const char *bas[] = {"abc", "salut", "école", "x"};
        static const char *haut[] = {"ABC", "SALUT", "ÉCOLE", "X"};
        char buf[256];
        int c;
        for (c = 0; c < 4; c++) {
            snprintf(buf, sizeof(buf), "abaissé de \"%s\"", textes[c]);
            ADD(xstrdup_c(buf), TX(bas[c]));
            snprintf(buf, sizeof(buf), "haussé de \"%s\"", textes[c]);
            ADD(xstrdup_c(buf), TX(haut[c]));
            snprintf(buf, sizeof(buf), "mesure de \"%s\"", textes[c]);
            {
                Grapheme *gg;
                int gn = graphemes(textes[c], (int)strlen(textes[c]), &gg);
                ADD(xstrdup_c(buf), N((long long)gn));
            }
        }
    }
    {
        static const char *paires[][3] = {{"a,b,c", ",", "abc"}, {"1;2", ";", "12"}, {"abc", "b", "ac"}};
        int c;
        char buf[256];
        for (c = 0; c < 3; c++) {
            snprintf(buf, sizeof(buf), "recoud de (découpe de \"%s\" par \"%s\") avec \"%s\"",
                     paires[c][0], paires[c][1], paires[c][1]);
            ADD(xstrdup_c(buf), TX(paires[c][0]));
        }
    }
    {
        static const struct {
            const char *src;
            long long want;
        } arith[] = {
            {"2 puissance 0", 1}, {"2 puissance 1", 2}, {"2 puissance 8", 256},
            {"10 puissance 3", 1000}, {"1 puissance 99", 1}, {"0 fois 999", 0},
            {"0 plus 0", 0}, {"1 moins 1", 0}, {"100 sur 4", 25},
            {"9 modulo 4", 1}, {"10 modulo 5", 0},
        };
        int c;
        for (c = 0; c < (int)(sizeof(arith) / sizeof(arith[0])); c++)
            ADD(arith[c].src, N(arith[c].want));
        ADD("100 sur 8", v_nombre(12500000000LL));
        ADD("1 sur 2", v_nombre(500000000LL));
    }
    {
        int k;
        char buf[1024];
        for (k = 0; k <= 5; k++) {
            snprintf(buf, sizeof(buf),
                     "on définit f de n comme ⟦\n"
                     "  si n plus petit que 1 alors ⟦ rends 0 ⟧\n"
                     "  rends n plus f de n moins 1\n⟧\nf de %d",
                     k);
            ADD(xstrdup_c(buf), N((long long)k * (k + 1) / 2));
        }
    }
    {
        char buf[512];
        for (a = 1; a <= 3; a++)
            for (b = 1; b <= 3; b++) {
                snprintf(buf, sizeof(buf),
                         "si %lld plus grand que %lld alors ⟦ rends 100 ⟧ sinon ⟦ rends 200 ⟧", a, b);
                ADD(xstrdup_c(buf), N(a > b ? 100 : 200));
            }
    }
    {
        char buf[512];
        for (a = 0; a <= 3; a++) {
            snprintf(buf, sizeof(buf),
                     "que x vaille %lld\ntant que x plus petit que %lld ⟦\n"
                     "  que x devienne x plus 1\n⟧\nx",
                     a, a + 2);
            ADD(xstrdup_c(buf), N(a + 2));
        }
    }

    /* 运行 */
    g_muet = 1;
    for (i = 0; i < nok; i++) {
        V *got = NULL;
        g_profondeur = 0;
        g_ctrl_top = NULL;
        g_jmp_live = 1;
        if (setjmp(g_jmp) == 0) {
            got = eval_source(ok[i].src, (int)strlen(ok[i].src), NULL);
            g_jmp_live = 0;
        } else {
            g_jmp_live = 0;
            failed++;
            printf("  FAIL %s -> %s: %s\n", ok[i].src, g_err.etape, g_err.note);
            continue;
        }
        if (egal(got, ok[i].want))
            passed++;
        else {
            Buf gb, wb;
            buf_init(&gb);
            buf_init(&wb);
            json_dump(got, &gb, -1, 0, 0);
            json_dump(ok[i].want, &wb, -1, 0, 0);
            failed++;
            printf("  FAIL %s -> %s != %s\n", ok[i].src, buf_c(&gb), buf_c(&wb));
        }
    }
    for (i = 0; i < nmal; i++) {
        int sig;
        g_profondeur = 0;
        g_ctrl_top = NULL;
        g_jmp_live = 1;
        sig = setjmp(g_jmp);
        if (sig == 0) {
            eval_source(mal[i].src, (int)strlen(mal[i].src), NULL);
            g_jmp_live = 0;
            failed++;
            printf("  FAIL %s -> aucune erreur (attendu %s)\n", mal[i].src, mal[i].etape);
            continue;
        }
        g_jmp_live = 0;
        if (g_err.etape && strcmp(g_err.etape, mal[i].etape) == 0)
            passed++;
        else {
            failed++;
            printf("  FAIL %s -> %s != %s\n", mal[i].src, g_err.etape ? g_err.etape : "?", mal[i].etape);
        }
    }
    g_muet = 0;
    printf("sillage_c: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
#undef ADD
#undef ADDM
}

/* ------------------------------------------------------------------ */
/* 入口辅助                                                            */
/* ------------------------------------------------------------------ */
static char **split_args(int *pargc)
{
    wchar_t *cl = GetCommandLineW();
    char **out = (char **)xmalloc(sizeof(char *) * 1024);
    int n = 0, i = 0;
    while (cl[i]) {
        Buf wb;
        int inq = 0;
        while (cl[i] == L' ' || cl[i] == L'\t')
            i++;
        if (!cl[i])
            break;
        buf_init(&wb);
        while (cl[i]) {
            if (!inq && (cl[i] == L' ' || cl[i] == L'\t'))
                break;
            if (cl[i] == L'\\') {
                int nb = 0, k;
                while (cl[i] == L'\\') {
                    nb++;
                    i++;
                }
                if (cl[i] == L'"') {
                    for (k = 0; k < nb / 2; k++)
                        buf_putc(&wb, '\\');
                    if (nb % 2 == 0) {
                        if (!inq) {
                            inq = 1;
                            i++;
                        } else {
                            inq = 0;
                            i++;
                        }
                    } else {
                        buf_putc(&wb, '"');
                        i++;
                    }
                } else {
                    for (k = 0; k < nb; k++)
                        buf_putc(&wb, '\\');
                }
                continue;
            }
            if (cl[i] == L'"') {
                inq = !inq;
                i++;
                continue;
            }
            {
                char u8[8];
                wchar_t one[2];
                int ln;
                one[0] = cl[i];
                one[1] = 0;
                ln = WideCharToMultiByte(CP_UTF8, 0, one, 1, u8, 8, NULL, NULL);
                if (ln > 0)
                    buf_putn(&wb, u8, ln);
            }
            i++;
        }
        if (n < 1023)
            out[n++] = buf_take(&wb);
    }
    *pargc = n;
    return out;
}

static void creer_repertoires(const char *path)
{
    wchar_t *w = u8_to_w(path, -1);
    wchar_t tmp[4096];
    int i, n;
    wcscpy(tmp, w);
    n = (int)wcslen(tmp);
    for (i = 1; i < n; i++) {
        if (tmp[i] == L'/' || tmp[i] == L'\\') {
            wchar_t save = tmp[i];
            tmp[i] = 0;
            _wmkdir(tmp);
            tmp[i] = save;
        }
    }
    _wmkdir(tmp);
}

/* 读取一行（动态长度） */
static char *lire_ligne(FILE *f, int *len)
{
    Buf b;
    int c;
    buf_init(&b);
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        buf_putc(&b, (char)c);
    }
    if (c == EOF && b.len == 0)
        return NULL;
    *len = b.len;
    return buf_take(&b);
}

static V *champ_fiche(V *f, const char *k)
{
    int i;
    if (!f || f->t != V_FICHE)
        return NULL;
    i = fiche_index(f, k, (int)strlen(k));
    return i < 0 ? NULL : f->u.fi.v[i];
}

static int rpc_repondre(V *id, int ok, V *value, int mode_erreur)
{
    Buf b;
    buf_init(&b);
    buf_putc(&b, '{');
    buf_puts(&b, "\"id\":");
    if (id)
        json_dump(id, &b, -1, 0, 1);
    else
        buf_puts(&b, "null");
    if (ok) {
        buf_puts(&b, ",\"ok\":true,\"value\":");
        json_dump(value, &b, -1, 0, 1);
        buf_putc(&b, '}');
    } else {
        buf_puts(&b, ",\"ok\":false,\"error\":");
        if (mode_erreur) {
            buf_puts(&b, "{");
            buf_puts(&b, "\"étape\":");
            json_escape_into(&b, g_err.etape ? g_err.etape : "calcul",
                             (int)strlen(g_err.etape ? g_err.etape : "calcul"));
            buf_puts(&b, ",\"ligne\":");
            if (g_err.has_ligne)
                buf_printf(&b, "%d", g_err.ligne);
            else
                buf_puts(&b, "null");
            buf_puts(&b, ",\"note\":");
            json_escape_into(&b, g_err.note ? g_err.note : "", (int)strlen(g_err.note ? g_err.note : ""));
            buf_putc(&b, '}');
        } else {
            buf_puts(&b, "null");
        }
        buf_putc(&b, '}');
    }
    fwrite(buf_c(&b), 1, (size_t)b.len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return 0;
}

static int serveur_rpc(void)
{
    int ligne_len = 0;
    char *ligne;
    g_muet = 1;
    while ((ligne = lire_ligne(stdin, &ligne_len)) != NULL) {
        V *req = NULL;
        int sig;
        V *id = NULL;
        if (ligne_len == 0) {
            free(ligne);
            continue;
        }
        g_profondeur = 0;
        g_ctrl_top = NULL;
        g_jmp_live = 1;
        sig = setjmp(g_jmp);
        if (sig != 0) {
            g_jmp_live = 0;
            rpc_repondre(id, 0, NULL, 1);
            free(ligne);
            continue;
        }
        req = json_parse(ligne, ligne_len);
        if (!req || req->t != V_FICHE) {
            g_jmp_live = 0;
            g_err.etape = "syntaxe";
            g_err.note = xstrdup_c("requête JSON invalide");
            g_err.has_ligne = 0;
            rpc_repondre(NULL, 0, NULL, 1);
            free(ligne);
            continue;
        }
        id = champ_fiche(req, "id");
        {
            V *cmd = champ_fiche(req, "cmd");
            const char *c = (cmd && cmd->t == V_TEXTE) ? cmd->u.s.p : "";
            if (strcmp(c, "ping") == 0) {
                rpc_repondre(id, 1, v_texte_c("pong"), 0);
            } else if (strcmp(c, "eval") == 0) {
                V *src = champ_fiche(req, "source");
                V *val;
                if (!src || src->t != V_TEXTE)
                    defaut("forme", -1, "eval: source texte attendue");
                val = eval_source(src->u.s.p, src->u.s.len, NULL);
                rpc_repondre(id, 1, val, 0);
            } else if (strcmp(c, "call") == 0) {
                V *file = champ_fiche(req, "file");
                V *fn = champ_fiche(req, "func");
                V *arg = champ_fiche(req, "arg");
                char *path;
                Prog *p;
                V *a1[1];
                V *val;
                if (!file || file->t != V_TEXTE || !fn || fn->t != V_TEXTE)
                    defaut("forme", -1, "call: file/func texte attendus");
                if (fichier_existe(file->u.s.p))
                    path = path_abs(file->u.s.p);
                else
                    path = resoudre_programme(file->u.s.p);
                if (!path)
                    defaut("hôte", -1, "call: program-not-found: %s", file->u.s.p);
                p = prog_obtenir(path);
                if (!p)
                    defaut("hôte", -1, "call: program-not-found: %s", file->u.s.p);
                a1[0] = arg ? arg : v_vide();
                val = appeler_fonction(p->glob, fn->u.s.p, a1, 1);
                rpc_repondre(id, 1, val, 0);
            } else if (strcmp(c, "names") == 0) {
                V *file = champ_fiche(req, "file");
                char *path;
                Prog *p;
                V **it;
                int i;
                if (!file || file->t != V_TEXTE)
                    defaut("forme", -1, "names: file texte attendu");
                if (fichier_existe(file->u.s.p))
                    path = path_abs(file->u.s.p);
                else
                    path = resoudre_programme(file->u.s.p);
                if (!path)
                    defaut("hôte", -1, "names: program-not-found: %s", file->u.s.p);
                p = prog_obtenir(path);
                if (!p)
                    defaut("hôte", -1, "names: program-not-found: %s", file->u.s.p);
                it = (V **)xmalloc(sizeof(V *) * (size_t)(p->glob->n > 0 ? p->glob->n : 1));
                for (i = 0; i < p->glob->n; i++)
                    it[i] = v_texte_n(p->glob->names[i], p->glob->lens[i]);
                rpc_repondre(id, 1, v_suite(it, p->glob->n), 0);
            } else if (strcmp(c, "store") == 0) {
                V *f = v_fiche();
                fiche_set(f, "collections", 11, g_store);
                rpc_repondre(id, 1, f, 0);
            } else if (strcmp(c, "quit") == 0 || strcmp(c, "shutdown") == 0) {
                rpc_repondre(id, 1, v_texte_c("bye"), 0);
                g_jmp_live = 0;
                free(ligne);
                break;
            } else {
                defaut("hôte", -1, "commande inconnue: %s", c);
            }
        }
        g_jmp_live = 0;
        free(ligne);
    }
    return 0;
}

static int usage(void)
{
    printf("usage: sillage_c.exe [repl]               交互式解释器（双击 exe 等价）\n");
    printf("       sillage_c.exe {test|run|call|ast} ...\n");
    printf("       sillage_c.exe --server [--store <zici.json>]\n");
    return 2;
}


/* ================================================================== */
/* 交互式解释器（双击 exe / sillage_c.exe repl 进入）                    */
/* ================================================================== */
static int repl_incomplet(const char *s)
{
    int braces = 0, i = 0, quote = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        if (quote == 1) {
            if (c == '\\') {
                if (s[i + 1]) i += 2; else i++;
                continue;
            }
            if (c == '"') quote = 0;
            i++;
            continue;
        }
        if (quote == 2) {
            if (c == 0xC2 && (unsigned char)s[i + 1] == 0xBB) { quote = 0; i += 2; continue; }
            i++;
            continue;
        }
        if (c == '"') { quote = 1; i++; continue; }
        if (c == '#') break;
        if (c == 0xC2 && (unsigned char)s[i + 1] == 0xAB) { quote = 2; i += 2; continue; }
        if (c == 0xE2 && (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x96)
            break;                                              /* ‖ 注释 */
        if (c == 0xE2 && (unsigned char)s[i + 1] == 0x9F) {
            if ((unsigned char)s[i + 2] == 0xA6) { braces++; i += 3; continue; }  /* ⟦ */
            if ((unsigned char)s[i + 2] == 0xA7) { braces--; i += 3; continue; }  /* ⟧ */
        }
        i++;
    }
    if (quote) return 1;
    return braces > 0 ? 1 : 0;
}

static void repl_banniere(void)
{
    printf("\n");
    printf("  SILLAGE C 引擎 · 交互式解释器（v1.0）\n");
    printf("  输入语句回车求值；以 : 开头是命令。\n");
    printf("  示例：que prix vaille 12.5   然后   affiche prix fois 2\n");
    printf("  命令：:help 帮助   :reset 清空会话   :load <文件> 载入程序   :quit 退出\n");
    printf("\n");
    fflush(stdout);
}

static void repl_aide(void)
{
    printf("可用命令：\n");
    printf("  :help            显示本帮助\n");
    printf("  :reset           清空当前会话里已输入的代码\n");
    printf("  :load <文件.sil> 把文件内容并入当前会话并执行\n");
    printf("  :quit / :q       退出解释器\n");
    printf("其它输入按 SILLAGE 语句处理：一行一句；块用左右块括号，键盘直接输入 { 和 } 也可以。\n");
    fflush(stdout);
}

static void repl_afficher(V *v)
{
    Buf b;
    if (!v || v->t == V_VIDE) return;
    buf_init(&b);
    json_dump(v, &b, 2, 0, 1);
    printf("%s\n", buf_c(&b));
    fflush(stdout);
}

/* 读一行输入：控制台用 ReadConsoleW（UTF-16）转 UTF-8，管道用 fgets。 */
static int repl_lire(char *out, int cap)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        wchar_t w[4096];
        DWORD n = 0;
        int len;
        if (!ReadConsoleW(h, w, 4096, &n, NULL))
            return 0;
        len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, NULL, 0, NULL, NULL);
        if (len > cap - 1) len = cap - 1;
        WideCharToMultiByte(CP_UTF8, 0, w, (int)n, out, len, NULL, NULL);
        out[len] = 0;
    } else {
        if (!fgets(out, cap, stdin))
            return 0;
    }
    {
        char *nl = strchr(out, '\n');
        if (nl) *nl = 0;
        nl = strchr(out, '\r');
        if (nl) *nl = 0;
    }
    return 1;
}

/* 把 REPL 里输入的 ASCII { } 当成块括号 ⟦ ⟧（字符串内不转换） */
static void repl_ajoute_ligne(Buf *b, const char *s)
{
    int i = 0, quote = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        if (quote == 1) {
            buf_putc(b, (char)c);
            if (c == '\\' && s[i + 1]) {
                buf_putc(b, s[i + 1]);
                i += 2;
                continue;
            }
            if (c == '"') quote = 0;
            i++;
            continue;
        }
        if (quote == 2) {
            if (c == 0xC2 && (unsigned char)s[i + 1] == 0xBB) {
                buf_putn(b, s + i, 2);
                i += 2;
                quote = 0;
                continue;
            }
            buf_putc(b, (char)c);
            i++;
            continue;
        }
        if (c == '"') { quote = 1; buf_putc(b, (char)c); i++; continue; }
        if (c == 0xC2 && (unsigned char)s[i + 1] == 0xAB) {
            quote = 2;
            buf_putn(b, s + i, 2);
            i += 2;
            continue;
        }
        if (c == '{') { buf_putn(b, "\xE2\x9F\xA6", 3); i++; continue; }
        if (c == '}') { buf_putn(b, "\xE2\x9F\xA7", 3); i++; continue; }
        buf_putc(b, (char)c);
        i++;
    }
}

static int repl_interactif(void)
{
    Buf instr;
    Scope *session = NULL;
    char ligne[8192];
    int suite = 0;

    buf_init(&instr);
    repl_banniere();

    for (;;) {
        printf("%s", suite ? "      ... " : "sillage> ");
        fflush(stdout);
        if (!repl_lire(ligne, (int)sizeof(ligne))) {
            printf("\n");
            break;
        }
        if (!suite && ligne[0] == ':') {
            char *cmd = ligne + 1;
            while (*cmd == ' ') cmd++;
            if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0 ||
                strcmp(cmd, "exit") == 0 || strcmp(cmd, "quitter") == 0) {
                break;
            }
            if (strcmp(cmd, "help") == 0 || strcmp(cmd, "aide") == 0) {
                repl_aide();
                continue;
            }
            if (strcmp(cmd, "reset") == 0) {
                instr.len = 0;
                if (instr.p && instr.cap > 0) instr.p[0] = 0;
                session = NULL;
                printf("（会话已清空）\n");
                fflush(stdout);
                continue;
            }
            if (strncmp(cmd, "load ", 5) == 0) {
                char *path = cmd + 5;
                char *txt = NULL;
                int len = 0;
                while (*path == ' ') path++;
                if (!fichier_lire(path, &txt, &len)) {
                    char *alt = path_join(g_exe_dir, path);
                    if (!fichier_lire(alt, &txt, &len)) {
                        printf("（读不到文件：%s）\n", path);
                        fflush(stdout);
                        continue;
                    }
                    free(alt);
                }
                printf("（载入 %s，%d 字节）\n", path, len);
                g_jmp_live = 1;
                if (setjmp(g_jmp) != 0) {
                    g_jmp_live = 0;
                    imprimer_defaut_cli();
                    continue;
                }
                {
                    V *r = eval_source_scope(txt, len, &session);
                    g_jmp_live = 0;
                    repl_afficher(r);
                }
                free(txt);
                continue;
            }
            if (cmd[0] == 0) continue;
            printf("（未知命令：%s，输入 :help 查看命令）\n", cmd);
            fflush(stdout);
            continue;
        }
        if (!suite && ligne[0] == 0) continue;
        if (instr.len > 0) buf_putc(&instr, '\n');
        repl_ajoute_ligne(&instr, ligne);
        if (repl_incomplet(buf_c(&instr))) {
            suite = 1;
            continue;
        }
        suite = 0;
        g_jmp_live = 1;
        if (setjmp(g_jmp) != 0) {
            g_jmp_live = 0;
            imprimer_defaut_cli();
            instr.len = 0;
            if (instr.p && instr.cap > 0) instr.p[0] = 0;
            continue;
        }
        {
            V *r = eval_source_scope(buf_c(&instr), instr.len, &session);
            g_jmp_live = 0;
            repl_afficher(r);
        }
        instr.len = 0;
        if (instr.p && instr.cap > 0) instr.p[0] = 0;
    }
    return 0;
}

int main(void)
{
    char **argv;
    int argc = 0, i;
    char *store_arg = NULL;
    int serveur = 0;
    int repl = 0;

    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    /* 不调用 setlocale：中文界面统一用 UTF-8 字节 + 控制台 65001 代码页 */

    argv = split_args(&argc);

    /* 解析 --server / --store */
    {
        char **reste = (char **)xmalloc(sizeof(char *) * (size_t)(argc > 0 ? argc : 1));
        int nreste = 0;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--server") == 0) {
                serveur = 1;
            } else if (strcmp(argv[i], "--store") == 0 && i + 1 < argc) {
                store_arg = argv[++i];
            } else if (strcmp(argv[i], "repl") == 0 || strcmp(argv[i], "--repl") == 0 ||
                       strcmp(argv[i], "-i") == 0) {
                repl = 1;
            } else {
                reste[nreste++] = argv[i];
            }
        }
        argv = reste;
        argc = nreste;
    }

    /* 路径初始化 */
    {
        wchar_t wbuf[4096];
        DWORD n = GetModuleFileNameW(NULL, wbuf, 4096);
        char *exe;
        char *dir;
        if (n > 0) {
            exe = w_to_u8(wbuf);
            dir = path_dirname(exe);
        } else {
            dir = xstrdup_c(".");
        }
        g_exe_dir = dir;
        if (store_arg) {
            g_store_path = path_abs(store_arg);
        } else if (serveur) {
            char *miku = path_join(path_join(g_exe_dir, ".."), "miku");
            char *p = path_join(miku, "zici.json");
            g_store_path = path_abs(p);
        } else {
            /* 语言工程命令与交互模式：默认 data/sillage.json，项目根 = exe 目录 */
            char *p = path_join(path_join(g_exe_dir, "数据"), "sillage.json");
            g_store_path = path_abs(p);
        }
        {
            char *dirn = path_dirname(g_store_path);
            g_root = path_dirname(dirn);
        }
    }

    if (serveur) {
        store_charger();
        return serveur_rpc();
    }

    if (repl || argc == 0) {
        store_charger();
        return repl_interactif();
    }

    if (argc < 1)
        return usage();

    if (strcmp(argv[0], "test") == 0)
        return executer_autotest();

    if (strcmp(argv[0], "run") == 0 || strcmp(argv[0], "call") == 0 || strcmp(argv[0], "ast") == 0) {
        char *txt = NULL;
        int len = 0;
        if (argc < 2) {
            g_err.etape = "syntaxe";
            g_err.note = xstrdup_c("fichier manquant");
            g_err.has_ligne = 0;
            imprimer_defaut_cli();
            return 2;
        }
        if (!fichier_lire(argv[1], &txt, &len)) {
            g_err.etape = "hôte";
            g_err.note = xstrdup_c("fichier introuvable");
            g_err.has_ligne = 0;
            imprimer_defaut_cli();
            return 1;
        }
        store_charger();
        g_jmp_live = 1;
        if (setjmp(g_jmp) != 0) {
            g_jmp_live = 0;
            imprimer_defaut_cli();
            return 1;
        }
        if (strcmp(argv[0], "ast") == 0) {
            Node *prog = analyser(txt, len);
            Buf b;
            buf_init(&b);
            ast_bloc(prog, &b);
            fputs(buf_c(&b), stdout);
            fputc('\n', stdout);
            fflush(stdout);
            g_jmp_live = 0;
            return 0;
        }
        {
            Scope *glob = NULL;
            V *valeur = eval_source(txt, len, &glob);
            V *sortie = valeur;
            if (strcmp(argv[0], "run") == 0 && argc > 2) {
                V *arg = json_parse(argv[2], (int)strlen(argv[2]));
                V *a1[1];
                if (!arg) {
                    g_err.etape = "syntaxe";
                    g_err.note = xstrdup_c("JSON invalide");
                    g_err.has_ligne = 0;
                    imprimer_defaut_cli();
                    g_jmp_live = 0;
                    return 1;
                }
                a1[0] = arg;
                sortie = appeler_fonction(glob, "handle", a1, 1);
            } else if (strcmp(argv[0], "call") == 0) {
                V *a1[1];
                int nargs = 0;
                if (argc < 3) {
                    g_err.etape = "syntaxe";
                    g_err.note = xstrdup_c("fonction manquante");
                    g_err.has_ligne = 0;
                    imprimer_defaut_cli();
                    g_jmp_live = 0;
                    return 2;
                }
                if (argc > 3) {
                    V *arg = json_parse(argv[3], (int)strlen(argv[3]));
                    if (!arg) {
                        g_err.etape = "syntaxe";
                        g_err.note = xstrdup_c("JSON invalide");
                        g_err.has_ligne = 0;
                        imprimer_defaut_cli();
                        g_jmp_live = 0;
                        return 1;
                    }
                    a1[0] = arg;
                    nargs = 1;
                }
                sortie = appeler_fonction(glob, argv[2], a1, nargs);
            }
            {
                Buf b;
                buf_init(&b);
                json_dump(sortie, &b, 2, 0, 1);
                fputs(buf_c(&b), stdout);
                fputc('\n', stdout);
                fflush(stdout);
            }
        }
        g_jmp_live = 0;
        return 0;
    }

    return usage();
}

