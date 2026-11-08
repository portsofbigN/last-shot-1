// A small zstd decoder (RFC 8878 frames, no dictionaries), for the host's own reads of the game's .zs files
// (nx2_shaderprep.c). The game decodes its files with its own recompiled zstd; this one is for host tools.
// nx2_zstd_size: the content size the frame header records (0 when unknown); nx2_zstd_decompress: the decoded size,
// or (size_t)-1 on bad input / small output.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ERR ((size_t)-1)

// ---- bit readers
typedef struct { const uint8_t *p; size_t n, pos; } FwdBits;   // forward, little-endian bit order
static uint32_t fwd_read(FwdBits *b, int nb) {
    uint32_t v = 0;
    for (int i = 0; i < nb; i++, b->pos++)
        if ((b->pos >> 3) < b->n) v |= (uint32_t)((b->p[b->pos >> 3] >> (b->pos & 7)) & 1) << i;
    return v;
}

typedef struct { const uint8_t *p; int64_t pos; } BackBits;   // backward: bits below pos, as a little-endian number
static int back_init(BackBits *b, const uint8_t *p, size_t n) {
    if (!n || !p[n - 1]) return 0;
    int hb = 7;
    while (!(p[n - 1] >> hb & 1)) hb--;
    b->p = p, b->pos = (int64_t)(n - 1) * 8 + hb;
    return 1;
}
static uint64_t bits_at(const uint8_t *p, int64_t pos, int nb) {   // nb bits from bit pos (pos >= 0)
    uint64_t v = 0;
    int64_t byte = pos >> 3;
    int sh = (int)(pos & 7), got = 0;
    while (got < nb + sh) {
        v |= (uint64_t)p[byte] << got;
        byte++, got += 8;
        if (got >= 56) break;
    }
    v >>= sh;
    if (nb < 64) v &= (1ull << nb) - 1;
    if (got - sh < nb) {   // more than 56 bits: not used here
        return v;
    }
    return v;
}
static uint64_t back_peek(const BackBits *b, int nb) {
    if (!nb) return 0;
    int64_t np = b->pos - nb;
    if (np >= 0) return bits_at(b->p, np, nb);
    if (b->pos <= 0) return 0;
    return bits_at(b->p, 0, (int)b->pos) << (-np);   // past the start: zeros below
}
static uint64_t back_read(BackBits *b, int nb) {
    uint64_t v = back_peek(b, nb);
    b->pos -= nb;
    return v;
}

static int highbit(uint32_t v) { int r = -1; while (v) v >>= 1, r++; return r; }

// ---- FSE
typedef struct { uint8_t sym, nb; uint16_t base; } FseCell;
typedef struct { FseCell t[1 << 9]; int log; } Fse;

static int fse_build(Fse *f, const int16_t *norm, int nsym, int log) {
    int size = 1 << log, high = size - 1;
    uint16_t next[256];
    if (nsym > 256 || log > 9) return 0;
    for (int s = 0; s < nsym; s++) {
        if (norm[s] == -1) f->t[high--].sym = (uint8_t)s, next[s] = 1;
        else next[s] = (uint16_t)(norm[s] > 0 ? norm[s] : 0);
    }
    int pos = 0, step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
    for (int s = 0; s < nsym; s++)
        for (int i = 0; i < norm[s]; i++) {
            f->t[pos].sym = (uint8_t)s;
            do pos = (pos + step) & mask;
            while (pos > high);
        }
    if (pos) return 0;
    for (int u = 0; u < size; u++) {
        int s = f->t[u].sym;
        uint32_t ns = next[s]++;
        int nb = log - highbit(ns);
        f->t[u].nb = (uint8_t)nb;
        f->t[u].base = (uint16_t)((ns << nb) - size);
    }
    f->log = log;
    return 1;
}
// a table description at p: returns its bytes, 0 on error
static size_t fse_read(Fse *f, const uint8_t *p, size_t n, int max_log, int max_sym) {
    FwdBits b = {p, n, 0};
    int log = (int)fwd_read(&b, 4) + 5;
    if (log > max_log) return 0;
    int16_t norm[256] = {0};
    int remaining = (1 << log) + 1, threshold = 1 << log, nbits = log + 1, s = 0, prev0 = 0;
    while (remaining > 1 && s <= max_sym) {
        if (prev0) {
            int n0 = 0, r;
            do r = (int)fwd_read(&b, 2), n0 += r;
            while (r == 3);
            while (n0-- > 0 && s <= max_sym) norm[s++] = 0;
            if (s > max_sym) return 0;
        }
        int max = 2 * threshold - 1 - remaining, count;
        uint32_t low = fwd_read(&b, nbits - 1);
        if ((int)low < max) count = (int)low;
        else {
            uint32_t hi = fwd_read(&b, 1);
            count = (int)(low | hi << (nbits - 1));
            if (count >= threshold) count -= max;
        }
        count--;
        remaining -= count < 0 ? -count : count;
        norm[s++] = (int16_t)count;
        prev0 = count == 0;
        while (remaining < threshold) nbits--, threshold >>= 1;
    }
    if (remaining != 1 || (b.pos + 7) / 8 > n) return 0;
    if (!fse_build(f, norm, s, log)) return 0;
    return (b.pos + 7) / 8;
}
static void fse_rle(Fse *f, uint8_t sym) { f->t[0] = (FseCell){sym, 0, 0}, f->log = 0; }

// ---- Huffman (literals)
typedef struct { uint8_t sym[1 << 11], nb[1 << 11]; int log, ok; } Huf;

static size_t huf_read(Huf *h, const uint8_t *p, size_t n) {
    if (!n) return 0;
    uint8_t w[256];
    int nw = 0;
    size_t used;
    int hb = p[0];
    if (hb >= 128) {   // direct 4-bit weights
        nw = hb - 127;
        used = 1 + (size_t)(nw + 1) / 2;
        if (used > n) return 0;
        for (int i = 0; i < nw; i++) w[i] = (uint8_t)(i & 1 ? p[1 + i / 2] & 15 : p[1 + i / 2] >> 4);
    } else {   // FSE-compressed weights, two interleaved states
        used = 1 + (size_t)hb;
        if (used > n) return 0;
        Fse f;
        size_t th = fse_read(&f, p + 1, hb, 6, 255);
        if (!th || th >= (size_t)hb) return 0;
        BackBits b;
        if (!back_init(&b, p + 1 + th, hb - th)) return 0;
        uint32_t s1 = (uint32_t)back_read(&b, f.log), s2 = (uint32_t)back_read(&b, f.log);
        for (;;) {
            if (nw > 253) return 0;
            w[nw++] = f.t[s1].sym;
            s1 = f.t[s1].base + (uint32_t)back_read(&b, f.t[s1].nb);
            if (b.pos < 0) { w[nw++] = f.t[s2].sym; break; }
            w[nw++] = f.t[s2].sym;
            s2 = f.t[s2].base + (uint32_t)back_read(&b, f.t[s2].nb);
            if (b.pos < 0) { w[nw++] = f.t[s1].sym; break; }
        }
    }
    uint32_t total = 0;
    for (int i = 0; i < nw; i++) {
        if (w[i] > 11) return 0;
        if (w[i]) total += 1u << (w[i] - 1);
    }
    if (!total) return 0;
    int log = highbit(total) + 1;
    uint32_t rest = (1u << log) - total;
    if (rest & (rest - 1)) return 0;
    w[nw++] = (uint8_t)(highbit(rest) + 1);
    if (log > 11) return 0;
    uint32_t start[13] = {0}, cnt[13] = {0};
    for (int i = 0; i < nw; i++) cnt[w[i]]++;
    uint32_t pos = 0;
    for (int k = 1; k <= log; k++) start[k] = pos, pos += cnt[k] << (k - 1);
    for (int s = 0; s < nw; s++) {
        if (!w[s]) continue;
        uint32_t len = 1u << (w[s] - 1);
        for (uint32_t i = 0; i < len; i++)
            h->sym[start[w[s]] + i] = (uint8_t)s, h->nb[start[w[s]] + i] = (uint8_t)(log + 1 - w[s]);
        start[w[s]] += len;
    }
    h->log = log, h->ok = 1;
    return used;
}
static int huf_stream(const Huf *h, const uint8_t *p, size_t n, uint8_t *out, size_t count) {
    BackBits b;
    if (!back_init(&b, p, n)) return 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t k = (uint32_t)back_peek(&b, h->log);
        out[i] = h->sym[k];
        b.pos -= h->nb[k];
    }
    return b.pos == 0;
}

// ---- sequences
static const uint32_t ll_base[36] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 18,
                                     20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const uint8_t ll_bits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                    1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint32_t ml_base[53] = {3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                                     21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41,
                                     43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
static const uint8_t ml_bits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const int16_t ll_def[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2,
                                   2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const int16_t ml_def[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
static const int16_t of_def[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

typedef struct {
    Huf huf;
    Fse ll, of, ml;
    int have_ll, have_of, have_ml;
    uint32_t rep[3];
    uint8_t *lit;   // literals buffer (one block: up to 128 KiB)
} Ctx;

static size_t table_for(Fse *f, int *have, int mode, const uint8_t *p, size_t n, const int16_t *def, int ndef, int deflog,
                        int maxlog, int maxsym) {
    if (mode == 0) { fse_build(f, def, ndef, deflog); *have = 1; return 0; }
    if (mode == 1) { if (!n) return ERR; fse_rle(f, p[0]); *have = 1; return 1; }
    if (mode == 2) { size_t u = fse_read(f, p, n, maxlog, maxsym); if (!u) return ERR; *have = 1; return u; }
    return *have ? 0 : ERR;   // repeat
}

static size_t block(Ctx *c, const uint8_t *p, size_t n, uint8_t *out, size_t out_pos, size_t cap) {
    // literals
    if (!n) return ERR;
    int lt = p[0] & 3, sf = (p[0] >> 2) & 3;
    size_t regen, comp = 0, hdr;
    if (lt < 2) {
        if (sf == 0 || sf == 2) regen = p[0] >> 3, hdr = 1;
        else if (sf == 1) { if (n < 2) return ERR; regen = (p[0] >> 4) + ((size_t)p[1] << 4), hdr = 2; }
        else { if (n < 3) return ERR; regen = (p[0] >> 4) + ((size_t)p[1] << 4) + ((size_t)p[2] << 12), hdr = 3; }
    } else {
        if (n < 5) return ERR;
        uint64_t h = p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32;
        if (sf < 2) regen = (h >> 4) & 0x3ff, comp = (h >> 14) & 0x3ff, hdr = 3;
        else if (sf == 2) regen = (h >> 4) & 0x3fff, comp = (h >> 18) & 0x3fff, hdr = 4;
        else regen = (h >> 4) & 0x3ffff, comp = (h >> 22) & 0x3ffff, hdr = 5;
    }
    if (regen > (128u << 10)) return ERR;
    const uint8_t *q = p + hdr;
    size_t left = n - hdr;
    if (lt == 0) { if (left < regen) return ERR; memcpy(c->lit, q, regen); q += regen, left -= regen; }
    else if (lt == 1) { if (!left) return ERR; memset(c->lit, q[0], regen); q++, left--; }
    else {
        if (left < comp) return ERR;
        const uint8_t *s = q;
        size_t sl = comp;
        if (lt == 2) {
            size_t u = huf_read(&c->huf, s, sl);
            if (!u) return ERR;
            s += u, sl -= u;
        } else if (!c->huf.ok) return ERR;
        if (sf == 0) {
            if (!huf_stream(&c->huf, s, sl, c->lit, regen)) return ERR;
        } else {
            if (sl < 6) return ERR;
            size_t l1 = s[0] | s[1] << 8, l2 = s[2] | s[3] << 8, l3 = s[4] | s[5] << 8;
            if (6 + l1 + l2 + l3 > sl) return ERR;
            size_t l4 = sl - 6 - l1 - l2 - l3, seg = (regen + 3) / 4;
            if (seg * 3 > regen) return ERR;
            const uint8_t *d = s + 6;
            if (!huf_stream(&c->huf, d, l1, c->lit, seg) || !huf_stream(&c->huf, d + l1, l2, c->lit + seg, seg) ||
                !huf_stream(&c->huf, d + l1 + l2, l3, c->lit + 2 * seg, seg) ||
                !huf_stream(&c->huf, d + l1 + l2 + l3, l4, c->lit + 3 * seg, regen - 3 * seg))
                return ERR;
        }
        q += comp, left -= comp;
    }
    // sequences
    if (!left) return ERR;
    size_t nseq;
    if (q[0] < 128) nseq = q[0], q++, left--;
    else if (q[0] < 255) { if (left < 2) return ERR; nseq = ((size_t)(q[0] - 128) << 8) + q[1], q += 2, left -= 2; }
    else { if (left < 3) return ERR; nseq = q[1] + ((size_t)q[2] << 8) + 0x7f00, q += 3, left -= 3; }
    size_t o = out_pos, lp = 0;
    if (nseq) {
        if (!left) return ERR;
        int m = q[0];
        q++, left--;
        size_t u;
        if ((u = table_for(&c->ll, &c->have_ll, m >> 6, q, left, ll_def, 36, 6, 9, 35)) == ERR) return ERR;
        q += u, left -= u;
        if ((u = table_for(&c->of, &c->have_of, (m >> 4) & 3, q, left, of_def, 29, 5, 8, 31)) == ERR) return ERR;
        q += u, left -= u;
        if ((u = table_for(&c->ml, &c->have_ml, (m >> 2) & 3, q, left, ml_def, 53, 6, 9, 52)) == ERR) return ERR;
        q += u, left -= u;
        BackBits b;
        if (!back_init(&b, q, left)) return ERR;
        uint32_t sl = (uint32_t)back_read(&b, c->ll.log), so = (uint32_t)back_read(&b, c->of.log),
                 sm = (uint32_t)back_read(&b, c->ml.log);
        for (size_t i = 0; i < nseq; i++) {
            int llc = c->ll.t[sl].sym, ofc = c->of.t[so].sym, mlc = c->ml.t[sm].sym;
            if (llc > 35 || mlc > 52 || ofc > 31) return ERR;
            uint32_t ov = (1u << ofc) + (uint32_t)back_read(&b, ofc);
            uint32_t ml = ml_base[mlc] + (uint32_t)back_read(&b, ml_bits[mlc]);
            uint32_t ll = ll_base[llc] + (uint32_t)back_read(&b, ll_bits[llc]);
            uint32_t off;
            if (ov > 3) off = ov - 3, c->rep[2] = c->rep[1], c->rep[1] = c->rep[0], c->rep[0] = off;
            else {
                int idx = (int)ov - 1 + (ll == 0);
                if (idx == 0) off = c->rep[0];
                else {
                    off = idx == 3 ? c->rep[0] - 1 : c->rep[idx];
                    if (idx > 1) c->rep[2] = c->rep[1];
                    c->rep[1] = c->rep[0], c->rep[0] = off;
                }
            }
            if (i + 1 < nseq) {
                sl = c->ll.t[sl].base + (uint32_t)back_read(&b, c->ll.t[sl].nb);
                sm = c->ml.t[sm].base + (uint32_t)back_read(&b, c->ml.t[sm].nb);
                so = c->of.t[so].base + (uint32_t)back_read(&b, c->of.t[so].nb);
            }
            if (lp + ll > regen || o + ll + ml > cap || !off || off > o + ll) return ERR;
            memcpy(out + o, c->lit + lp, ll), o += ll, lp += ll;
            const uint8_t *from = out + o - off;
            if (off >= ml) memcpy(out + o, from, ml), o += ml;
            else for (uint32_t k = 0; k < ml; k++) out[o] = out[o - off], o++;
        }
        if (b.pos != 0) return ERR;
    }
    if (o + (regen - lp) > cap) return ERR;
    memcpy(out + o, c->lit + lp, regen - lp), o += regen - lp;
    return o - out_pos;
}

size_t nx2_zstd_size(const void *src, size_t n) {
    const uint8_t *p = src;
    if (n < 6 || p[0] != 0x28 || p[1] != 0xb5 || p[2] != 0x2f || p[3] != 0xfd) return 0;
    int d = p[4], fcs = d >> 6, single = (d >> 5) & 1, did = d & 3;
    size_t o = 5 + (single ? 0 : 1) + (did == 3 ? 4 : did);
    int fs = fcs == 0 ? (single ? 1 : 0) : fcs == 1 ? 2 : fcs == 2 ? 4 : 8;
    if (!fs || o + fs > n) return 0;
    uint64_t v = 0;
    for (int i = 0; i < fs; i++) v |= (uint64_t)p[o + i] << (8 * i);
    if (fs == 2) v += 256;
    return (size_t)v;
}

size_t nx2_zstd_decompress(void *dst, size_t cap, const void *src, size_t n) {
    const uint8_t *p = src;
    uint8_t *out = dst;
    size_t o = 0, i = 0;
    Ctx *c = calloc(1, sizeof *c);
    if (!c || !(c->lit = malloc(128u << 10))) { if (c) free(c); return ERR; }
    size_t ret = ERR;
    while (i + 4 <= n) {
        uint32_t magic = p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
        if ((magic & 0xfffffff0u) == 0x184d2a50u) {   // skippable frame
            if (i + 8 > n) goto done;
            uint32_t sz = p[i + 4] | p[i + 5] << 8 | p[i + 6] << 16 | (uint32_t)p[i + 7] << 24;
            i += 8 + (size_t)sz;
            continue;
        }
        if (magic != 0xfd2fb528u || i + 5 > n) goto done;
        int d = p[i + 4], fcs = d >> 6, single = (d >> 5) & 1, did = d & 3, sum = (d >> 2) & 1;
        if (did) goto done;   // dictionaries: not supported
        i += 5 + (single ? 0 : 1);
        i += fcs == 0 ? (single ? 1 : 0) : fcs == 1 ? 2 : fcs == 2 ? 4 : 8;
        c->rep[0] = 1, c->rep[1] = 4, c->rep[2] = 8;
        c->huf.ok = 0, c->have_ll = c->have_of = c->have_ml = 0;
        for (;;) {
            if (i + 3 > n) goto done;
            uint32_t bh = p[i] | p[i + 1] << 8 | (uint32_t)p[i + 2] << 16;
            int last = bh & 1, type = (bh >> 1) & 3;
            size_t bs = bh >> 3;
            i += 3;
            if (type == 0) {
                if (i + bs > n || o + bs > cap) goto done;
                memcpy(out + o, p + i, bs), o += bs, i += bs;
            } else if (type == 1) {
                if (i + 1 > n || o + bs > cap) goto done;
                memset(out + o, p[i], bs), o += bs, i += 1;
            } else if (type == 2) {
                if (i + bs > n) goto done;
                size_t r = block(c, p + i, bs, out, o, cap);
                if (r == ERR) goto done;
                o += r, i += bs;
            } else goto done;
            if (last) break;
        }
        if (sum) i += 4;
    }
    ret = o;
done:
    free(c->lit);
    free(c);
    return ret;
}
