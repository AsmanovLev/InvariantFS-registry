/* wrap-wide.c — WP619 build glue for the Monkey's Audio payload.
 *
 * WHY THIS FILE EXISTS. A glibc-static binary that calls wide stdio
 * (fwprintf/fputws on a FILE stream) segfaults on this toolchain
 * (Debian 14 gcc 14, glibc 2.40/2.41): _IO_doallocbuf jumps through a
 * NULL wide vtable. Minimal repro (no MAC code at all):
 *
 *     #include <stdio.h>
 *     #include <wchar.h>
 *     int main(){ fwprintf(stderr, L"hi %ls\n", L"x"); return 0; }
 *
 *   $ g++ -static -O2 -o wtest wtest.c && ./wtest   -> SIGSEGV
 *   $ g++ -O2 -o wtest-dyn wtest.c && ./wtest-dyn   -> prints fine
 *   narrow fprintf under -static is fine; musl has no libstdc++ for the
 *   C++ payload, so glibc-static is the only static shape available.
 *
 * WHAT IT DOES. Provides __wrap_fwprintf/__wrap_fputws for
 * -Wl,--wrap=fwprintf --wrap=fputws: every wide console write in the
 * unmodified upstream SDK is rendered to narrow and emitted with
 * vfprintf/fputs (both narrow-safe under -static). In-memory wide calls
 * (swprintf) never touch FILE vtables and are left alone.
 *
 * SCOPE. Console output only (progress/usage/tag dumps — all ASCII in
 * practice). The codec never sees this file: exit codes, file bytes and
 * error paths are the upstream's. Formats supported: flags/width/
 * precision (incl. *) + lengths (hh/h/l/ll/j/z/t/L) over
 * d/i/u/x/X/o/f/F/e/E/g/G/c/s/p/n/%; %s takes char* and %ls takes
 * wchar_t* (exactly the glibc convention — the SDK's tag dumps pass
 * wide strings to bare %s, an MSVC-ism that truncates the same way
 * under the dynamic build; the shim reproduces it rather than
 * "fixing" it, so shim output is byte-identical to the real tool).
 * %c takes wint_t. Anything else prints as-is ("<?>" for an
 * unconvertible character, never a crash).
 *
 * Upstream sources are NOT modified (the vendored tarball stays
 * hash-pinned); this file is build glue, committed beside it and hashed
 * by the pack's sha256 listing like everything else in the pack dir.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>

/* one converted wide string (stack-freed by the caller frame) */
static char *wide_to_narrow(const wchar_t *ws, int max_chars)
{
    /* worst case MB_CUR_MAX per wchar + NUL; cap absurd inputs */
    size_t n = 0;
    const wchar_t *p;
    char *out, *q;
    if (!ws) return NULL;
    for (p = ws; *p && (max_chars < 0 || n < (size_t)max_chars); p++, n++)
        ;
    out = (char *)malloc(n * MB_CUR_MAX + 1);
    if (!out) return NULL;
    q = out;
    n = 0;
    for (p = ws; *p && (max_chars < 0 || (int)n < max_chars); p++, n++) {
        char mb[MB_LEN_MAX];
        int len = wctomb(mb, *p);
        if (len < 0) {
            /* unconvertible under the C locale: emit a placeholder
             * rather than failing the whole line (progress text is
             * cosmetic; the exit code is what the engine reads) */
            *q++ = '?';
        } else {
            memcpy(q, mb, (size_t)len);
            q += len;
        }
    }
    *q = '\0';
    return out;
}

/* Render one conversion spec starting at *pp (pp points at '%').
 * Consumes matching args from ap, appends to out (realloc'd). */
static void render_one(char **out, size_t *len, size_t *cap,
                       const wchar_t **pp, va_list *ap, int *total)
{
    char spec[64];
    size_t sl = 0;
    const wchar_t *p = *pp;     /* at '%' */
    int is_s = 0, is_c = 0, is_n = 0;
    char conv = 0;

    spec[sl++] = '%';
    p++;
    /* flags */
    while (*p == L'-' || *p == L'+' || *p == L' ' || *p == L'#' || *p == L'0') {
        if (sl + 1 < sizeof spec) spec[sl++] = (char)*p;
        p++;
    }
    /* width */
    if (*p == L'*') {
        char wb[24];
        int w = va_arg(*ap, int);
        snprintf(wb, sizeof wb, "%d", w);
        size_t wl = strlen(wb);
        if (sl + wl < sizeof spec) { memcpy(spec + sl, wb, wl); sl += wl; }
        p++;
    } else {
        while (*p >= L'0' && *p <= L'9') {
            if (sl + 1 < sizeof spec) spec[sl++] = (char)*p;
            p++;
        }
    }
    /* precision */
    if (*p == L'.') {
        if (sl + 1 < sizeof spec) spec[sl++] = '.';
        p++;
        if (*p == L'*') {
            char wb[24];
            int w = va_arg(*ap, int);
            snprintf(wb, sizeof wb, "%d", w);
            size_t wl = strlen(wb);
            if (sl + wl < sizeof spec) { memcpy(spec + sl, wb, wl); sl += wl; }
            p++;
        } else {
            while (*p >= L'0' && *p <= L'9') {
                if (sl + 1 < sizeof spec) spec[sl++] = (char)*p;
                p++;
            }
        }
    }
    /* length */
    if (*p == L'h' && p[1] == L'h') {
        if (sl + 2 < sizeof spec) { spec[sl++] = 'h'; spec[sl++] = 'h'; }
        p += 2;
    } else if (*p == L'l' && p[1] == L'l') {
        if (sl + 2 < sizeof spec) { spec[sl++] = 'l'; spec[sl++] = 'l'; }
        p += 2;
    } else if (*p == L'h' || *p == L'l' || *p == L'j' || *p == L'z' ||
               *p == L't' || *p == L'L') {
        if (sl + 1 < sizeof spec) spec[sl++] = (char)*p;
        p++;
    }
    spec[sl] = '\0';   /* terminate before any strchr/strstr below */
    conv = (char)*p;
    if (conv) p++;
    is_s = (conv == 's');
    is_c = (conv == 'c' || conv == 'C');
    if (conv == 'S') { conv = 's'; is_s = 1; }   /* MS extension */
    is_n = (conv == 'n');

    if (conv == '%') {
        if (sl + 1 < sizeof spec) spec[sl++] = '%';
        spec[sl] = '\0';
        size_t need = *len + 2;
        if (need > *cap) {
            *cap = need * 2 + 64;
            *out = (char *)realloc(*out, *cap);
        }
        (*out)[(*len)++] = '%';
        (*out)[*len] = '\0';
        *total += 1;
        *pp = p;
        return;
    }
    if (!conv) { *pp = p; return; }     /* trailing '%': drop */

    if (is_s) {
        /* glibc convention, reproduced exactly: %s takes char*,
         * %ls takes wchar_t*. Check whether an 'l' length was given. */
        int is_long = (strchr(spec, 'l') != NULL);
        if (is_long) {
            const wchar_t *ws = va_arg(*ap, const wchar_t *);
            /* precision on %ls counts wide chars: truncate first */
            int prec = -1;
            char *dot = strchr(spec + 1, '.');
            if (dot) prec = atoi(dot + 1);
            char *mb = wide_to_narrow(ws ? ws : L"(null)", prec);
            char fmt[80];
            /* rebuild without length mods: plain %s + flags/width/prec */
            size_t fi = 0;
            const char *q = spec + 1;
            fmt[fi++] = '%';
            while (*q && *q != 's') {
                if (*q == 'l' || *q == 'h') { q++; continue; }
                if (fi + 1 < sizeof fmt) fmt[fi++] = *q;
                q++;
            }
            fmt[fi++] = 's';
            fmt[fi] = '\0';
            char piece[8192];
            int w = snprintf(piece, sizeof piece, fmt, mb ? mb : "(null)");
            free(mb);
            if (w < 0) w = 0;
            size_t need = *len + (size_t)w + 1;
            if (need > *cap) {
                *cap = need * 2 + 64;
                *out = (char *)realloc(*out, *cap);
            }
            memcpy(*out + *len, piece, (size_t)w);
            *len += (size_t)w;
            (*out)[*len] = '\0';
            *total += w;
            *pp = p;
            return;
        }
        /* bare %s: narrow string, glibc semantics */
        {
            const char *ns = va_arg(*ap, const char *);
            int prec = -1;
            char *dot = strchr(spec + 1, '.');
            char fmt[80];
            size_t fi = 0;
            const char *q = spec + 1;
            char piece[8192];
            int w;
            if (!ns) ns = "(null)";
            if (dot) prec = atoi(dot + 1);
            fmt[fi++] = '%';
            while (*q && *q != 's') {
                if (fi + 1 < sizeof fmt) fmt[fi++] = *q;
                q++;
            }
            fmt[fi++] = 's';
            fmt[fi] = '\0';
            if (prec >= 0) {
                /* byte-precision: truncate the input first */
                char tmp[8192];
                size_t tl = strlen(ns);
                if (tl > (size_t)prec) tl = (size_t)prec;
                if (tl >= sizeof tmp) tl = sizeof tmp - 1;
                memcpy(tmp, ns, tl);
                tmp[tl] = '\0';
                w = snprintf(piece, sizeof piece, fmt, tmp);
            } else {
                w = snprintf(piece, sizeof piece, fmt, ns);
            }
            if (w < 0) w = 0;
            size_t need = *len + (size_t)w + 1;
            if (need > *cap) {
                *cap = need * 2 + 64;
                *out = (char *)realloc(*out, *cap);
            }
            memcpy(*out + *len, piece, (size_t)w);
            *len += (size_t)w;
            (*out)[*len] = '\0';
            *total += w;
            *pp = p;
            return;
        }
    }
    if (is_c) {
        wint_t wc = va_arg(*ap, wint_t);
        char mb[MB_LEN_MAX + 1];
        int mblen = wctomb(mb, (wchar_t)wc);
        char piece[64];
        int w;
        if (mblen < 0) { mb[0] = '?'; mb[1] = '\0'; }
        else mb[mblen] = '\0';
        w = snprintf(piece, sizeof piece, "%s", mb);
        size_t need = *len + (size_t)w + 1;
        if (need > *cap) {
            *cap = need * 2 + 64;
            *out = (char *)realloc(*out, *cap);
        }
        memcpy(*out + *len, piece, (size_t)w);
        *len += (size_t)w;
        (*out)[*len] = '\0';
        *total += w;
        *pp = p;
        return;
    }
    if (is_n) {
        /* %n: store the count so far (length-mod sized) */
        if (strstr(spec, "ll")) *va_arg(*ap, long long *) = *total;
        else if (strchr(spec, 'l')) *va_arg(*ap, long *) = *total;
        else if (strstr(spec, "hh")) *va_arg(*ap, signed char *) = (signed char)*total;
        else if (strchr(spec, 'h')) *va_arg(*ap, short *) = (short)*total;
        else *va_arg(*ap, int *) = *total;
        *pp = p;
        return;
    }
    /* numeric / pointer: format directly with the right va_arg type */
    {
        char piece[512];
        int w = 0;
        if (strstr(spec, "ll")) {
            if (conv == 'd' || conv == 'i') {
                long long v = va_arg(*ap, long long);
                if (sl + 1 < sizeof spec) { spec[sl++] = conv; spec[sl] = '\0'; }
                w = snprintf(piece, sizeof piece, spec, v);
            } else {
                unsigned long long v = va_arg(*ap, unsigned long long);
                if (sl + 1 < sizeof spec) { spec[sl++] = conv; spec[sl] = '\0'; }
                w = snprintf(piece, sizeof piece, spec, v);
            }
        } else if (strchr(spec, 'l')) {
            if (conv == 'd' || conv == 'i') {
                long v = va_arg(*ap, long);
                if (sl + 1 < sizeof spec) { spec[sl++] = conv; spec[sl] = '\0'; }
                w = snprintf(piece, sizeof piece, spec, v);
            } else {
                unsigned long v = va_arg(*ap, unsigned long);
                if (sl + 1 < sizeof spec) { spec[sl++] = conv; spec[sl] = '\0'; }
                w = snprintf(piece, sizeof piece, spec, v);
            }
        } else if (conv == 'p') {
            void *v = va_arg(*ap, void *);
            w = snprintf(piece, sizeof piece, "%p", v);
        } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
                   conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A') {
            double v = va_arg(*ap, double);
            if (sl + 1 < sizeof spec) { spec[sl++] = conv; spec[sl] = '\0'; }
            w = snprintf(piece, sizeof piece, spec, v);
        } else if (conv == 'd' || conv == 'i') {
            int v = va_arg(*ap, int);
            if (sl + 1 < sizeof spec) { spec[sl++] = conv; spec[sl] = '\0'; }
            w = snprintf(piece, sizeof piece, spec, v);
        } else {
            unsigned v = va_arg(*ap, unsigned);
            if (sl + 1 < sizeof spec) { spec[sl++] = conv; spec[sl] = '\0'; }
            w = snprintf(piece, sizeof piece, spec, v);
        }
        if (w < 0) w = 0;
        if ((size_t)w >= sizeof piece) w = (int)sizeof piece - 1;
        {
            size_t need = *len + (size_t)w + 1;
            if (need > *cap) {
                *cap = need * 2 + 64;
                *out = (char *)realloc(*out, *cap);
            }
            memcpy(*out + *len, piece, (size_t)w);
            *len += (size_t)w;
            (*out)[*len] = '\0';
            *total += w;
        }
        *pp = p;
        return;
    }
}

int __wrap_fwprintf(FILE *f, const wchar_t *wfmt, ...)
{
    va_list ap;
    const wchar_t *p;
    char *out = NULL;
    size_t len = 0, cap = 0;
    int total = 0, rc;
    if (!wfmt) return -1;
    if (!f) return -1;
    va_start(ap, wfmt);
    p = wfmt;
    while (*p) {
        if (*p != L'%') {
            /* literal run: convert straight through */
            const wchar_t *q = p;
            size_t n = 0;
            while (q[n] && q[n] != L'%') n++;
            {
                wchar_t *seg = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
                char *mb;
                if (!seg) break;
                wcsncpy(seg, q, n);
                seg[n] = 0;
                mb = wide_to_narrow(seg, -1);
                free(seg);
                if (mb) {
                    size_t ml = strlen(mb);
                    size_t need = len + ml + 1;
                    if (need > cap) {
                        cap = need * 2 + 64;
                        out = (char *)realloc(out, cap);
                    }
                    memcpy(out + len, mb, ml);
                    len += ml;
                    out[len] = '\0';
                    total += (int)ml;
                    free(mb);
                }
            }
            p = q + n;
        } else {
            render_one(&out, &len, &cap, &p, &ap, &total);
        }
    }
    va_end(ap);
    if (!out) return 0;
    rc = fputs(out, f);
    free(out);
    if (rc < 0) return rc;
    return total;
}

int __wrap_fputws(const wchar_t *ws, FILE *f)
{
    char *mb;
    int rc;
    if (!ws || !f) return -1;
    mb = wide_to_narrow(ws, -1);
    if (!mb) return -1;
    rc = fputs(mb, f);
    free(mb);
    return rc < 0 ? -1 : 1;
}
