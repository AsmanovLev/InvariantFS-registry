/*
 * invfs-pack.c — InvariantFS codecpack registry manager (WP60)
 *
 * Standalone C11/POSIX tool for the InvariantFS-registry repository.
 * Location decision (WP60): the manager lives in the REGISTRY repo
 * (tools/invfs-pack.c) rather than the engine tree (src/cli/pack.c), so
 * the registry is a self-contained deliverable and the engine build stays
 * untouched. The main repo only carries the man page (packaging/man).
 *
 *   invfs-pack list [family]         installed + available
 *   invfs-pack search <term>
 *   invfs-pack info <pack>
 *   invfs-pack install <pack>        + external depends check (prints distro cmd)
 *   invfs-pack remove <pack>
 *   invfs-pack update [<pack>]
 *   invfs-pack verify [<pack>]       sha256 (sig optional, NOT enforced)
 *   invfs-pack alternatives <family>
 *   invfs-pack use <family> <pack>   primary for FUTURE sweeps only
 *   invfs-pack where
 *
 * Repo source: file://path or https://host/raw-base. Install root defaults
 * to /.invariantfs/codecpacks (env INVFS_PACK_ROOT); --on-volume <mnt>
 * installs under <mnt>/.invariantfs/codecpacks for self-hosting.
 *
 * Conflict default: INSTALL BUT DO NOT TOUCH THE EXISTING PACK.
 * No external tools are ever installed; missing ones get a distro hint.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAXL 4096
#define MAX_PKGS 512
#define DEFAULT_REPO "https://raw.githubusercontent.com/AsmanovLev/InvariantFS-registry/main"
#define DEFAULT_ROOT "/.invariantfs/codecpacks"

static const char *prog = "invfs-pack";
static char pack_root[MAXL];

static int vercmp_name(const char *a, const char *b);
struct jval;
static void jfree(struct jval *v);

/* ---------------- small utils ---------------- */

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static void str_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* bounded append that never overflows and keeps NUL-termination */
static void sappend(char *dst, size_t cap, const char *src)
{
    size_t have = strlen(dst);
    size_t room = (have < cap) ? (cap - have - 1) : 0;
    size_t n = strlen(src);
    if (n > room) n = room;
    memcpy(dst + have, src, n);
    dst[have + n] = '\0';
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void mkdir_p(const char *path)
{
    char tmp[MAXL];
    size_t len;
    str_copy(tmp, sizeof tmp, path);
    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                die("mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        die("mkdir %s: %s", tmp, strerror(errno));
}

static void join(char *out, size_t cap, const char *a, const char *b)
{
    if (snprintf(out, cap, "%s/%s", a, b) >= (int)cap)
        die("path too long: %s/%s", a, b);
}

static char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = xmalloc((size_t)n + 1);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    if (len_out) *len_out = (size_t)n;
    return buf;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    return fclose(f);
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out;
    char buf[65536];
    size_t r;
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((r = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, r, out) != r) { fclose(in); fclose(out); return -1; }
    }
    if (ferror(in)) { fclose(in); fclose(out); return -1; }
    fclose(in);
    return fclose(out);
}

static int copy_tree(const char *src, const char *dst)
{
    DIR *d;
    struct dirent *de;
    mkdir_p(dst);
    if (!is_dir(dst)) return -1;
    d = opendir(src);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char sp[MAXL], dp[MAXL];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        join(sp, sizeof sp, src, de->d_name);
        join(dp, sizeof dp, dst, de->d_name);
        if (lstat(sp, &st) != 0) { closedir(d); return -1; }
        if (S_ISDIR(st.st_mode)) {
            if (copy_tree(sp, dp) != 0) { closedir(d); return -1; }
        } else if (S_ISLNK(st.st_mode)) {
            char target[MAXL];
            ssize_t n = readlink(sp, target, sizeof target - 1);
            if (n < 0) { closedir(d); return -1; }
            target[n] = '\0';
            unlink(dp);
            if (symlink(target, dp) != 0) { closedir(d); return -1; }
        } else if (S_ISREG(st.st_mode)) {
            if (copy_file(sp, dp) != 0) { closedir(d); return -1; }
            chmod(dp, st.st_mode & 07777);
        }
    }
    closedir(d);
    return 0;
}

static int rm_rf(const char *path)
{
    DIR *d;
    struct dirent *de;
    struct stat st;

    if (lstat(path, &st) != 0) return 0;      /* already gone */
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    d = opendir(path);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char p[MAXL];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        join(p, sizeof p, path, de->d_name);
        if (rm_rf(p) != 0) { closedir(d); return -1; }
    }
    closedir(d);
    return rmdir(path);
}

/* ---------------- SHA-256 ---------------- */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_ctx;

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
    e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->len = 0; c->n = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        size_t take = 64 - c->n;
        if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    pad = 0x00;
    while (c->n != 56) sha256_update(c, &pad, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (i*8)); sha256_update(c, &b, 1); }
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

static int sha256_file(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    sha256_ctx c;
    uint8_t digest[32];
    uint8_t buf[65536];
    size_t r;
    static const char hx[] = "0123456789abcdef";
    if (!f) return -1;
    sha256_init(&c);
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, r);
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    sha256_final(&c, digest);
    for (int i = 0; i < 32; i++) { hex[i*2] = hx[digest[i] >> 4]; hex[i*2+1] = hx[digest[i] & 15]; }
    hex[64] = '\0';
    return 0;
}

/* ---------------- minimal JSON DOM ---------------- */

typedef enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ } jtype;
typedef struct jval {
    jtype t;
    double num;
    char *str;
    struct jval **items; size_t n;
    char **keys; struct jval **vals; size_t nkv;
} jval;

typedef struct { const char *p; int err; } jparser;

static void jskip(jparser *j)
{
    while (*j->p && isspace((unsigned char)*j->p)) j->p++;
}

static char *jparse_str_raw(jparser *j)
{
    size_t cap = 32, n = 0;
    char *s = xmalloc(cap);
    j->p++;                                     /* opening quote */
    while (*j->p && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') {
            char e = *j->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '/': c = '/'; break;
            case '\\': c = '\\'; break;
            case '"': c = '"'; break;
            case 'u': {
                int v = 0;
                for (int i = 0; i < 4 && isxdigit((unsigned char)*j->p); i++) {
                    char h = *j->p++;
                    v = v * 16 + (isdigit((unsigned char)h) ? h - '0' : (tolower(h) - 'a' + 10));
                }
                if (v < 0x80) c = (unsigned char)v;
                else if (v < 0x800) {
                    if (n + 2 >= cap) { cap *= 2; s = xrealloc(s, cap); }
                    s[n++] = (char)(0xC0 | (v >> 6));
                    c = (unsigned char)(0x80 | (v & 0x3F));
                } else {
                    if (n + 3 >= cap) { cap *= 2; s = xrealloc(s, cap); }
                    s[n++] = (char)(0xE0 | (v >> 12));
                    s[n++] = (char)(0x80 | ((v >> 6) & 0x3F));
                    c = (unsigned char)(0x80 | (v & 0x3F));
                }
                break;
            }
            default: c = (unsigned char)e; break;
            }
        }
        if (n + 1 >= cap) { cap *= 2; s = xrealloc(s, cap); }
        s[n++] = (char)c;
    }
    if (*j->p != '"') { j->err = 1; free(s); return NULL; }
    j->p++;
    s[n] = '\0';
    return s;
}

static jval *jparse_value(jparser *j);

static jval *jnew(jtype t) { jval *v = xmalloc(sizeof *v); memset(v, 0, sizeof *v); v->t = t; return v; }

static jval *jparse_array(jparser *j)
{
    jval *a = jnew(JARR);
    j->p++;
    jskip(j);
    if (*j->p == ']') { j->p++; return a; }
    for (;;) {
        jval *v;
        jskip(j);
        v = jparse_value(j);
        if (!v) { j->err = 1; return a; }
        a->items = xrealloc(a->items, (a->n + 1) * sizeof *a->items);
        a->items[a->n++] = v;
        jskip(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; break; }
        j->err = 1; break;
    }
    return a;
}

static jval *jparse_object(jparser *j)
{
    jval *o = jnew(JOBJ);
    j->p++;
    jskip(j);
    if (*j->p == '}') { j->p++; return o; }
    for (;;) {
        char *key;
        jval *v;
        jskip(j);
        if (*j->p != '"') { j->err = 1; break; }
        key = jparse_str_raw(j);
        if (!key) { j->err = 1; break; }
        jskip(j);
        if (*j->p != ':') { j->err = 1; free(key); break; }
        j->p++;
        jskip(j);
        v = jparse_value(j);
        if (!v) { j->err = 1; free(key); break; }
        o->keys = xrealloc(o->keys, (o->nkv + 1) * sizeof *o->keys);
        o->vals = xrealloc(o->vals, (o->nkv + 1) * sizeof *o->vals);
        o->keys[o->nkv] = key;
        o->vals[o->nkv] = v;
        o->nkv++;
        jskip(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; break; }
        j->err = 1; break;
    }
    return o;
}

static jval *jparse_value(jparser *j)
{
    jskip(j);
    if (*j->p == '{') return jparse_object(j);
    if (*j->p == '[') return jparse_array(j);
    if (*j->p == '"') {
        char *s = jparse_str_raw(j);
        jval *v = jnew(JSTR);
        v->str = s;
        return v;
    }
    if (!strncmp(j->p, "true", 4)) { j->p += 4; jval *v = jnew(JBOOL); v->num = 1; return v; }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; jval *v = jnew(JBOOL); v->num = 0; return v; }
    if (!strncmp(j->p, "null", 4)) { j->p += 4; return jnew(JNULL); }
    if (*j->p == '-' || isdigit((unsigned char)*j->p)) {
        char *end;
        double d = strtod(j->p, &end);
        jval *v = jnew(JNUM);
        v->num = d;
        j->p = end;
        return v;
    }
    j->err = 1;
    return NULL;
}

static jval *jparse(const char *text)
{
    jparser j = { text, 0 };
    jval *v = jparse_value(&j);
    jskip(&j);
    if (j.err || !v) { jfree(v); return NULL; }
    return v;
}

static jval *jget(jval *o, const char *key)
{
    if (!o || o->t != JOBJ) return NULL;
    for (size_t i = 0; i < o->nkv; i++)
        if (!strcmp(o->keys[i], key)) return o->vals[i];
    return NULL;
}

static const char *jstr(jval *v) { return (v && v->t == JSTR) ? v->str : ""; }
static double jnum(jval *v) { return (v && (v->t == JNUM || v->t == JBOOL)) ? v->num : 0; }
static size_t jlen(jval *v) { return (v && v->t == JARR) ? v->n : 0; }
static jval *jat(jval *v, size_t i) { return (v && v->t == JARR && i < v->n) ? v->items[i] : NULL; }

static void jfree(jval *v)
{
    if (!v) return;
    if (v->t == JSTR) free(v->str);
    if (v->t == JARR) { for (size_t i = 0; i < v->n; i++) jfree(v->items[i]); free(v->items); }
    if (v->t == JOBJ) {
        for (size_t i = 0; i < v->nkv; i++) { free(v->keys[i]); jfree(v->vals[i]); }
        free(v->keys); free(v->vals);
    }
    free(v);
}

static void jarr_join(jval *v, char *dst, size_t cap)
{
    dst[0] = '\0';
    if (!v || v->t != JARR) return;
    for (size_t i = 0; i < v->n; i++) {
        if (i) sappend(dst, cap, ",");
        sappend(dst, cap, jstr(v->items[i]));
    }
}

/* ---------------- pack model ---------------- */

typedef struct {
    char name[128], version[64], codec_id[128], format_id[256];
    char family[64], category[32], type[32], min_read[64];
    char caps[256], provides[512], replaces[512], conflicts[512];
    char depends[512], recommends[512], description[256];
    long algo, priority;
    int installed;
} pkg;

static pkg *pkgs; static size_t npkgs;

static int list_has(const char *list, const char *tok)
{
    const char *p = list;
    size_t tl = strlen(tok);
    if (!list || !*list || !tl) return 0;
    while (*p) {
        const char *c = strchr(p, ',');
        const char *start = p;
        size_t l = c ? (size_t)(c - p) : strlen(p);
        while (l && (*start == ' ' || *start == '\t')) { start++; l--; }
        while (l && (start[l-1] == ' ' || start[l-1] == '\t')) l--;
        if (l == tl && !strncmp(start, tok, tl)) return 1;
        if (!c) break;
        p = c + 1;
    }
    return 0;
}

static void add_pkg(const pkg *p)
{
    if (npkgs >= MAX_PKGS) return;
    pkgs = xrealloc(pkgs, (npkgs + 1) * sizeof *pkgs);
    pkgs[npkgs++] = *p;
}

static int parse_manifest_text(const char *text, pkg *p)
{
    char *copy = xstrdup(text);
    char *save = NULL, *line;

    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *s = line, *eq, *val, *e;
        size_t l = strlen(s);
        while (l && (s[l-1] == '\r' || s[l-1] == ' ' || s[l-1] == '\t')) s[--l] = '\0';
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        eq = strchr(s, '=');
        if (!eq) continue;
        val = eq + 1;
        e = eq;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = '\0';
        while (*val == ' ' || *val == '\t') val++;
        if (!strcmp(s, "name")) str_copy(p->name, sizeof p->name, val);
        else if (!strcmp(s, "version")) str_copy(p->version, sizeof p->version, val);
        else if (!strcmp(s, "codec_id")) str_copy(p->codec_id, sizeof p->codec_id, val);
        else if (!strcmp(s, "format_id")) str_copy(p->format_id, sizeof p->format_id, val);
        else if (!strcmp(s, "family")) str_copy(p->family, sizeof p->family, val);
        else if (!strcmp(s, "category")) str_copy(p->category, sizeof p->category, val);
        else if (!strcmp(s, "type")) str_copy(p->type, sizeof p->type, val);
        else if (!strcmp(s, "min_read")) str_copy(p->min_read, sizeof p->min_read, val);
        else if (!strcmp(s, "caps")) { if (*val) str_copy(p->caps, sizeof p->caps, val); }
        else if (!strcmp(s, "provides")) { if (*val) str_copy(p->provides, sizeof p->provides, val); }
        else if (!strcmp(s, "replaces")) { if (*val) str_copy(p->replaces, sizeof p->replaces, val); }
        else if (!strcmp(s, "conflicts")) { if (*val) str_copy(p->conflicts, sizeof p->conflicts, val); }
        else if (!strcmp(s, "depends")) { if (*val) str_copy(p->depends, sizeof p->depends, val); }
        else if (!strcmp(s, "recommends")) { if (*val) str_copy(p->recommends, sizeof p->recommends, val); }
        else if (!strcmp(s, "description")) str_copy(p->description, sizeof p->description, val);
        else if (!strcmp(s, "algo")) p->algo = strtol(val, NULL, 0);
        else if (!strcmp(s, "priority")) p->priority = strtol(val, NULL, 0);
    }
    free(copy);
    return p->name[0] != '\0';
}

static void pkg_from_json(jval *o, pkg *p)
{
    memset(p, 0, sizeof *p);
    str_copy(p->name, sizeof p->name, jstr(jget(o, "name")));
    str_copy(p->version, sizeof p->version, jstr(jget(o, "version")));
    str_copy(p->codec_id, sizeof p->codec_id, jstr(jget(o, "codec_id")));
    str_copy(p->format_id, sizeof p->format_id, jstr(jget(o, "format_id")));
    str_copy(p->family, sizeof p->family, jstr(jget(o, "family")));
    str_copy(p->category, sizeof p->category, jstr(jget(o, "category")));
    str_copy(p->type, sizeof p->type, jstr(jget(o, "type")));
    str_copy(p->min_read, sizeof p->min_read, jstr(jget(o, "min_read")));
    str_copy(p->description, sizeof p->description, jstr(jget(o, "description")));
    jarr_join(jget(o, "caps"), p->caps, sizeof p->caps);
    jarr_join(jget(o, "provides"), p->provides, sizeof p->provides);
    jarr_join(jget(o, "replaces"), p->replaces, sizeof p->replaces);
    jarr_join(jget(o, "conflicts"), p->conflicts, sizeof p->conflicts);
    jarr_join(jget(o, "depends"), p->depends, sizeof p->depends);
    jarr_join(jget(o, "recommends"), p->recommends, sizeof p->recommends);
    p->algo = (long)jnum(jget(o, "algo"));
    p->priority = (long)jnum(jget(o, "priority"));
}

/* ---------------- repo access ---------------- */

typedef struct { int http; char base[MAXL]; } repo;
static repo g_repo;

static void repo_resolve(repo *r, const char *spec)
{
    if (!strncmp(spec, "file://", 7)) { r->http = 0; str_copy(r->base, sizeof r->base, spec + 7); }
    else if (!strncmp(spec, "http://", 7) || !strncmp(spec, "https://", 8)) {
        r->http = 1; str_copy(r->base, sizeof r->base, spec);
    } else { r->http = 0; str_copy(r->base, sizeof r->base, spec); }
}

static int curl_run(const char *url, const char *outpath, char **mem, size_t *memlen)
{
    int pipefd[2] = { -1, -1 };
    pid_t pid;
    int status = 0;
    if (mem && pipe(pipefd) != 0) return -1;
    pid = fork();
    if (pid < 0) { if (mem) { close(pipefd[0]); close(pipefd[1]); } return -1; }
    if (pid == 0) {
        char *argv[10]; int ac = 0;
        if (mem) {
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[0]); close(pipefd[1]);
        }
        argv[ac++] = (char *)"curl";
        argv[ac++] = (char *)"-fsSL";
        argv[ac++] = (char *)"--retry"; argv[ac++] = (char *)"2";
        if (!mem) { argv[ac++] = (char *)"-o"; argv[ac++] = (char *)outpath; }
        argv[ac++] = (char *)url;
        argv[ac] = NULL;
        execvp("curl", argv);
        _exit(127);
    }
    if (mem) {
        size_t cap = 65536, n = 0;
        char *buf = xmalloc(cap);
        ssize_t r;
        close(pipefd[1]);
        while ((r = read(pipefd[0], buf + n, cap - n)) > 0) {
            n += (size_t)r;
            if (n == cap) { cap *= 2; buf = xrealloc(buf, cap); }
        }
        close(pipefd[0]);
        buf[n] = '\0';
        *mem = buf; if (memlen) *memlen = n;
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) ;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static void repo_fetch_mem(repo *r, const char *rel, char **buf, size_t *len)
{
    if (len) *len = 0;
    if (!r->http) {
        char path[MAXL];
        join(path, sizeof path, r->base, rel);
        *buf = read_file(path, len);
        return;
    }
    {
        char url[2 * MAXL];
        if (snprintf(url, sizeof url, "%s/%s", r->base, rel) >= (int)sizeof url)
            die("url too long");
        if (curl_run(url, NULL, buf, len) != 0) { *buf = NULL; if (len) *len = 0; }
    }
}

static int repo_fetch_file(repo *r, const char *rel, const char *dest)
{
    if (!r->http) {
        char path[MAXL];
        join(path, sizeof path, r->base, rel);
        return copy_file(path, dest);
    }
    {
        char url[2 * MAXL];
        if (snprintf(url, sizeof url, "%s/%s", r->base, rel) >= (int)sizeof url) die("url too long");
        return curl_run(url, dest, NULL, NULL);
    }
}

static int load_index(repo *r)
{
    char *buf = NULL; size_t n = 0;
    jval *root, *packs;
    repo_fetch_mem(r, "codecpacks/index.json", &buf, &n);
    if (!buf) return -1;
    root = jparse(buf);
    free(buf);
    if (!root) return -1;
    packs = jget(root, "packs");
    for (size_t i = 0; i < jlen(packs); i++) {
        pkg p;
        pkg_from_json(jat(packs, i), &p);
        if (p.name[0]) add_pkg(&p);
    }
    jfree(root);
    return 0;
}

/* ---------------- installed scan ---------------- */

static void set_root(const char *dir)
{
    str_copy(pack_root, sizeof pack_root, dir);
    mkdir_p(pack_root);
}

static void scan_installed(void)
{
    DIR *d = opendir(pack_root);
    struct dirent *de;
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        char nd[MAXL];
        DIR *vd;
        struct dirent *ve;
        size_t l = strlen(de->d_name);
        if (de->d_name[0] == '.') continue;
        if (l > 10 && !strcmp(de->d_name + l - 10, ".codecpack")) continue;
        join(nd, sizeof nd, pack_root, de->d_name);
        if (!is_dir(nd)) continue;
        vd = opendir(nd);
        if (!vd) continue;
        while ((ve = readdir(vd)) != NULL) {
            char vp[MAXL], mp[MAXL];
            char *text;
            pkg p;
            if (ve->d_name[0] == '.') continue;
            join(vp, sizeof vp, nd, ve->d_name);
            if (!is_dir(vp)) continue;
            join(mp, sizeof mp, vp, "manifest");
            text = read_file(mp, NULL);
            if (!text) continue;
            memset(&p, 0, sizeof p);
            if (!parse_manifest_text(text, &p)) { free(text); continue; }
            free(text);
            if (!p.version[0]) str_copy(p.version, sizeof p.version, ve->d_name);
            p.installed = 1;
            add_pkg(&p);
        }
        closedir(vd);
    }
    closedir(d);
}

static pkg *find_avail(const char *name, const char *ver)
{
    for (size_t i = 0; i < npkgs; i++)
        if (!strcmp(pkgs[i].name, name) && !pkgs[i].installed &&
            (!ver || !*ver || !strcmp(pkgs[i].version, ver)))
            return &pkgs[i];
    return NULL;
}

static pkg *find_best_avail(const char *name)
{
    pkg *best = NULL;
    for (size_t i = 0; i < npkgs; i++)
        if (!strcmp(pkgs[i].name, name) && !pkgs[i].installed)
            if (!best || vercmp_name(pkgs[i].version, best->version) > 0) best = &pkgs[i];
    return best;
}

static pkg *find_any(const char *name, const char *ver)
{
    for (size_t i = 0; i < npkgs; i++)
        if (!strcmp(pkgs[i].name, name) && (!ver || !*ver || !strcmp(pkgs[i].version, ver)))
            return &pkgs[i];
    return NULL;
}

/* ---------------- semver ---------------- */

static int vercmp_name(const char *a, const char *b)
{
    long va[3] = {0,0,0}, vb[3] = {0,0,0};
    sscanf(a, "%ld.%ld.%ld", &va[0], &va[1], &va[2]);
    sscanf(b, "%ld.%ld.%ld", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; i++) {
        if (va[i] < vb[i]) return -1;
        if (va[i] > vb[i]) return 1;
    }
    return 0;
}

/* ---------------- alternatives.conf ---------------- */

static void alt_path(char *out, size_t cap) { join(out, cap, pack_root, "alternatives.conf"); }

static const char *alt_get(const char *family, char *out, size_t cap)
{
    char path[MAXL], *text;
    char *save = NULL, *line;
    out[0] = '\0';
    alt_path(path, sizeof path);
    text = read_file(path, NULL);
    if (!text) return NULL;
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '='), *k, *v, *e;
        if (!eq) continue;
        *eq = '\0'; k = line; v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        e = k + strlen(k);
        while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
        while (*v == ' ' || *v == '\t') v++;
        if (!strcmp(k, family)) { str_copy(out, cap, v); free(text); return out; }
    }
    free(text);
    return NULL;
}

static int alt_set(const char *family, const char *pack)
{
    char path[MAXL], *text, out[8192];
    char *save = NULL, *line;
    size_t n = 0;
    int replaced = 0;
    alt_path(path, sizeof path);
    text = read_file(path, NULL);
    out[0] = '\0';
    if (text) {
        for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *eq = strchr(line, '=');
            char key[128];
            if (!eq) continue;
            str_copy(key, sizeof key, line);
            { char *e = key + strlen(key); while (e > key && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0'; }
            if (!strcmp(key, family)) {
                n += (size_t)snprintf(out + n, sizeof out - n, "%s = %s\n", family, pack);
                replaced = 1;
            } else {
                n += (size_t)snprintf(out + n, sizeof out - n, "%s\n", line);
            }
        }
        free(text);
    }
    if (!replaced)
        n += (size_t)snprintf(out + n, sizeof out - n, "%s = %s\n", family, pack);
    return write_file(path, out, n);
}

/* ---------------- external dependency hints ---------------- */

static const struct { const char *tool, *deb, *arch, *gentoo, *void_, *fedora; } deptab[] = {
    { "python3",  "apt install python3",       "pacman -S python",      "emerge dev-lang/python",     "xbps-install python3", "dnf install python3" },
    { "cjxl",     "apt install libjxl-tools",  "pacman -S libjxl",      "emerge media-libs/libjxl",   "xbps-install libjxl",  "dnf install libjxl-tools" },
    { "djxl",     "apt install libjxl-tools",  "pacman -S libjxl",      "emerge media-libs/libjxl",   "xbps-install libjxl",  "dnf install libjxl-tools" },
    { "mac",      "apt install monkeys-audio", "pacman -S mac",         "emerge media-sound/mac",     "xbps-install mac",     "dnf install mac" },
    { "wavpack",  "apt install wavpack",       "pacman -S wavpack",     "emerge media-sound/wavpack", "xbps-install wavpack", "dnf install wavpack" },
    { "wvunpack", "apt install wavpack",       "pacman -S wavpack",     "emerge media-sound/wavpack", "xbps-install wavpack", "dnf install wavpack" },
    { "flac",     "apt install flac",          "pacman -S flac",        "emerge media-sound/flac",    "xbps-install flac",    "dnf install flac" },
    { "7zz",      "apt install 7zip",          "pacman -S 7zip",        "emerge app-arch/7zip",       "xbps-install 7zip",    "dnf install 7zip" },
    { "curl",     "apt install curl",          "pacman -S curl",        "emerge net-misc/curl",       "xbps-install curl",    "dnf install curl" },
};

static int on_path(const char *tool)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    while (*path) {
        const char *c = strchr(path, ':');
        size_t l = c ? (size_t)(c - path) : strlen(path);
        char full[MAXL];
        if (l && l < sizeof full - 2) {
            memcpy(full, path, l);
            full[l] = '\0';
            sappend(full, sizeof full, "/");
            sappend(full, sizeof full, tool);
            if (access(full, X_OK) == 0) return 1;
        }
        if (!c) break;
        path = c + 1;
    }
    return 0;
}

static const char *distro_name(char *buf, size_t cap)
{
    char *text = read_file("/etc/os-release", NULL);
    const char *found = "unknown";
    buf[0] = '\0';
    if (text) {
        /* Match ID= at a line start; a substring search hits VERSION_ID= first. */
        char *p = text;
        for (;;) {
            if (!strncmp(p, "ID=", 3)) break;
            p = strchr(p, '\n');
            if (!p) break;
            p++;
        }
        if (p) {
            char *v = strchr(p, '=') + 1, *e;
            if (*v == '"') v++;
            e = v;
            while (*e && *e != '\n' && *e != '"') e++;
            *e = '\0';
            str_copy(buf, cap, v);
        }
        free(text);
    }
    if (strstr(buf, "debian") || strstr(buf, "ubuntu")) found = "debian";
    else if (strstr(buf, "arch")) found = "arch";
    else if (strstr(buf, "gentoo")) found = "gentoo";
    else if (strstr(buf, "void")) found = "void";
    else if (strstr(buf, "fedora") || strstr(buf, "rhel") || strstr(buf, "centos")) found = "fedora";
    return found;
}

static void dep_hint(const char *tool, const char *distro)
{
    for (size_t i = 0; i < sizeof deptab / sizeof deptab[0]; i++) {
        if (strcmp(deptab[i].tool, tool)) continue;
        if (!strcmp(distro, "debian")) printf("    Debian/Ubuntu: sudo %s\n", deptab[i].deb);
        else if (!strcmp(distro, "arch")) printf("    Arch:          sudo %s\n", deptab[i].arch);
        else if (!strcmp(distro, "gentoo")) printf("    Gentoo:        sudo %s\n", deptab[i].gentoo);
        else if (!strcmp(distro, "void")) printf("    Void:          sudo %s\n", deptab[i].void_);
        else if (!strcmp(distro, "fedora")) printf("    Fedora/RHEL:   sudo %s\n", deptab[i].fedora);
        else printf("    install package providing: %s\n", tool);
        return;
    }
    printf("    install package providing: %s\n", tool);
}

static void check_deps(const pkg *p, int hard)
{
    char distro[64];
    const char *list = hard ? p->depends : p->recommends;
    char *copy, *save = NULL, *tok;
    if (!list || !*list) return;
    distro_name(distro, sizeof distro);
    copy = xstrdup(list);
    for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *e;
        while (*tok == ' ' || *tok == '\t') tok++;
        e = tok + strlen(tok);
        while (e > tok && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
        if (!*tok || on_path(tok)) continue;
        if (hard) {
            printf("  ! missing required external tool '%s' — pack installs but coverage is degraded.\n", tok);
            dep_hint(tok, distro);
        } else {
            printf("  - missing recommended tool '%s' (optional).\n", tok);
            dep_hint(tok, distro);
        }
    }
    free(copy);
}

/* ---------------- verify ---------------- */

static int verify_dir(const char *dir, int quiet)
{
    char shp[MAXL], *text;
    char *save = NULL, *line;
    int bad = 0, checked = 0;
    join(shp, sizeof shp, dir, "sha256");
    text = read_file(shp, NULL);
    if (!text) {
        if (!quiet) printf("  %s: no sha256 manifest — cannot verify\n", dir);
        return 1;
    }
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char expect[128], rel[MAXL], full[MAXL], got[65];
        char *sp;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        if (sscanf(line, "%127s", expect) != 1) continue;
        sp = strstr(line, "  ");
        if (!sp) sp = strchr(line, ' ');
        if (!sp) continue;
        sp++;
        while (*sp == ' ') sp++;
        str_copy(rel, sizeof rel, sp);
        { char *e = rel + strlen(rel); while (e > rel && (e[-1] == '\r' || e[-1] == ' ')) *--e = '\0'; }
        join(full, sizeof full, dir, rel);
        if (sha256_file(full, got) != 0) {
            printf("  FAIL %s/%s: missing or unreadable\n", dir, rel);
            bad++;
            continue;
        }
        checked++;
        if (strcasecmp(expect, got)) {
            printf("  FAIL %s/%s\n    expected %s\n    actual   %s\n", dir, rel, expect, got);
            bad++;
        } else if (!quiet) {
            printf("  OK   %s/%s\n", dir, rel);
        }
    }
    free(text);
    if (!bad && !quiet) printf("  %s: %d file(s) verified\n", dir, checked);
    return bad ? 1 : 0;
}

static int verify_named(const char *name, int quiet, int *found)
{
    char nd[MAXL];
    DIR *d;
    struct dirent *de;
    int rc = 0;
    join(nd, sizeof nd, pack_root, name);
    if (!is_dir(nd)) return 0;
    d = opendir(nd);
    if (!d) return 0;
    while ((de = readdir(d)) != NULL) {
        char vp[MAXL];
        if (de->d_name[0] == '.') continue;
        join(vp, sizeof vp, nd, de->d_name);
        if (!is_dir(vp)) continue;
        *found = 1;
        rc |= verify_dir(vp, quiet);
    }
    closedir(d);
    return rc;
}

static int verify_all(int quiet, int *found)
{
    DIR *d = opendir(pack_root);
    struct dirent *de;
    int rc = 0;
    if (!d) return 0;
    while ((de = readdir(d)) != NULL) {
        char pnd[MAXL];
        DIR *vd;
        struct dirent *ve;
        size_t l = strlen(de->d_name);
        if (de->d_name[0] == '.') continue;
        if (l > 10 && !strcmp(de->d_name + l - 10, ".codecpack")) continue;
        join(pnd, sizeof pnd, pack_root, de->d_name);
        if (!is_dir(pnd)) continue;
        vd = opendir(pnd);
        if (!vd) continue;
        while ((ve = readdir(vd)) != NULL) {
            char vp[MAXL];
            if (ve->d_name[0] == '.') continue;
            join(vp, sizeof vp, pnd, ve->d_name);
            if (!is_dir(vp)) continue;
            *found = 1;
            rc |= verify_dir(vp, quiet);
        }
        closedir(vd);
    }
    closedir(d);
    return rc;
}

/* ---------------- install ---------------- */

static int install_from_repo(repo *r, pkg *p, int force)
{
    char relbase[MAXL], dest[MAXL], destparent[MAXL], tmp[MAXL];
    snprintf(relbase, sizeof relbase, "codecpacks/%s/%s", p->name, p->version);
    join(destparent, sizeof destparent, pack_root, p->name);
    join(dest, sizeof dest, destparent, p->version);
    if (is_dir(dest) && !force) {
        printf("  %s %s already installed (%s)\n", p->name, p->version, dest);
        return 0;
    }
    join(tmp, sizeof tmp, pack_root, ".staging");
    rm_rf(tmp);
    mkdir_p(tmp);
    if (!r->http) {
        char src[MAXL];
        join(src, sizeof src, r->base, relbase);
        if (!is_dir(src)) { printf("  repo pack dir missing: %s\n", src); rm_rf(tmp); return 1; }
        if (copy_tree(src, tmp) != 0) { printf("  copy failed\n"); rm_rf(tmp); return 1; }
    } else {
        char shrel[MAXL], shpath[MAXL], mrel[MAXL], mpath[MAXL];
        char *shtext, *save = NULL, *line;
        snprintf(shrel, sizeof shrel, "%s/sha256", relbase);
        join(shpath, sizeof shpath, tmp, "sha256");
        if (repo_fetch_file(r, shrel, shpath) != 0) { printf("  cannot fetch %s\n", shrel); rm_rf(tmp); return 1; }
        shtext = read_file(shpath, NULL);
        if (!shtext) { printf("  cannot read fetched sha256\n"); rm_rf(tmp); return 1; }
        snprintf(mrel, sizeof mrel, "%s/manifest", relbase);
        join(mpath, sizeof mpath, tmp, "manifest");
        if (repo_fetch_file(r, mrel, mpath) != 0) { printf("  cannot fetch %s\n", mrel); free(shtext); rm_rf(tmp); return 1; }
        for (line = strtok_r(shtext, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char rel[MAXL], full[MAXL], dpath[MAXL];
            char *sp, *slash;
            while (*line == ' ' || *line == '\t') line++;
            if (!*line || *line == '#') continue;
            sp = strstr(line, "  ");
            if (!sp) sp = strchr(line, ' ');
            if (!sp) continue;
            sp++;
            while (*sp == ' ') sp++;
            str_copy(rel, sizeof rel, sp);
            { char *e = rel + strlen(rel); while (e > rel && (e[-1] == '\r' || e[-1] == ' ')) *--e = '\0'; }
            if (!strcmp(rel, "sha256")) continue;
            snprintf(full, sizeof full, "%s/%s", relbase, rel);
            join(dpath, sizeof dpath, tmp, rel);
            slash = strrchr(dpath, '/');
            if (slash) { *slash = '\0'; mkdir_p(dpath); *slash = '/'; }
            if (repo_fetch_file(r, full, dpath) != 0) { printf("  cannot fetch %s\n", full); free(shtext); rm_rf(tmp); return 1; }
        }
        free(shtext);
    }

    if (verify_dir(tmp, 1) != 0) {
        printf("  REFUSED: sha256 mismatch installing %s %s (tampered pack?)\n", p->name, p->version);
        rm_rf(tmp);
        return 1;
    }
    {
        char parent[MAXL];
        join(parent, sizeof parent, pack_root, p->name);
        mkdir_p(parent);
        if (is_dir(dest)) rm_rf(dest);
        if (rename(tmp, dest) != 0) {
            if (copy_tree(tmp, dest) != 0) { printf("  install move failed\n"); rm_rf(tmp); return 1; }
            rm_rf(tmp);
        }
    }
    {
        char linkp[MAXL], target[MAXL];
        join(linkp, sizeof linkp, pack_root, p->name);
        sappend(linkp, sizeof linkp, ".codecpack");
        snprintf(target, sizeof target, "%s/%s", p->name, p->version);
        rm_rf(linkp);
        if (symlink(target, linkp) != 0)
            printf("  note: could not create legacy symlink %s\n", linkp);
    }
    printf("  installed %s %s -> %s\n", p->name, p->version, dest);
    p->installed = 1;
    return 0;
}

static void warn_overlap(pkg *p)
{
    for (size_t i = 0; i < npkgs; i++) {
        pkg *o = &pkgs[i];
        if (o == p || !o->installed || !strcmp(o->name, p->name)) continue;
        if (list_has(p->conflicts, o->name) || list_has(o->conflicts, p->name))
            printf("  ! WARNING: %s conflicts with installed %s — keeping both (conflict default).\n",
                   p->name, o->name);
        if (p->provides[0] && o->provides[0] && !strcmp(p->family, o->family) &&
            list_has(p->provides, p->family) && list_has(o->provides, p->family))
            printf("  note: %s also provides family %s (priority %ld vs %ld); existing %s kept.\n",
                   o->name, p->family, o->priority, p->priority, o->name);
    }
}

/* ---------------- commands ---------------- */

static int cmd_list(int argc, char **argv)
{
    const char *family = argc ? argv[0] : NULL;
    printf("Codecpacks (root %s, repo %s)\n", pack_root, g_repo.base);
    printf("  %-12s %-8s %-10s %-5s %s\n", "NAME", "VERSION", "FAMILY", "INST", "FORMAT");
    for (size_t i = 0; i < npkgs; i++) {
        if (family && *family && strcmp(pkgs[i].family, family)) continue;
        printf("  %-12s %-8s %-10s %-5s %s\n", pkgs[i].name, pkgs[i].version,
               pkgs[i].family, pkgs[i].installed ? "yes" : "no", pkgs[i].format_id);
    }
    return 0;
}

static int cmd_search(int argc, char **argv)
{
    const char *term;
    if (argc < 1) die("usage: %s search <term>", prog);
    term = argv[0];
    printf("Matches for '%s':\n", term);
    for (size_t i = 0; i < npkgs; i++) {
        pkg *p = &pkgs[i];
        if (strstr(p->name, term) || strstr(p->format_id, term) ||
            strstr(p->provides, term) || strstr(p->family, term) ||
            strstr(p->description, term))
            printf("  %-12s %-8s %-10s %s\n", p->name, p->version, p->family,
                   p->installed ? "installed" : "available");
    }
    return 0;
}

static void print_kv(const char *k, const char *v)
{
    if (v && *v) printf("  %-14s %s\n", k, v);
}

static int cmd_info(int argc, char **argv)
{
    pkg *p;
    if (argc < 1) die("usage: %s info <pack>", prog);
    p = find_any(argv[0], NULL);
    if (!p) { fprintf(stderr, "%s: pack '%s' not found\n", prog, argv[0]); return 1; }
    printf("Pack: %s %s%s\n", p->name, p->version, p->installed ? " (installed)" : "");
    printf("  %-14s %s\n", "codec_id", p->codec_id);
    printf("  %-14s %ld\n", "algo", p->algo);
    print_kv("format_id", p->format_id);
    print_kv("family", p->family);
    print_kv("category", p->category);
    print_kv("type", p->type);
    print_kv("min_read", p->min_read);
    printf("  %-14s %ld\n", "priority", p->priority);
    print_kv("caps", p->caps);
    print_kv("provides", p->provides);
    print_kv("replaces", p->replaces);
    print_kv("conflicts", p->conflicts);
    print_kv("depends", p->depends);
    print_kv("recommends", p->recommends);
    print_kv("description", p->description);
    if (p->installed) {
        char d[MAXL], nd[MAXL];
        join(nd, sizeof nd, pack_root, p->name);
        join(d, sizeof d, nd, p->version);
        printf("  %-14s %s\n", "installed", d);
    }
    return 0;
}

static int cmd_install(int argc, char **argv)
{
    const char *name = NULL, *ver = NULL;
    pkg *p;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--version") && i + 1 < argc) ver = argv[++i];
        else if (!strcmp(argv[i], "--without") && i + 1 < argc) { i++; }
        else if (argv[i][0] != '-') name = argv[i];
    }
    if (!name) die("usage: %s install <pack> [--version V]", prog);
    p = find_avail(name, ver);
    if (!p) {
        pkg *inst = find_any(name, ver);
        if (inst && inst->installed) { printf("  %s already installed\n", name); return 0; }
        fprintf(stderr, "%s: pack '%s' not found in the index\n", prog, name);
        return 1;
    }
    printf("Installing %s %s (%s/%s)\n", p->name, p->version, p->family, p->format_id);
    warn_overlap(p);
    check_deps(p, 1);
    check_deps(p, 0);
    return install_from_repo(&g_repo, p, 0);
}

static int cmd_remove(int argc, char **argv)
{
    const char *name;
    char nd[MAXL], lf[MAXL];
    DIR *d;
    struct dirent *de;
    int removed = 0;
    if (argc < 1) die("usage: %s remove <pack>", prog);
    name = argv[0];
    join(nd, sizeof nd, pack_root, name);
    if (!is_dir(nd)) { fprintf(stderr, "%s: '%s' is not installed\n", prog, name); return 1; }
    d = opendir(nd);
    if (!d) { fprintf(stderr, "%s: cannot open %s\n", prog, nd); return 1; }
    while ((de = readdir(d)) != NULL) {
        char vp[MAXL];
        if (de->d_name[0] == '.') continue;
        join(vp, sizeof vp, nd, de->d_name);
        if (!is_dir(vp)) continue;
        rm_rf(vp);
        removed++;
    }
    closedir(d);
    join(lf, sizeof lf, pack_root, name);
    sappend(lf, sizeof lf, ".codecpack");
    rm_rf(lf);
    rm_rf(nd);
    printf("  removed %s (%d version(s))\n", name, removed);
    return 0;
}

static int cmd_update(int argc, char **argv)
{
    const char *name = argc ? argv[0] : NULL;
    int any = 0;
    for (size_t i = 0; i < npkgs; i++) {
        pkg *inst = &pkgs[i], *best;
        if (!inst->installed) continue;
        if (name && strcmp(inst->name, name)) continue;
        best = find_best_avail(inst->name);
        if (best && vercmp_name(best->version, inst->version) > 0) {
            printf("Updating %s %s -> %s\n", inst->name, inst->version, best->version);
            install_from_repo(&g_repo, best, 0);
            any = 1;
        } else if (name) {
            printf("  %s is up to date (%s)\n", inst->name, inst->version);
        }
    }
    if (name && !any) printf("  %s: no update available\n", name);
    return 0;
}

static int cmd_verify(int argc, char **argv)
{
    int found = 0, rc;
    if (argc) rc = verify_named(argv[0], 0, &found);
    else rc = verify_all(0, &found);
    if (!found) { printf("No installed pack to verify under %s\n", pack_root); return 1; }
    printf(rc ? "verify: FAIL\n" : "verify: PASS\n");
    return rc;
}

typedef struct { char name[128], version[64], family[64], provides[512]; long priority; int installed; } altrow;

static int cmd_alternatives(int argc, char **argv)
{
    const char *family, *primary = NULL;
    char prim[128];
    altrow rows[MAX_PKGS]; size_t nr = 0;
    if (argc < 1) die("usage: %s alternatives <family>", prog);
    family = argv[0];
    if (alt_get(family, prim, sizeof prim) && prim[0]) primary = prim;
    for (size_t i = 0; i < npkgs; i++) {
        pkg *p = &pkgs[i];
        int provides = !strcmp(p->family, family) || list_has(p->provides, family);
        int dup = 0;
        if (!provides) continue;
        for (size_t j = 0; j < nr; j++) if (!strcmp(rows[j].name, p->name)) dup = 1;
        if (dup) continue;
        str_copy(rows[nr].name, sizeof rows[nr].name, p->name);
        str_copy(rows[nr].version, sizeof rows[nr].version, p->version);
        str_copy(rows[nr].family, sizeof rows[nr].family, p->family);
        str_copy(rows[nr].provides, sizeof rows[nr].provides, p->provides);
        rows[nr].priority = p->priority;
        rows[nr].installed = p->installed;
        nr++;
    }
    if (!primary) {
        long best = -1;
        for (size_t i = 0; i < nr; i++)
            if (rows[i].installed && rows[i].priority > best) { best = rows[i].priority; primary = rows[i].name; }
        if (!primary)
            for (size_t i = 0; i < nr; i++)
                if (rows[i].priority > best) { best = rows[i].priority; primary = rows[i].name; }
    }
    printf("Alternatives for family '%s':\n", family);
    printf("  %-12s %-8s %-5s %-5s %s\n", "NAME", "VERSION", "PRIO", "INST", "PRIMARY");
    for (size_t i = 0; i < nr; i++)
        printf("  %-12s %-8s %-5ld %-5s %s\n", rows[i].name, rows[i].version, rows[i].priority,
               rows[i].installed ? "yes" : "no",
               (primary && !strcmp(primary, rows[i].name)) ? "*" : "");
    if (primary)
        printf("Primary: %s (future sweeps only; no existing file is re-encoded)\n", primary);
    return 0;
}

static int cmd_use(int argc, char **argv)
{
    const char *family, *pack;
    pkg *p;
    if (argc < 2) die("usage: %s use <family> <pack>", prog);
    family = argv[0]; pack = argv[1];
    p = find_any(pack, NULL);
    if (!p) { fprintf(stderr, "%s: pack '%s' not found\n", prog, pack); return 1; }
    if (!p->installed) { fprintf(stderr, "%s: pack '%s' is not installed\n", prog, pack); return 1; }
    if (strcmp(p->family, family) && !list_has(p->provides, family)) {
        fprintf(stderr, "%s: pack '%s' does not provide family '%s'\n", prog, pack, family);
        return 1;
    }
    if (alt_set(family, pack) != 0) die("cannot write alternatives.conf");
    printf("  primary for %s := %s (future sweeps only; no re-encode)\n", family, pack);
    return 0;
}

static int cmd_where(void)
{
    printf("repo:        %s\n", g_repo.base);
    printf("repo kind:   %s\n", g_repo.http ? "https" : "file");
    printf("pack root:   %s\n", pack_root);
    printf("host root:   %s\n", DEFAULT_ROOT);
    printf("search order: $INVFS_CODECPACKS -> host root -> on-volume root -> /usr/lib/invfs/codecpacks (legacy)\n");
    printf("on-volume:   pass --on-volume <mnt> to install under <mnt>/.invariantfs/codecpacks\n");
    printf("sig policy:  sha256 always; signatures optional and NOT enforced\n");
    return 0;
}

/* ---------------- main ---------------- */

static void usage(void)
{
    printf(
"usage: %s [--repo file://DIR|https://BASE] [--root DIR] [--on-volume MNT] <cmd> [args]\n"
"\n"
"commands:\n"
"  list [family]           installed + available packs\n"
"  search <term>           match name/format/provides/family/description\n"
"  info <pack>             show manifest fields\n"
"  install <pack>          install (conflict default: keep existing)\n"
"  remove <pack>           uninstall\n"
"  update [<pack>]         update installed packs to newer versions\n"
"  verify [<pack>]         recompute sha256 against the installed manifest\n"
"  alternatives <family>   list candidates + the current primary\n"
"  use <family> <pack>     set the primary for FUTURE sweeps only\n"
"  where                   resolved repo + pack roots\n"
"\n"
"env: INVFS_PACK_REPO (repo), INVFS_PACK_ROOT (install root, default %s)\n",
    prog, DEFAULT_ROOT);
}

int main(int argc, char **argv)
{
    const char *repo_spec = NULL, *root = NULL, *on_volume = NULL;
    int cmd_i = -1;
    const char *cmd;
    char **cargv;
    int cargc;

    {
        const char *e = getenv("INVFS_PACK_REPO");
        if (e && *e) repo_spec = e;
        e = getenv("INVFS_PACK_ROOT");
        if (e && *e) root = e;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--repo") && i + 1 < argc) repo_spec = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--on-volume") && i + 1 < argc) on_volume = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else { cmd_i = i; break; }
    }
    if (cmd_i < 0) { usage(); return 2; }
    cmd = argv[cmd_i];
    cargv = argv + cmd_i + 1;
    cargc = argc - cmd_i - 1;

    if (on_volume) {
        char r[MAXL];
        if (!is_dir(on_volume))
            die("--on-volume %s is not a mounted directory (mount the image first)", on_volume);
        join(r, sizeof r, on_volume, ".invariantfs/codecpacks");
        root = r;
    }
    repo_resolve(&g_repo, repo_spec ? repo_spec : DEFAULT_REPO);
    set_root(root ? root : DEFAULT_ROOT);

    if (strcmp(cmd, "where") != 0) {
        if (load_index(&g_repo) != 0)
            fprintf(stderr, "%s: warning: no codecpacks/index.json in %s (available list empty)\n",
                    prog, g_repo.base);
        scan_installed();
        for (size_t a = 0; a < npkgs; a++)
            for (size_t b = a + 1; b < npkgs; b++)
                if (!strcmp(pkgs[a].name, pkgs[b].name) &&
                    !strcmp(pkgs[a].version, pkgs[b].version)) {
                    if (pkgs[b].installed) pkgs[a].installed = 1;
                    npkgs--;
                    memmove(&pkgs[b], &pkgs[b+1], (npkgs - b) * sizeof *pkgs);
                    b--;
                }
    }

    if (!strcmp(cmd, "list")) return cmd_list(cargc, cargv);
    if (!strcmp(cmd, "search")) return cmd_search(cargc, cargv);
    if (!strcmp(cmd, "info")) return cmd_info(cargc, cargv);
    if (!strcmp(cmd, "install")) return cmd_install(cargc, cargv);
    if (!strcmp(cmd, "remove")) return cmd_remove(cargc, cargv);
    if (!strcmp(cmd, "update")) return cmd_update(cargc, cargv);
    if (!strcmp(cmd, "verify")) return cmd_verify(cargc, cargv);
    if (!strcmp(cmd, "alternatives")) return cmd_alternatives(cargc, cargv);
    if (!strcmp(cmd, "use")) return cmd_use(cargc, cargv);
    if (!strcmp(cmd, "where")) return cmd_where();

    fprintf(stderr, "%s: unknown command '%s'\n", prog, cmd);
    usage();
    return 2;
}
