/*
 * X-Shot - Screenshot-Accessory fuer Atari ST/STE/TT/Falcon
 *          (+ EmuTOS, MiNT, MagiC)
 *
 * Autor: Bernd Mueller  <ragnar76@googlemail.com>
 * Web:   https://theragnarbay.org/xshot/
 *
 * - Fenster mit 4 Buttons: "Ganzer Bildschirm", "Bereich auswaehlen",
 *   "Fenster aufnehmen", "Ueber" (Kuerzel Alt+G / Alt+B / Alt+F / Alt+U)
 * - aufloesungs- und farbtiefenunabhaengig (1..8 Planes, 15/16/24/32 Bit)
 * - speichert PNG (Standard) oder TIFF (wenn Dateiname auf .TIF/.TIFF endet)
 * - PNG/TIFF-Encoder komplett selbst geschrieben, keine Fremd-Libs
 *   (PNG: "stored" Deflate-Bloecke, also unkomprimiert, aber gueltig)
 * - Schliessen ueber den Closer des Fensters
 * - laeuft ausschliesslich als Accessory (.ACC)
 *
 * Bauen:
 *   m68k-atari-mint-gcc -m68000 -O2 -fomit-frame-pointer -fno-strict-aliasing \
 *       -Wall -s -o shot.acc shot.c -lgem
 */

#include <gem.h>
#include <mint/osbind.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Einstellungen                                                       */
/* ------------------------------------------------------------------ */
#ifndef XSHOT_VERSION
#define XSHOT_VERSION "1.0"      /* vom Makefile per -DXSHOT_VERSION=... gesetzt */
#endif
#define MENU_TEXT   "X-Shot"
#define DELAY_MS    300          /* Wartezeit, bis Desktop neu gezeichnet ist */
#define BANDBYTES   32768L       /* Groesse des Zwischenpuffers beim Grab   */

long _stksize = 16384L;          /* kleiner Stack, wir brauchen nicht viel  */

/* ------------------------------------------------------------------ */
/* eigene Konstanten (unabhaengig von der gem.h-Variante)              */
/* ------------------------------------------------------------------ */
#define EV_KEYBD     0x0001
#define EV_BUTTON    0x0002
#define EV_MESAG     0x0010
#define EV_TIMER     0x0020

#define MSG_AC_OPEN   40
#define MSG_AC_CLOSE  41
#define MSG_REDRAW    20
#define MSG_TOPPED    21
#define MSG_CLOSED    22
#define MSG_MOVED     28
#define MSG_NEWTOP    33

#define K_NAME      0x0001
#define K_CLOSER    0x0002
#define K_MOVER     0x0008

#define WIN_NAME      2
#define WIN_WORK      4
#define WIN_CURR      5
#define WIN_TOP       10
#define WIN_FIRST     11
#define WIN_NEXT      12
#define CALC_BORDER 0

#define WU_END      0
#define WU_BEG      1
#define MC_END      2
#define MC_BEG      3

#define CUR_ARROW   0
#define CUR_BUSY    2            /* Sanduhr / Biene */
#define CUR_CROSS   5            /* THIN_CROSS (4 waere die grosse Hand!) */
#define MOUSE_OFF   256
#define MOUSE_ON    257

#define T_BOX       20
#define T_BUTTON    26
#define OF_SELECTABLE 0x0001
#define OF_LASTOB     0x0020
#define OS_SELECTED   0x0001
#define OF_DEFAULT    0x0002
#define OF_EXIT       0x0004
#define T_STRING      28
#define OF_3D_ACTIVATOR  0x0400   /* 3D-Button (AES mit 3D-Unterstuetzung) */
#define OF_3D_BACKGROUND 0x0800   /* 3D-Hintergrund (hellgrau)             */

#define WM_XOR      3
#define COPY_S_ONLY 3

/* ------------------------------------------------------------------ */
/* Globale Daten                                                       */
/* ------------------------------------------------------------------ */
static short  ap_id, menu_id;
static short  win = -1;
static OBJECT tree[5];
static char   winname[] = " X-Shot ";

typedef struct {
    short w, h;
    short tc;                    /* 1 = Truecolor (RGB, 3 Byte/Pixel)       */
    short ncol;                  /* Palettengroesse (nur wenn !tc)          */
    unsigned char pal[256][3];
    unsigned char *base;         /* Anfang des allozierten Blocks           */
    unsigned char *data;         /* = base + IMG_PRE; Zeilen: [0][Pixel...] */
    long stride;                 /* Bytes pro Zeile inkl. Filterbyte        */
} Image;

/*
 * Bildpuffer-Layout (PNG-Rohdatenstrom): jede Zeile beginnt mit einem
 * Filterbyte 0, danach die Pixel (tc: RGB, sonst: Index). Vor den Daten
 * liegen IMG_PRE, dahinter IMG_POST Bytes Reserve, damit der PNG-Writer
 * Kopf, Blockkoepfe, Adler32 und CRC direkt im Puffer aufbauen und die
 * Daten ohne Zwischenkopien in grossen Bloecken schreiben kann.
 */
#define IMG_PRE   16
#define IMG_POST   8

static Image img;
static short g_vh, g_cw = 8, g_ch = 8;   /* AES-Handle, Zeichenzelle */
static int   last_tiff;            /* zuletzt gewaehltes Format fuer den Namensvorschlag */

/* ------------------------------------------------------------------ */
/* Speicher                                                            */
/* ------------------------------------------------------------------ */
static void *getmem(long n)
{
    long p = (long)Mxalloc(n, 3);        /* bevorzugt TT-/Alternate-RAM */
    if (p <= 0)
        p = (long)Malloc(n);
    return (p > 0) ? (void *)p : (void *)0;
}

static void putmem(void *p)
{
    if (p) Mfree(p);
}

/* ------------------------------------------------------------------ */
/* kleine Helfer                                                       */
/* ------------------------------------------------------------------ */
static void delay_ms(short ms)
{
    short msg[8], a, b, c, d, e, f;
    evnt_multi(EV_TIMER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
               msg, (unsigned long)ms, &a, &b, &c, &d, &e, &f);
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static int intersect(short *x, short *y, short *w, short *h,
                     short ax, short ay, short aw, short ah)
{
    short x2 = (*x + *w < ax + aw) ? *x + *w : ax + aw;
    short y2 = (*y + *h < ay + ah) ? *y + *h : ay + ah;
    if (ax > *x) *x = ax;
    if (ay > *y) *y = ay;
    *w = x2 - *x;
    *h = y2 - *y;
    return (*w > 0 && *h > 0);
}

/* ================================================================== */
/*  Screen-Grab: beliebige Aufloesung / Farbtiefe                       */
/* ================================================================== */

/* Pixelwert (Hardware) -> VDI-Farbindex, fuer die Palette */
static short pix2vdi(int p, int planes)
{
    static const unsigned char t[16] =
        { 0, 2, 3, 6, 4, 7, 5, 8, 9, 10, 11, 14, 12, 15, 13, 1 };

    if (planes == 1) return (short)p;
    if (planes == 2) return (p == 0) ? 0 : (p == 1) ? 2 : (p == 2) ? 3 : 1;
    if (planes == 3) return (short)p;
    if (planes == 4) return t[p];
    if (planes == 8) {
        if (p == 15)  return 255;
        if (p == 255) return 1;
    }
    return (p < 16) ? t[p] : (short)p;
}

/* ------------------------------------------------------------------ */
/*  Truecolor: Pixelformat des Bildschirms wird zur Laufzeit gemessen   */
/*                                                                      */
/*  Statt ein Format anzunehmen (Falcon: RGB565, ...) zeichnen wir kurz */
/*  je einen Pixel in Schwarz, Rot, Gruen und Blau, lesen die rohen     */
/*  Bytes zurueck und leiten daraus ab, welche Bits zu welchem Kanal    */
/*  gehoeren. Das funktioniert fuer 15/16/24/32 Bit, jede Kanalreihen-  */
/*  folge und beide Byte-Reihenfolgen.                                  */
/* ------------------------------------------------------------------ */
#define PX_BUF 160

typedef struct {
    short          le;            /* Pixelwert little endian zusammensetzen */
    short          bytepos[3];    /* >= 0: Kanal ist genau dieses Byte      */
    unsigned long  mask[3];
    short          shift[3];
    unsigned char  tab[3][256];   /* Kanalwert -> 0..255                    */
} PixFmt;

static int g_cal_failed;          /* Messung nicht gelungen -> Standardannahme */

static unsigned long px_val(const unsigned char *s, short bpp, int le)
{
    unsigned long v = 0;
    short k;

    if (le) for (k = bpp - 1; k >= 0; k--) v = (v << 8) | s[k];
    else    for (k = 0; k < bpp; k++)      v = (v << 8) | s[k];
    return v;
}

static int contiguous(unsigned long m, short *sh, short *bits)
{
    short s = 0, b = 0;

    if (!m) return 0;
    while (!(m & 1)) { m >>= 1; s++; }
    while (m & 1)    { m >>= 1; b++; }
    if (m) return 0;
    *sh = s;
    *bits = b;
    return 1;
}

/* Rohbytes von schwarz/rot/gruen/blau -> Pixelformat; 0 = nicht bestimmbar */
static int pf_from_samples(const unsigned char *const pix[4], short bpp, PixFmt *pf)
{
    unsigned long m[2][3], black[2];
    int le, c;
    long i, maxv;

    memset(pf, 0, sizeof(*pf));
    for (le = 0; le < 2; le++) {
        black[le] = px_val(pix[0], bpp, le);
        for (c = 0; c < 3; c++)
            m[le][c] = px_val(pix[c + 1], bpp, le) ^ black[le];  /* Alpha o.ae. faellt raus */
    }
    for (le = 0; le < 2; le++) {
        short sh[3], bt[3];
        int good = 1;

        for (c = 0; c < 3; c++)
            if (!contiguous(m[le][c], &sh[c], &bt[c]) || bt[c] > 8) good = 0;
        if (!good) continue;
        if ((m[le][0] & m[le][1]) || (m[le][0] & m[le][2]) || (m[le][1] & m[le][2]))
            continue;

        pf->le = (short)le;
        for (c = 0; c < 3; c++) {
            pf->mask[c]  = m[le][c];
            pf->shift[c] = sh[c];
            maxv = (1L << bt[c]) - 1;
            for (i = 0; i <= maxv; i++)
                pf->tab[c][i] = (unsigned char)((i * 255 + maxv / 2) / maxv);
            pf->bytepos[c] = -1;
            if (bt[c] == 8 && (sh[c] & 7) == 0)
                pf->bytepos[c] = le ? sh[c] / 8 : bpp - 1 - sh[c] / 8;
        }
        return 1;
    }
    return 0;
}

/* eine Zeile Rohpixel -> RGB (3 Bytes/Pixel) */
static void convert_row(const PixFmt *pf, const unsigned char *s,
                        unsigned char *o, short w, short bpp)
{
    short xx, b0 = pf->bytepos[0], b1 = pf->bytepos[1], b2 = pf->bytepos[2];

    if (b0 >= 0 && b1 >= 0 && b2 >= 0) {          /* Kanaele sind ganze Bytes */
        for (xx = 0; xx < w; xx++, s += bpp, o += 3) {
            o[0] = s[b0];
            o[1] = s[b1];
            o[2] = s[b2];
        }
    } else {
        for (xx = 0; xx < w; xx++, s += bpp, o += 3) {
            unsigned long v = px_val(s, bpp, pf->le);
            o[0] = pf->tab[0][(v & pf->mask[0]) >> pf->shift[0]];
            o[1] = pf->tab[1][(v & pf->mask[1]) >> pf->shift[1]];
            o[2] = pf->tab[2][(v & pf->mask[2]) >> pf->shift[2]];
        }
    }
}

/* misst das Pixelformat; der Pixel (0,0) wird kurz benutzt und wiederhergestellt */
static int calibrate(short vh, short planes, PixFmt *pf)
{
    static unsigned char orig[PX_BUF], pix[4][PX_BUF];
    static const short idx[4] = { 1, 2, 3, 4 };        /* VDI: schwarz, rot, gruen, blau */
    static const short want[4][3] = {
        { 0, 0, 0 }, { 1000, 0, 0 }, { 0, 1000, 0 }, { 0, 0, 1000 }
    };
    const unsigned char *pp[4];
    short mp = (planes == 15) ? 16 : planes;
    short bpp = (mp + 7) >> 3;
    short i, c, rgb[3], pxy[4], cp[8], clip[4];
    MFDB scr, a;

    if (bpp < 2 || bpp > 4 || 2L * mp > PX_BUF) return 0;

    for (i = 0; i < 4; i++) {                 /* sind die VDI-Farben wirklich rein? */
        vq_color(vh, idx[i], 1, rgb);
        for (c = 0; c < 3; c++) {
            short d = rgb[c] - want[i][c];
            if (d < -100 || d > 100) return 0;
        }
    }

    memset(orig, 0, sizeof(orig));
    memset(pix, 0, sizeof(pix));
    memset(&scr, 0, sizeof(scr));             /* Bildschirm */
    for (i = 0; i < 8; i++) cp[i] = 0;        /* 1x1 Pixel bei (0,0) */
    a.fd_addr = orig;  a.fd_w = 1;  a.fd_h = 1;
    a.fd_wdwidth = 1;  a.fd_stand = 0;  a.fd_nplanes = mp;
    a.fd_r1 = a.fd_r2 = a.fd_r3 = 0;
    vro_cpyfm(vh, COPY_S_ONLY, cp, &scr, &a);          /* Original sichern */

    vs_clip(vh, 0, clip);
    vswr_mode(vh, 1);                                  /* ersetzen */
    vsf_interior(vh, 1);                               /* massiv */
    vsf_perimeter(vh, 0);
    pxy[0] = pxy[1] = pxy[2] = pxy[3] = 0;
    for (i = 0; i < 4; i++) {
        vsf_color(vh, idx[i]);
        v_bar(vh, pxy);
        a.fd_addr = pix[i];
        vro_cpyfm(vh, COPY_S_ONLY, cp, &scr, &a);
    }

    a.fd_addr = orig;                                  /* Original zurueck */
    vro_cpyfm(vh, COPY_S_ONLY, cp, &a, &scr);

    for (i = 0; i < 4; i++) pp[i] = pix[i];
    return pf_from_samples(pp, bpp, pf);
}

/*
 * Kopiert den Bereich in "im". Bei <= 8 Planes entsteht ein Index-Bild
 * mit Palette, bei > 8 Planes ein RGB-Bild. Gelesen wird baenderweise,
 * damit der Zwischenpuffer klein bleibt.
 *
 * Annahmen bei Truecolor (Motorola-Bytefolge, wie Falcon/NVDI):
 *   15 Bit: xRRRRRGGGGGBBBBB   16 Bit: RRRRRGGGGGGBBBBB
 *   24 Bit: R,G,B              32 Bit: x,R,G,B
 */
static int grab(short vh, short x, short y, short w, short h,
                short planes, Image *im)
{
    short wdw = (w + 15) >> 4;
    short mp  = (planes == 15) ? 16 : planes;
    int   tc  = (planes > 8);
    long  rowbytes = (long)wdw * 2 * mp;
    long  bh = BANDBYTES / rowbytes;
    short row, n, r, p, xx, k, i;
    unsigned char *src, *std = 0, *out;
    MFDB scr, a, b;
    short pxy[8];
    static PixFmt pf;
    int have_pf = 0;

    memset(im, 0, sizeof(*im));
    im->w = w;
    im->h = h;
    im->tc = (short)tc;
    im->stride = (long)w * (tc ? 3 : 1) + 1;           /* +1 = Filterbyte */

    if (bh < 1) bh = 1;
    if (bh > h) bh = h;

    src = getmem(bh * rowbytes);
    if (!tc) std = getmem(bh * rowbytes);
    im->base = getmem(IMG_PRE + (long)h * im->stride + IMG_POST);

    if (!src || (!tc && !std) || !im->base) {
        putmem(src); putmem(std); putmem(im->base);
        im->base = im->data = 0;
        return 0;
    }
    im->data = im->base + IMG_PRE;

    if (tc) {
        have_pf = calibrate(vh, planes, &pf);   /* Pixelformat messen */
        g_cal_failed = !have_pf;
    }

    memset(&scr, 0, sizeof(scr));            /* fd_addr = 0 -> Bildschirm */

    for (row = 0; row < h; row += n) {
        n = h - row;
        if (n > bh) n = (short)bh;

        a.fd_addr = src;  a.fd_w = w;  a.fd_h = n;
        a.fd_wdwidth = wdw;  a.fd_stand = 0;  a.fd_nplanes = mp;
        a.fd_r1 = a.fd_r2 = a.fd_r3 = 0;

        pxy[0] = x;          pxy[1] = y + row;
        pxy[2] = x + w - 1;  pxy[3] = y + row + n - 1;
        pxy[4] = 0;          pxy[5] = 0;
        pxy[6] = w - 1;      pxy[7] = n - 1;
        vro_cpyfm(vh, COPY_S_ONLY, pxy, &scr, &a);

        if (!tc) {
            /* geraeteabhaengig -> Standardformat (Planes hintereinander) */
            b = a;
            b.fd_addr  = std;
            b.fd_stand = 1;
            vr_trnfm(vh, &a, &b);

            for (r = 0; r < n; r++) {
                out = im->data + (long)(row + r) * im->stride + 1;
                out[-1] = 0;                              /* Filterbyte */
                memset(out, 0, w);
                for (p = 0; p < planes; p++) {
                    const unsigned char *pr =
                        std + ((long)p * n + r) * wdw * 2;
                    unsigned char bit = (unsigned char)(1 << p);
                    for (xx = 0; xx < wdw * 2; xx++) {
                        unsigned char bv = pr[xx];
                        if (bv) {
                            unsigned char *o = out + (xx << 3);
                            short lim = w - (xx << 3);
                            if (lim > 8) lim = 8;
                            for (k = 0; k < lim; k++)
                                if (bv & (0x80 >> k)) o[k] |= bit;
                        }
                    }
                }
            }
        } else {
            short bpp = (planes + 7) >> 3;
            for (r = 0; r < n; r++) {
                const unsigned char *s = src + (long)r * rowbytes;
                unsigned char *o = im->data + (long)(row + r) * im->stride + 1;
                o[-1] = 0;                                /* Filterbyte */
                if (have_pf) {
                    convert_row(&pf, s, o, w, bpp);       /* gemessenes Format */
                } else if (bpp == 2) {                    /* Standardannahmen */
                    for (xx = 0; xx < w; xx++, s += 2, o += 3) {
                        unsigned v = ((unsigned)s[0] << 8) | s[1];
                        unsigned rr, gg, bb;
                        if (planes == 16) {
                            rr = (v >> 11) & 31; gg = (v >> 5) & 63; bb = v & 31;
                            o[0] = (unsigned char)((rr << 3) | (rr >> 2));
                            o[1] = (unsigned char)((gg << 2) | (gg >> 4));
                            o[2] = (unsigned char)((bb << 3) | (bb >> 2));
                        } else {
                            rr = (v >> 10) & 31; gg = (v >> 5) & 31; bb = v & 31;
                            o[0] = (unsigned char)((rr << 3) | (rr >> 2));
                            o[1] = (unsigned char)((gg << 3) | (gg >> 2));
                            o[2] = (unsigned char)((bb << 3) | (bb >> 2));
                        }
                    }
                } else if (bpp == 3) {
                    for (xx = 0; xx < w; xx++, s += 3, o += 3) {
                        o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
                    }
                } else {                                  /* 32 Bit: xRGB */
                    for (xx = 0; xx < w; xx++, s += 4, o += 3) {
                        o[0] = s[1]; o[1] = s[2]; o[2] = s[3];
                    }
                }
            }
        }
    }

    if (!tc) {
        short rgb[3];
        im->ncol = (short)(1 << planes);
        for (i = 0; i < im->ncol; i++) {
            vq_color(vh, pix2vdi(i, planes), 1, rgb);
            im->pal[i][0] = (unsigned char)((rgb[0] * 255L + 500) / 1000);
            im->pal[i][1] = (unsigned char)((rgb[1] * 255L + 500) / 1000);
            im->pal[i][2] = (unsigned char)((rgb[2] * 255L + 500) / 1000);
        }
    }

    putmem(src);
    putmem(std);
    return 1;
}

/* ================================================================== */
/*  Datei-Ausgabe                                                       */
/*  (Bildpuffer-Layout siehe Image / IMG_PRE / IMG_POST)                */
/* ================================================================== */
static long fh;
static int  werr;

static void wr(const void *p, long n)
{
    if (!werr && Fwrite((short)fh, n, p) != n)
        werr = 1;
}

static int pal_depth(int ncol)
{
    return (ncol <= 2) ? 1 : (ncol <= 4) ? 2 : (ncol <= 16) ? 4 : 8;
}

/* Index-Zeile auf 1/2/4/8 Bit packen */
static void pack_row(unsigned char *out, const unsigned char *in,
                     int w, int depth)
{
    int x, n = 0;
    unsigned acc = 0;

    if (depth == 8) { memcpy(out, in, w); return; }
    for (x = 0; x < w; x++) {
        acc = (acc << depth) | in[x];
        n += depth;
        if (n == 8) { *out++ = (unsigned char)acc; acc = 0; n = 0; }
    }
    if (n) *out++ = (unsigned char)(acc << (8 - n));
}

/*
 * Index-Bild (1 Byte/Pixel, mit Filterbyte) im Puffer auf 1/2/4 Bit je Pixel
 * verdichten. Das geht "in place": Ziel liegt nie hinter der Quelle.
 */
static void pack_indexed(Image *im, int depth)
{
    long rl = ((long)im->w * depth + 7) / 8;
    long y, stride0 = (long)im->w + 1;

    for (y = 0; y < im->h; y++) {
        unsigned char *src = im->data + y * stride0 + 1;
        unsigned char *dst = im->data + y * (rl + 1);
        dst[0] = 0;
        pack_row(dst + 1, src, im->w, depth);
    }
    im->stride = rl + 1;
}

/* ---------------------------- PNG --------------------------------- */
static unsigned long crc_tab[4][256];     /* "slicing-by-4": 4 Bytes pro Schritt */
static int           crc_ready;

static void make_crc(void)
{
    unsigned long c;
    int n, k;

    if (crc_ready) return;
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        crc_tab[0][n] = c;
    }
    for (n = 0; n < 256; n++) {
        c = crc_tab[0][n];
        for (k = 1; k < 4; k++) {
            c = crc_tab[0][c & 0xff] ^ (c >> 8);
            crc_tab[k][n] = c;
        }
    }
    crc_ready = 1;
}

static unsigned long crc_upd(unsigned long crc, const unsigned char *p, long n)
{
    while (n >= 4) {
        crc ^= (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
               ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
        crc = crc_tab[3][crc & 0xff] ^ crc_tab[2][(crc >> 8) & 0xff] ^
              crc_tab[1][(crc >> 16) & 0xff] ^ crc_tab[0][crc >> 24];
        p += 4;
        n -= 4;
    }
    while (n-- > 0)
        crc = crc_tab[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc;
}

static unsigned long adler32(const unsigned char *p, long len)
{
    unsigned long a = 1, s = 0;
    long n;

    while (len > 0) {
        n = (len > 5552) ? 5552 : len;     /* so lange geht es ohne "mod" */
        len -= n;
        while (n >= 8) {
            a += p[0]; s += a;  a += p[1]; s += a;
            a += p[2]; s += a;  a += p[3]; s += a;
            a += p[4]; s += a;  a += p[5]; s += a;
            a += p[6]; s += a;  a += p[7]; s += a;
            p += 8;
            n -= 8;
        }
        while (n-- > 0) { a += *p++; s += a; }
        a %= 65521UL;
        s %= 65521UL;
    }
    return (s << 16) | a;
}

static void chunk(const char *type, const unsigned char *data, long len)
{
    unsigned char hdr[8], tmp[4];
    unsigned long crc;

    put32(hdr, (unsigned long)len);
    memcpy(hdr + 4, type, 4);
    wr(hdr, 8);
    crc = crc_upd(0xFFFFFFFFUL, hdr + 4, 4);
    if (len > 0) {
        wr(data, len);
        crc = crc_upd(crc, data, len);
    }
    put32(tmp, crc ^ 0xFFFFFFFFUL);
    wr(tmp, 4);
}

/*
 * PNG: ein einziger IDAT-Chunk mit zlib-Stream aus "stored" Bloecken (je bis
 * 65535 Bytes). Der Bildpuffer ist schon der Rohdatenstrom. Pro Block wird
 * der 5-Byte-Blockkopf in die (bereits geschriebenen) 5 Bytes vor dem Block
 * geschrieben, sodass jeder Block mit EINEM Fwrite direkt aus dem Puffer
 * rausgeht. Der Puffer wird dabei zerstoert.
 */
static int write_png(Image *im)
{
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char hdr[13], plte[768], *start, *w0, *bh, *cs;
    int depth = im->tc ? 8 : pal_depth(im->ncol);
    long N = (long)im->h * im->stride;               /* Rohdatenstrom */
    long nblocks = (N + 65534L) / 65535L;
    long off = 0, k, n, wl;
    unsigned long crc, adler;
    int i, fin;

    make_crc();
    put32(hdr, im->w);
    put32(hdr + 4, im->h);
    hdr[8] = (unsigned char)depth;
    hdr[9] = im->tc ? 2 : 3;
    hdr[10] = hdr[11] = hdr[12] = 0;

    wr(sig, 8);
    chunk("IHDR", hdr, 13);
    if (!im->tc) {
        for (i = 0; i < im->ncol; i++) {
            plte[i * 3]     = im->pal[i][0];
            plte[i * 3 + 1] = im->pal[i][1];
            plte[i * 3 + 2] = im->pal[i][2];
        }
        chunk("PLTE", plte, im->ncol * 3L);
    }

    adler = adler32(im->data, N);
    crc = 0xFFFFFFFFUL;

    for (k = 0; k < nblocks && !werr; k++) {
        n = N - off;
        if (n > 65535L) n = 65535L;
        fin = (k == nblocks - 1);
        start = im->data + off;

        if (k == 0) {                      /* Chunk-Kopf + zlib-Kopf + Blockkopf */
            w0 = start - 15;
            put32(w0, (unsigned long)(2 + N + 5 * nblocks + 4));
            memcpy(w0 + 4, "IDAT", 4);
            w0[8] = 0x78;
            w0[9] = 0x01;
            bh = w0 + 10;
            cs = w0 + 4;                   /* CRC beginnt beim Chunk-Typ */
        } else {
            w0 = start - 5;                /* ueberschreibt schon Geschriebenes */
            bh = w0;
            cs = w0;
        }
        bh[0] = (unsigned char)fin;
        bh[1] = (unsigned char)(n & 0xff);
        bh[2] = (unsigned char)(n >> 8);
        bh[3] = (unsigned char)(~n & 0xff);
        bh[4] = (unsigned char)((~n >> 8) & 0xff);

        wl = (start + n) - w0;
        if (fin) {                         /* Adler32 + CRC hinten anhaengen */
            put32(start + n, adler);
            wl += 4;
        }
        crc = crc_upd(crc, cs, (w0 + wl) - cs);
        if (fin) {
            put32(w0 + wl, crc ^ 0xFFFFFFFFUL);
            wl += 4;
        }
        wr(w0, wl);
        off += n;
    }
    chunk("IEND", 0, 0);
    return !werr;
}

/* ---------------------------- TIFF -------------------------------- */
static void ifd_entry(unsigned char *p, unsigned tag, unsigned type,
                      unsigned long count, unsigned long val)
{
    put16(p, tag);
    put16(p + 2, type);
    put32(p + 4, count);
    if (type == 3 && count == 1) {
        put16(p + 8, (unsigned)val);
        p[10] = p[11] = 0;
    } else
        put32(p + 8, val);
}

static int write_tiff(Image *im)
{
    static unsigned char cm[1536];
    unsigned char buf[140], *e;
    int tc = im->tc;
    int depth = tc ? 8 : pal_depth(im->ncol);
    int nent = tc ? 9 : 10;
    long rowlen = im->stride - 1;                    /* ohne Filterbyte */
    long ifdsize = 2 + nent * 12L + 4;
    long extra_off = 8 + ifdsize;
    long extra_len = tc ? 6L : (6L << depth);
    long data_off = extra_off + extra_len;
    long y;
    int ch, i;

    memset(buf, 0, sizeof(buf));
    buf[0] = buf[1] = 'M';
    buf[2] = 0; buf[3] = 42;
    put32(buf + 4, 8);
    put16(buf + 8, nent);
    e = buf + 10;
    ifd_entry(e, 256, 4, 1, im->w);                         e += 12;
    ifd_entry(e, 257, 4, 1, im->h);                         e += 12;
    ifd_entry(e, 258, 3, tc ? 3 : 1, tc ? extra_off : depth); e += 12;
    ifd_entry(e, 259, 3, 1, 1);                             e += 12;
    ifd_entry(e, 262, 3, 1, tc ? 2 : 3);                    e += 12;
    ifd_entry(e, 273, 4, 1, data_off);                      e += 12;
    ifd_entry(e, 277, 3, 1, tc ? 3 : 1);                    e += 12;
    ifd_entry(e, 278, 4, 1, im->h);                         e += 12;
    ifd_entry(e, 279, 4, 1, rowlen * im->h);                e += 12;
    if (!tc)
        ifd_entry(e, 320, 3, 3L << depth, extra_off);
    wr(buf, 8 + ifdsize);

    if (tc) {
        static const unsigned char bps[6] = { 0, 8, 0, 8, 0, 8 };
        wr(bps, 6);
    } else {
        memset(cm, 0, sizeof(cm));
        for (ch = 0; ch < 3; ch++)
            for (i = 0; i < im->ncol; i++)
                put16(cm + ((((long)ch << depth) + i) * 2),
                      im->pal[i][ch] * 257u);
        wr(cm, extra_len);
    }

    for (y = 0; y < im->h && !werr; y++)             /* Zeilen direkt aus dem Puffer */
        wr(im->data + y * im->stride + 1, rowlen);
    return !werr;
}

static int write_file(const char *name, Image *im, int tiff)
{
    int ok;

    graf_mouse(CUR_BUSY, 0);                         /* Sanduhr waehrend des Schreibens */
    if (!im->tc) {
        int d = pal_depth(im->ncol);
        if (d < 8) pack_indexed(im, d);
    }
    fh = Fcreate(name, 0);
    if (fh < 0) {
        graf_mouse(CUR_ARROW, 0);
        return 0;
    }
    werr = 0;
    ok = tiff ? write_tiff(im) : write_png(im);
    Fclose((short)fh);
    graf_mouse(CUR_ARROW, 0);
    return ok && !werr;
}

/* ================================================================== */
/*  Dateiauswahl + Speichern                                            */
/* ================================================================== */
/* hoechste vorhandene Nummer von <prefix>nnnn.* im Ordner suchen, +1 liefern */
static int next_number(const char *fpath, const char *prefix)
{
    static unsigned char dta[44];
    char pat[170], *p;
    void *olddta;
    int max = 0, plen = (int)strlen(prefix), k;
    long r;

    strcpy(pat, fpath);
    p = strrchr(pat, '\\');
    if (p) p[1] = 0; else pat[0] = 0;
    strcat(pat, prefix);
    strcat(pat, "*.*");

    olddta = (void *)Fgetdta();
    Fsetdta(dta);                          /* eigener DTA, fremden nicht zerstoeren */
    r = Fsfirst(pat, 0);
    while (r == 0) {
        const char *n = (const char *)dta + 30;       /* d_fname */
        for (k = 0; k < plen; k++)
            if ((n[k] & 0xDF) != prefix[k]) break;
        if (k == plen) {
            int v = 0, d = 0;
            n += plen;
            while (*n >= '0' && *n <= '9' && d < 4) {
                v = v * 10 + (*n - '0');
                n++; d++;
            }
            if (d > 0 && *n == '.' && v > max) max = v;
        }
        r = Fsnext();
    }
    Fsetdta(olddta);
    return (max >= 9999) ? 9999 : max + 1;
}

static void save_image(Image *im, const char *prefix)
{
    static char fpath[128];
    static char fname[20] = "SHOT0001.PNG";
    char full[170];
    short btn = 0;
    char *p;
    int tiff = 0, len;

    if (!fpath[0]) {
        short drv = (short)Dgetdrv();
        fpath[0] = (char)('A' + drv);
        fpath[1] = ':';
        fpath[2] = 0;
        Dgetpath(fpath + 2, drv + 1);
        len = (int)strlen(fpath);
        if (fpath[len - 1] != '\\') strcat(fpath, "\\");
        strcat(fpath, "*.*");
    }

    graf_mouse(CUR_ARROW, 0);

    {                                           /* Namensvorschlag SHOTnnnn.ext */
        int num = next_number(fpath, prefix);
        int pl = (int)strlen(prefix);
        strcpy(fname, prefix);
        fname[pl]     = (char)('0' + (num / 1000) % 10);
        fname[pl + 1] = (char)('0' + (num / 100) % 10);
        fname[pl + 2] = (char)('0' + (num / 10) % 10);
        fname[pl + 3] = (char)('0' + num % 10);
        strcpy(fname + pl + 4, last_tiff ? ".TIF" : ".PNG");
    }

    for (;;) {                                  /* bis Name ok oder Abbruch */
        char msg[110];
        long t;

        btn = 0;
        fsel_exinput(fpath, fname, &btn, "Screenshot speichern");
        if (!btn || !fname[0]) return;

        strcpy(full, fpath);
        p = strrchr(full, '\\');
        if (p) p[1] = 0; else full[0] = 0;
        strcat(full, fname);

        tiff = 0;
        p = strrchr(fname, '.');
        if (!p) {
            strcat(full, ".PNG");
        } else {
            char e1 = (char)(p[1] & 0xDF), e2 = (char)(p[2] & 0xDF);
            char e3 = (char)(p[3] & 0xDF), e4 = p[4] ? (char)(p[4] & 0xDF) : 0;
            if (e1 == 'T' && e2 == 'I' && e3 == 'F' &&
                (e4 == 0 || (e4 == 'F' && !p[5])))
                tiff = 1;
        }

        t = Fopen(full, 0);                     /* existiert die Datei schon? */
        if (t >= 0) {
            Fclose((short)t);
            strcpy(msg, "[2][");
            strcat(msg, fname);
            strcat(msg, "|existiert bereits.|Ueberschreiben?][Ja|Nein]");
            if (form_alert(2, msg) != 1)
                continue;                       /* anderen Namen waehlen */
        }
        break;
    }

    last_tiff = tiff;                           /* Format fuer naechsten Vorschlag merken */
    if (!write_file(full, im, tiff))
        form_alert(1, "[3][Speichern fehlgeschlagen!|Disk voll oder schreibgeschuetzt?][Abbruch]");
}

/* ================================================================== */
/*  Gummiband-Auswahl                                                   */
/* ================================================================== */
static void frame(short vh, short x0, short y0, short x1, short y1)
{
    short p[10];
    short xa = (x0 < x1) ? x0 : x1, xb = (x0 < x1) ? x1 : x0;
    short ya = (y0 < y1) ? y0 : y1, yb = (y0 < y1) ? y1 : y0;

    p[0] = xa; p[1] = ya;  p[2] = xb; p[3] = ya;
    p[4] = xb; p[5] = yb;  p[6] = xa; p[7] = yb;
    p[8] = xa; p[9] = ya + 1;          /* Ecke nicht doppelt XOR-en */
    v_pline(vh, 5, p);
}

/* 1 = Bereich gewaehlt, 0 = abgebrochen (rechte Maustaste / zu klein) */
static int select_rect(short vh, short sw, short sh,
                       short *rx, short *ry, short *rw, short *rh)
{
    short mx, my, mb, ks, x0, y0, x1, y1, clip[4];

    graf_mouse(CUR_CROSS, 0);

    do {                                   /* auf Tastendruck warten */
        graf_mkstate(&mx, &my, &mb, &ks);
        if (!(mb & 3)) delay_ms(10);
    } while (!(mb & 3));

    if (mb & 2) {                          /* rechte Taste = abbrechen */
        graf_mouse(CUR_ARROW, 0);
        do { graf_mkstate(&mx, &my, &mb, &ks); delay_ms(10); } while (mb);
        return 0;
    }

    vs_clip(vh, 0, clip);
    vswr_mode(vh, WM_XOR);
    vsl_type(vh, 1);
    vsl_color(vh, 1);
    vsl_width(vh, 1);

    if (mx < 0) mx = 0;  if (mx >= sw) mx = sw - 1;
    if (my < 0) my = 0;  if (my >= sh) my = sh - 1;
    x0 = x1 = mx;
    y0 = y1 = my;
    graf_mouse(MOUSE_OFF, 0);              /* Zeiger weg, sonst Artefakte */
    frame(vh, x0, y0, x1, y1);
    graf_mouse(MOUSE_ON, 0);

    do {
        graf_mkstate(&mx, &my, &mb, &ks);
        if (mx < 0) mx = 0;  if (mx >= sw) mx = sw - 1;
        if (my < 0) my = 0;  if (my >= sh) my = sh - 1;
        if (mx != x1 || my != y1) {
            graf_mouse(MOUSE_OFF, 0);
            frame(vh, x0, y0, x1, y1);     /* alten Rahmen loeschen */
            x1 = mx;
            y1 = my;
            frame(vh, x0, y0, x1, y1);
            graf_mouse(MOUSE_ON, 0);
        } else
            delay_ms(10);
    } while (mb & 1);

    graf_mouse(MOUSE_OFF, 0);
    frame(vh, x0, y0, x1, y1);             /* Rahmen entfernen */
    graf_mouse(MOUSE_ON, 0);
    graf_mouse(CUR_ARROW, 0);              /* sofort wieder normaler Pfeil */

    *rx = (x0 < x1) ? x0 : x1;
    *ry = (y0 < y1) ? y0 : y1;
    *rw = (x0 < x1) ? x1 - x0 + 1 : x0 - x1 + 1;
    *rh = (y0 < y1) ? y1 - y0 + 1 : y0 - y1 + 1;

    return (*rw >= 2 && *rh >= 2);
}

/* Fenster per Mausklick waehlen; das Fenster unter dem Zeiger wird markiert */
static int select_window(short vh, short sw, short sh,
                         short *rx, short *ry, short *rw, short *rh)
{
    short mx, my, mb, ks, wh, last = -1, x, y, w, h, clip[4];
    short lx = 0, ly = 0, lw = 0, lh = 0;
    int have = 0;

    graf_mouse(CUR_CROSS, 0);
    vs_clip(vh, 0, clip);
    vswr_mode(vh, WM_XOR);
    vsl_type(vh, 1);
    vsl_color(vh, 1);
    vsl_width(vh, 1);

    for (;;) {
        graf_mkstate(&mx, &my, &mb, &ks);
        wh = wind_find(mx, my);
        if (wh != last) {
            graf_mouse(MOUSE_OFF, 0);
            if (have) {
                frame(vh, lx, ly, lx + lw - 1, ly + lh - 1);
                have = 0;
            }
            if (wh > 0) {
                wind_get(wh, WIN_CURR, &x, &y, &w, &h);
                if (x < 0) { w += x; x = 0; }
                if (y < 0) { h += y; y = 0; }
                if (x + w > sw) w = sw - x;
                if (y + h > sh) h = sh - y;
                if (w >= 2 && h >= 2) {
                    lx = x; ly = y; lw = w; lh = h;
                    frame(vh, lx, ly, lx + lw - 1, ly + lh - 1);
                    have = 1;
                }
            }
            graf_mouse(MOUSE_ON, 0);
            last = wh;
        }
        if (mb & 3) break;
        delay_ms(10);
    }

    if (have) {                              /* Markierung entfernen */
        graf_mouse(MOUSE_OFF, 0);
        frame(vh, lx, ly, lx + lw - 1, ly + lh - 1);
        graf_mouse(MOUSE_ON, 0);
    }
    graf_mouse(CUR_ARROW, 0);
    {
        short m2 = mb;                       /* auf Loslassen warten */
        while (m2) { graf_mkstate(&mx, &my, &m2, &ks); delay_ms(10); }
    }

    if ((mb & 2) || !have) return 0;         /* rechte Taste oder kein Fenster */
    *rx = lx; *ry = ly; *rw = lw; *rh = lh;
    return 1;
}

/* ================================================================== */
/*  Fenster / Dialog                                                    */
/* ================================================================== */
static void init_tree(void)
{
    enum { NOBJ = 5 };
    static const short lay[NOBJ][4] = {
        {0, 0, 28, 13}, {2, 1, 24, 2}, {2, 4, 24, 2}, {2, 7, 24, 2}, {2, 10, 24, 2}
    };
    static char *txt[NOBJ] = {
        0, "Ganzer Bildschirm", "Bereich ausw\204hlen",
        "Fenster aufnehmen", "\232ber"
    };
    short i, dummy, phys, ext[57];
    int   col16;

    /* >= 16 Farben (4 Planes oder mehr, inkl. Truecolor) -> 3D + hellgrau */
    phys = graf_handle(&g_cw, &g_ch, &dummy, &dummy);
    g_vh = phys;
    vq_extnd(phys, 1, ext);
    col16 = (ext[4] >= 4);

    for (i = 0; i < NOBJ; i++) {
        OBJECT *o = &tree[i];
        o->ob_next   = (i == 0) ? -1 : (i == NOBJ - 1) ? 0 : i + 1;
        o->ob_head   = (i == 0) ? 1 : -1;
        o->ob_tail   = (i == 0) ? NOBJ - 1 : -1;
        o->ob_type   = (i == 0) ? T_BOX : T_BUTTON;
        o->ob_flags  = (i == 0) ? (col16 ? OF_3D_BACKGROUND : 0)
                                : (OF_SELECTABLE | (col16 ? OF_3D_ACTIVATOR : 0));
        if (i == NOBJ - 1) o->ob_flags |= OF_LASTOB;
        o->ob_state  = 0;
        o->ob_x      = lay[i][0];
        o->ob_y      = lay[i][1];
        o->ob_width  = lay[i][2];
        o->ob_height = lay[i][3];
        if (i == 0)
            *(long *)&o->ob_spec = col16 ? 0x00001178L  /* hellgrau (VDI-Farbe 8) */
                                         : 0x00001170L; /* weiss */
        else
            *(char **)&o->ob_spec = txt[i];
        rsrc_obfix(tree, i);
    }
}

static void place_tree(void)
{
    short w, h;
    wind_get(win, WIN_WORK, &tree[0].ob_x, &tree[0].ob_y, &w, &h);
}

/* Kuerzel-Buchstaben (erster Buchstabe des Buttons) unterstreichen */
static void draw_underlines(short cx, short cy, short cw, short ch)
{
    short clip[4], pxy[4], i, ox, oy, len, x0;

    clip[0] = cx;  clip[1] = cy;
    clip[2] = cx + cw - 1;  clip[3] = cy + ch - 1;
    vs_clip(g_vh, 1, clip);
    vswr_mode(g_vh, 1);                       /* 1 = ersetzen */
    vsl_type(g_vh, 1);
    vsl_color(g_vh, 1);
    vsl_width(g_vh, 1);

    for (i = 1; i <= 4; i++) {
        const char *t = *(char **)&tree[i].ob_spec;
        objc_offset(tree, i, &ox, &oy);
        len = (short)strlen(t);
        x0 = ox + (tree[i].ob_width - len * g_cw) / 2;   /* Text ist zentriert */
        pxy[0] = x0;
        pxy[2] = x0 + g_cw - 1;
        pxy[1] = pxy[3] = oy + (tree[i].ob_height - g_ch) / 2 + g_ch - 1;
        v_pline(g_vh, 2, pxy);
    }
    vs_clip(g_vh, 0, clip);
}

static void redraw(short x, short y, short w, short h)
{
    short rx, ry, rw, rh;

    if (win < 0) return;
    wind_update(WU_BEG);
    graf_mouse(MOUSE_OFF, 0);
    wind_get(win, WIN_FIRST, &rx, &ry, &rw, &rh);
    while (rw > 0 && rh > 0) {
        if (intersect(&rx, &ry, &rw, &rh, x, y, w, h)) {
            objc_draw(tree, 0, 8, rx, ry, rw, rh);
            draw_underlines(rx, ry, rw, rh);
        }
        wind_get(win, WIN_NEXT, &rx, &ry, &rw, &rh);
    }
    graf_mouse(MOUSE_ON, 0);
    wind_update(WU_END);
}

static void redraw_obj(short obj)
{
    short ox, oy;
    objc_offset(tree, obj, &ox, &oy);
    redraw(ox - 3, oy - 3, tree[obj].ob_width + 6, tree[obj].ob_height + 6);
}

static void open_window(void)
{
    short dx, dy, dw, dh, bx, by, bw, bh;
    short kind = K_NAME | K_CLOSER | K_MOVER;

    init_tree();
    wind_get(0, WIN_WORK, &dx, &dy, &dw, &dh);
    wind_calc(CALC_BORDER, kind, 0, 0, tree[0].ob_width, tree[0].ob_height,
              &bx, &by, &bw, &bh);
    bx = dx + (dw - bw) / 2;
    by = dy + (dh - bh) / 2;

    win = wind_create(kind, bx, by, bw, bh);
    if (win < 0) {
        form_alert(1, "[3][Kein Fenster mehr frei!][Abbruch]");
        return;
    }
    wind_set_str(win, WIN_NAME, winname);
    wind_open(win, bx, by, bw, bh);
    place_tree();
}

static void close_window(void)
{
    if (win >= 0) {
        wind_close(win);
        wind_delete(win);
        win = -1;
    }
}

/* ================================================================== */
/*  Aktionen                                                            */
/* ================================================================== */
/*  Speicher-Check vor der Aufnahme                                     */
/* ================================================================== */

/* Bedarf in Bytes: Bildpuffer + Band-Puffer(n) wie in grab(), plus etwas Reserve */
static long mem_needed(short w, short h, short planes)
{
    int   tc = (planes > 8);
    short mp = (planes == 15) ? 16 : planes;
    short wdw = (w + 15) >> 4;
    long  rowbytes = (long)wdw * 2 * mp;
    long  bh = BANDBYTES / rowbytes;
    long  image = IMG_PRE + (long)h * ((long)w * (tc ? 3 : 1) + 1) + IMG_POST;

    if (bh < 1) bh = 1;
    if (bh > h) bh = h;
    return image + bh * rowbytes * (tc ? 1 : 2) + 16384L;
}

/* groesster freier zusammenhaengender Block (ST-RAM oder Alternate RAM);
 * -1 = unbekannt, dann wird nicht vorab geprueft */
static long mem_avail(void)
{
    long a = (long)Mxalloc(-1L, 0);         /* ST-RAM                 */
    long b = (long)Mxalloc(-1L, 1);         /* Alternate-/TT-RAM      */
    long best = 0;

    if (a > best) best = a;
    if (b > best) best = b;
    if (best <= 0) {                        /* TOS ohne Mxalloc */
        a = (long)Malloc(-1L);
        if (a > best) best = a;
    }
    return (best > 0) ? best : -1L;
}

static char *put_long(char *d, long v)
{
    char t[12];
    int n = 0;

    if (v <= 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) *d++ = t[--n];
    return d;
}

/* "6.2 MB" bzw. "640 KB" */
static char *put_size(char *d, long bytes)
{
    long kb = (bytes + 512) >> 10;

    if (kb >= 1024) {
        long t = (kb * 10 + 512) >> 10;     /* MB mit einer Nachkommastelle */
        d = put_long(d, t / 10);
        *d++ = '.';
        *d++ = (char)('0' + t % 10);
        strcpy(d, " MB");
    } else {
        d = put_long(d, kb);
        strcpy(d, " KB");
    }
    return d + strlen(d);
}

static void alert_lowmem(long need, long avail)
{
    char msg[160], *p = msg;

    strcpy(p, "[3][Zu wenig Speicher!|Ben\224tigt: ");
    p += strlen(p);
    p = put_size(p, need);
    strcpy(p, "|Frei:     ");
    p += strlen(p);
    p = put_size(p, avail);
    strcpy(p, "|Tipp: Kleineren Bereich|w\204hlen.][Abbruch]");
    form_alert(1, msg);
}

/* ================================================================== */
static void do_capture(int mode)          /* 0 = ganz, 1 = Bereich, 2 = Fenster */
{
    short vh, wi[11], wo[57], ext[57], dummy, i;
    short rx = 0, ry = 0, rw, rh, sw, sh, planes;
    int ok = 1, got = 0, lowmem = 0;
    long need = 0, avail = 0;

    close_window();
    delay_ms(DELAY_MS);               /* Desktop soll sich neu zeichnen */

    vh = graf_handle(&dummy, &dummy, &dummy, &dummy);
    for (i = 0; i < 10; i++) wi[i] = 1;
    wi[10] = 2;
    v_opnvwk(wi, &vh, wo);
    if (vh <= 0) {
        form_alert(1, "[3][Workstation konnte nicht|geoeffnet werden!][Abbruch]");
        return;
    }
    sw = wo[0] + 1;
    sh = wo[1] + 1;
    rw = sw;
    rh = sh;
    vq_extnd(vh, 1, ext);
    planes = ext[4];

    wind_update(WU_BEG);
    wind_update(MC_BEG);

    if (mode == 1)
        ok = select_rect(vh, sw, sh, &rx, &ry, &rw, &rh);
    else if (mode == 2)
        ok = select_window(vh, sw, sh, &rx, &ry, &rw, &rh);

    if (ok) {                         /* reicht der Speicher? */
        need = mem_needed(rw, rh, planes);
        avail = mem_avail();
        if (avail >= 0 && avail < need) {
            lowmem = 1;
            ok = 0;
        }
    }

    if (ok) {
        graf_mouse(MOUSE_OFF, 0);     /* Mauszeiger nicht mitfotografieren */
        got = grab(vh, rx, ry, rw, rh, planes, &img);
        graf_mouse(MOUSE_ON, 0);
    }
    graf_mouse(CUR_ARROW, 0);

    wind_update(MC_END);
    wind_update(WU_END);

    if (mode && !ok && !lowmem)       /* Abbruch: Bildschirm neu zeichnen lassen */
        form_dial(3, 0, 0, 0, 0, 0, 0, sw, sh);   /* 3 = FMD_FINISH */

    v_clsvwk(vh);

    if (lowmem)
        alert_lowmem(need, avail);
    else if (ok && !got)
        form_alert(1, "[3][Zu wenig Speicher!][Abbruch]");
    if (got && img.tc && g_cal_failed)
        form_alert(1, "[1][Farbformat nicht erkannt.|Standardannahme wird benutzt,|die Farben k\224nnen abweichen.][ OK ]");
    if (got) {
        save_image(&img, (mode == 1) ? "AREA" : (mode == 2) ? "WIN" : "SHOT");
        putmem(img.base);
        img.base = img.data = 0;
    }
}

/* ---- "Ueber"-Dialog: baut sich im Code zusammen, keine RSC-Datei ----- */
static void setobj(OBJECT *o, short type, short flags,
                   short x, short y, short w, short h)
{
    o->ob_next = o->ob_head = o->ob_tail = -1;
    o->ob_type = type;
    o->ob_flags = flags;
    o->ob_state = 0;
    o->ob_x = x;  o->ob_y = y;
    o->ob_width = w;  o->ob_height = h;
}

static void do_about(void)
{
    enum { W = 38 };                         /* Breite in Zeichen (passt in 40 Spalten) */
    static OBJECT dlg[32];
    static const struct { char kind; const char *txt; } L[] = {
        { 'T', "X-Shot " XSHOT_VERSION " - Screenshot-Tool" },     /* fett */
        { 'H', "-- Beschreibung --" },
        { ' ', "Speichert Bildschirmfotos in jeder" },
        { ' ', "Aufl\224sung und Farbtiefe als" },
        { ' ', "PNG- oder TIFF-Datei." },
        { 'H', "-- Anleitung --" },
        { ' ', "Ganzer Bildschirm (Alt+G): alles" },
        { ' ', "Bereich (Alt+B): Rahmen aufziehen," },
        { ' ', "  rechte Maustaste = Abbruch." },
        { ' ', "Fenster (Alt+F): Fenster anklicken." },
        { ' ', "Speichern: Endung .PNG oder .TIF" },
        { ' ', "  im Namen w\204hlt das Format." },
        { 'C', "https://theragnarbay.org/xshot/" },
        { 0, 0 }
    };
    short dummy, cw, ch, phys, ext[57];
    short i, n, y, x, len, nh = 0, btn_h, fx, fy, fw, fh, bold2 = -1;
    int   col16, gap;

    phys = graf_handle(&cw, &ch, &dummy, &dummy);
    vq_extnd(phys, 1, ext);
    col16 = (ext[4] >= 4);
    gap   = (ch >= 16);                      /* hohe Aufloesung: Leerzeilen erlaubt */

    n = 1;
    y = 1;
    for (i = 0; L[i].txt; i++) {
        len = (short)strlen(L[i].txt);
        x = 2;
        if (L[i].kind == 'H') {
            nh++;
            if (gap && nh >= 2) y++;         /* Luft vor Abschnitt 2 und 3 */
        }
        if (L[i].kind == 'C' && gap) y++;    /* Luft vor der Webadresse */
        if (L[i].kind != ' ')
            x = (W - len) / 2;               /* Titel, Ueberschriften, Adresse zentrieren */
        setobj(&dlg[n], T_STRING, 0, x, y, len, 1);
        *(const char **)&dlg[n].ob_spec = L[i].txt;
        n++;
        if (L[i].kind == 'T') {              /* "fett": Text nochmal, 1 Pixel versetzt */
            setobj(&dlg[n], T_STRING, 0, x, y, len, 1);
            *(const char **)&dlg[n].ob_spec = L[i].txt;
            bold2 = n;
            n++;
        }
        y++;
    }
    if (gap) y++;
    btn_h = gap ? 1 : 2;
    setobj(&dlg[n], T_BUTTON,
           OF_SELECTABLE | OF_DEFAULT | OF_EXIT | (col16 ? OF_3D_ACTIVATOR : 0),
           (W - 10) / 2, y, 10, btn_h);
    *(char **)&dlg[n].ob_spec = "OK";
    dlg[n].ob_flags |= OF_LASTOB;
    n++;
    y += btn_h + 1;

    setobj(&dlg[0], T_BOX, col16 ? OF_3D_BACKGROUND : 0, 0, 0, W, y);
    *(long *)&dlg[0].ob_spec = col16 ? 0x00021178L : 0x00021170L;  /* Rahmen 2 */
    dlg[0].ob_head = 1;
    dlg[0].ob_tail = n - 1;
    for (i = 1; i < n; i++)
        dlg[i].ob_next = (i == n - 1) ? 0 : i + 1;
    for (i = 0; i < n; i++)
        rsrc_obfix(dlg, i);
    if (bold2 > 0)
        dlg[bold2].ob_x += 1;                /* Pixelversatz fuer den Fettdruck */

    graf_mouse(CUR_ARROW, 0);
    wind_update(WU_BEG);
    wind_update(MC_BEG);
    form_center(dlg, &fx, &fy, &fw, &fh);
    form_dial(0, 0, 0, 0, 0, fx, fy, fw, fh);          /* 0 = FMD_START  */
    objc_draw(dlg, 0, 8, fx, fy, fw, fh);
    form_do(dlg, 0);
    dlg[n - 1].ob_state &= ~OS_SELECTED;
    form_dial(3, 0, 0, 0, 0, fx, fy, fw, fh);          /* 3 = FMD_FINISH */
    wind_update(MC_END);
    wind_update(WU_END);
}

static void run_action(short obj)
{
    switch (obj) {
    case 1:
    case 2:
    case 3:
        do_capture(obj - 1);
        break;
    case 4:
        do_about();
        break;
    }
}

static void handle_click(short mx, short my)
{
    short obj = objc_find(tree, 0, 8, mx, my);
    short x, y, b, k;

    if (obj < 1 || obj > 4) return;

    tree[obj].ob_state |= OS_SELECTED;
    redraw_obj(obj);
    evnt_button(1, 1, 0, &x, &y, &b, &k);          /* auf Loslassen warten */
    tree[obj].ob_state &= ~OS_SELECTED;
    redraw_obj(obj);

    if (objc_find(tree, 0, 8, x, y) != obj) return; /* losgelassen ausserhalb */
    run_action(obj);
}

/* ================================================================== */
/*  main: Event-Schleife                                                */
/* ================================================================== */
int main(void)
{
    short msg[8], mx, my, mb, ks, kc, br, ev, top, d1, d2, d3;

    ap_id = appl_init();
    if (ap_id < 0) return 1;

    menu_id = menu_register(ap_id, MENU_TEXT);

    for (;;) {                         /* ein Accessory laeuft endlos */
        ev = evnt_multi(EV_MESAG | EV_BUTTON | EV_KEYBD, 1, 1, 1,
                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                        msg, 0UL, &mx, &my, &mb, &ks, &kc, &br);

        if (ev & EV_MESAG) {
            switch (msg[0]) {
            case MSG_AC_OPEN:
                if (msg[4] == menu_id) {
                    if (win < 0) open_window();
                    else wind_set(win, WIN_TOP, 0, 0, 0, 0);
                }
                break;
            case MSG_AC_CLOSE:
                if (msg[3] == menu_id)
                    win = -1;           /* AES hat das Fenster schon entfernt */
                break;
            case MSG_REDRAW:
                if (msg[3] == win)
                    redraw(msg[4], msg[5], msg[6], msg[7]);
                break;
            case MSG_TOPPED:
            case MSG_NEWTOP:
                if (msg[3] == win)
                    wind_set(win, WIN_TOP, 0, 0, 0, 0);
                break;
            case MSG_MOVED:
                if (msg[3] == win) {
                    wind_set(win, WIN_CURR, msg[4], msg[5], msg[6], msg[7]);
                    place_tree();
                }
                break;
            case MSG_CLOSED:
                if (msg[3] == win)
                    close_window();
                break;
            }
        }

        if ((ev & EV_KEYBD) && win >= 0 && (ks & 0x08)) {     /* Alt gedrueckt */
            wind_get(0, WIN_TOP, &top, &d1, &d2, &d3);
            if (top == win) {
                switch ((kc >> 8) & 0xff) {            /* Scancode der Taste */
                case 0x22: run_action(1); break;       /* Alt+G */
                case 0x30: run_action(2); break;       /* Alt+B */
                case 0x21: run_action(3); break;       /* Alt+F */
                case 0x16: run_action(4); break;       /* Alt+U */
                }
            }
        }

        if ((ev & EV_BUTTON) && win >= 0) {
            wind_get(0, WIN_TOP, &top, &d1, &d2, &d3);
            if (wind_find(mx, my) == win && top == win)
                handle_click(mx, my);
        }
    }
}