/*
 * indi_mini.c — mini servidor INDI (protocolo XML, puerto 7624) para IMX662 +
 * Hi3516CV610. Sin libindi: habla el dialecto INDI justo para ser usable desde
 * Ekos/KStars como CCD de astrofotografía.
 *
 * Reutiliza el path RAW verificado de astro_streamer.c (VI ISP-bypass, control
 * directo del sensor por I2C, VMAX extendido para exposiciones largas).
 *
 * Compilación (desde utils/):
 *   ./build_indi_mini.sh
 *
 * Uso (device, tras reboot limpio y módulos con imx662):
 *   /tmp/indi_mini --preset validated --port 7624
 *
 * Cliente (PC): Ekos → INDI → host 192.168.1.16 puerto 7624, driver IMX662 CCD.
 *   o: indi_getprop -h 192.168.1.16 -p 7624
 *
 * Propiedades expuestas (device "IMX662 CCD"):
 *   DRIVER_INFO, CONNECTION, CCD_INFO, CCD_EXPOSURE, CCD_ABORT_EXPOSURE,
 *   CCD_FRAME_TYPE, CCD_FRAME, CCD_BINNING, CCD_GAIN (dB 0..54),
 *   CCD1 (BLOB FITS base64). CCD_TEMPERATURE solo si hay NTC en el módulo
 *   (LSADC CH1, ver adc_temp_init; el IMX662 no tiene termómetro).
 *
 * Notas de diseño (RAM 64MB Linux):
 *  - 1 pool VB común (RAW, MMZ), sin ISP/VPSS/VENC → sin deps pesadas.
 *  - El FITS (2880 + W*H*2 = 4150080 B a full frame) se emite en streaming
 *    base64 por chunks: sin buffer de 5.5MB en RAM.
 *  - 1 thread por cliente (stack 64KB), máx 4 clientes. 1 exposición a la vez.
 *  - Decode raw12 LSB-first (decisión 0003): v0=((b1&0xF)<<4)|(b0>>4), v1=b2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/* ---------------------------------------------------------- constantes -- */

#define DEVICE_NAME      "IMX662 CCD"
#define INDI_VERSION     "1.7"
#define DRIVER_VERSION   "0.1"
#define MAX_CLIENTS      4
#define RX_BUF_SZ        16384
#define CHUNK_OUT        4096

#define SENSOR_W         1920
#define SENSOR_H         1080
#define PIXEL_UM         2.9
#define EXP_MAX_S        25.0
#define FITS_HDR_SZ      2880

/* Registros IMX662 (parity astro_streamer.c) */
#define REG_EXP_LSB      0x3050
#define REG_EXP_MID      0x3051
#define REG_EXP_MSB      0x3052
#define REG_GAIN_LSB     0x3070
#define REG_GAIN_MSB     0x3071
#define REG_GROUP_HOLD   0x3001
/* LSADC (termistor del módulo en CH1). ioctl API = adc.h del kernel. */
#define LSADC_DEV        "/dev/ot_lsadc"
#define LSADC_IOC_MODEL_SEL  _IOWR('A', 0, int)
#define LSADC_IOC_CHN_ENABLE _IOW('A', 1, int)
#define LSADC_IOC_CHN_DISABLE _IOW('A', 2, int)
#define LSADC_IOC_START      _IO('A', 3)
#define LSADC_IOC_STOP       _IO('A', 4)
#define LSADC_IOC_GET_CHNVAL _IOWR('A', 5, int)
#define TEMP_ADC_CHN     1
#define VMAX_DEFAULT     1250
#define EXP_MIN_LINES    11
#define EXP_MAX_LINES    0xFFFF7

static unsigned us_to_lines(unsigned long long us) { return (unsigned)((us * 3) / 80); }
static unsigned long long lines_to_us(unsigned lines) { return ((unsigned long long)lines * 80) / 3; }

/* ============================================ helpers puros (testeables) == */

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

typedef struct { uint8_t carry[3]; int ncarry; } b64_state_t;

/* Codifica hasta len bytes; devuelve nº de chars escritos (múltiplo de 4). */
static int b64_push(b64_state_t *s, const uint8_t *data, int len, char *out)
{
    int oi = 0, i = 0;
    uint8_t buf[3];
    /* completar carry pendiente */
    if (s->ncarry > 0) {
        while (s->ncarry < 3 && i < len)
            s->carry[s->ncarry++] = data[i++];
        if (s->ncarry == 3) {
            out[oi++] = b64tab[s->carry[0] >> 2];
            out[oi++] = b64tab[((s->carry[0] & 3) << 4) | (s->carry[1] >> 4)];
            out[oi++] = b64tab[((s->carry[1] & 15) << 2) | (s->carry[2] >> 6)];
            out[oi++] = b64tab[s->carry[2] & 63];
            s->ncarry = 0;
        }
    }
    while (i + 3 <= len) {
        buf[0] = data[i]; buf[1] = data[i+1]; buf[2] = data[i+2]; i += 3;
        out[oi++] = b64tab[buf[0] >> 2];
        out[oi++] = b64tab[((buf[0] & 3) << 4) | (buf[1] >> 4)];
        out[oi++] = b64tab[((buf[1] & 15) << 2) | (buf[2] >> 6)];
        out[oi++] = b64tab[buf[2] & 63];
    }
    while (i < len)
        s->carry[s->ncarry++] = data[i++];
    return oi;
}

static int b64_finish(b64_state_t *s, char *out)
{
    int oi = 0;
    if (s->ncarry == 1) {
        out[oi++] = b64tab[s->carry[0] >> 2];
        out[oi++] = b64tab[(s->carry[0] & 3) << 4];
        out[oi++] = '=';
        out[oi++] = '=';
    } else if (s->ncarry == 2) {
        out[oi++] = b64tab[s->carry[0] >> 2];
        out[oi++] = b64tab[((s->carry[0] & 3) << 4) | (s->carry[1] >> 4)];
        out[oi++] = b64tab[(s->carry[1] & 15) << 2];
        out[oi++] = '=';
    }
    s->ncarry = 0;
    return oi;
}

/* GAIN dB (0..54) → regs again/dgain 22.10. IMX662: GAIN[10:0] = dB*10/3. */
static void gain_db_to_regs(double db, unsigned *again, unsigned *dgain)
{
    double total;
    double ar, dr;
    if (db < 0) db = 0;
    if (db > 54) db = 54;
    total = pow(10.0, db / 20.0);
    ar = total < 32.0 ? total : 32.0;
    *again = (unsigned)(ar * 1024.0 + 0.5);
    if (*again < 1024) *again = 1024;
    if (*again > 32768) *again = 32768;
    dr = total / (*again / 1024.0);
    *dgain = (unsigned)(dr * 1024.0 + 0.5);
    if (*dgain < 1024) *dgain = 1024;
    if (*dgain > 16384) *dgain = 16384;
}

static void fits_card(uint8_t *hdr, int *pos, const char *key, const char *val)
{
    snprintf((char *)hdr + *pos, 81, "%-8s= %s", key, val);
    *pos = ((*pos / 80) + 1) * 80;
}

/* Header FITS 2880 B, NAXIS1=w NAXIS2=h. frame_type sin comillas. */
static void build_fits_header(uint8_t *hdr, int w, int h, double exptime_s,
                              double gain_lin, const char *frame_type,
                              double temp_c, const char *date_obs, int bin)
{
    /* temp_c < -100 = sin termómetro: se omite la tarjeta CCD-TEMP */
    char card[81];
    int pos = 0;
    memset(hdr, ' ', FITS_HDR_SZ);
    fits_card(hdr, &pos, "SIMPLE", "T");
    fits_card(hdr, &pos, "BITPIX", "16");
    fits_card(hdr, &pos, "NAXIS", "2");
    snprintf(card, 81, "%d", w); fits_card(hdr, &pos, "NAXIS1", card);
    snprintf(card, 81, "%d", h); fits_card(hdr, &pos, "NAXIS2", card);
    fits_card(hdr, &pos, "BZERO", "0.0");
    fits_card(hdr, &pos, "BSCALE", "1.0");
    snprintf(card, 81, "%.6f", exptime_s); fits_card(hdr, &pos, "EXPTIME", card);
    snprintf(card, 81, "%.4f", gain_lin); fits_card(hdr, &pos, "GAIN", card);
    snprintf(card, 81, "'%s'", frame_type); fits_card(hdr, &pos, "FRAME", card);
    fits_card(hdr, &pos, "BAYERPAT", "'RGGB'");
    fits_card(hdr, &pos, "INSTRUME", "'IMX662 Hi3516CV610'");
    if (temp_c > -100.0) {
        snprintf(card, 81, "%.2f", temp_c);
        fits_card(hdr, &pos, "CCD-TEMP", card);
    }
    snprintf(card, 81, "'%s'", date_obs); fits_card(hdr, &pos, "DATE-OBS", card);
    snprintf(card, 81, "%d", bin); fits_card(hdr, &pos, "XBINNING", card);
    fits_card(hdr, &pos, "YBINNING", card);
    /* Tamaño de píxel efectivo (escala con binning) para plate-solving.
       XBAYROFF/YBAYROFF=0: los offsets de subframe se exigen pares, así que
       la fase RGGB se preserva. */
    snprintf(card, 81, "%.2f", PIXEL_UM * bin); fits_card(hdr, &pos, "XPIXSZ", card);
    fits_card(hdr, &pos, "YPIXSZ", card);
    fits_card(hdr, &pos, "XBAYROFF", "0");
    fits_card(hdr, &pos, "YBAYROFF", "0");
    memset(hdr + pos, ' ', 80);
    memcpy(hdr + pos, "END", 3);
    /* snprintf deja NULs dentro del bloque: FITS exige espacios */
    for (int i = 0; i < FITS_HDR_SZ; i++)
        if (hdr[i] == 0) hdr[i] = ' ';
}

/* Extrae attr="valor" (o attr='valor': Ekos manda comillas simples).
   Devuelve 1 si ok. */
static int xml_get_attr(const char *msg, const char *attr, char *out, size_t outsz)
{
    char pat[64];
    const char *p, *q;
    char quote;
    size_t n;
    snprintf(pat, sizeof(pat), "%s=", attr);
    p = strstr(msg, pat);
    if (!p) return 0;
    p += strlen(pat);
    if (*p != '"' && *p != '\'') return 0;
    quote = *p++;
    q = strchr(p, quote);
    if (!q) return 0;
    n = (size_t)(q - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

/* Extrae <elem ... name="elemname">valor</elem> (comillas ' o ";
   recorta espacios del valor: Ekos manda "<oneSwitch ...>      On  </oneSwitch>").
   Devuelve 1 si ok. */
static int xml_get_elem(const char *msg, const char *elem, const char *elemname,
                        char *out, size_t outsz)
{
    char open[128];
    const char *p, *vs, *ve;
    size_t n;
    snprintf(open, sizeof(open), "<%s", elem);
    p = msg;
    while ((p = strstr(p, open)) != NULL) {
        char nm[64];
        const char *gt;
        /* verificar name="elemname" dentro de este tag de apertura */
        gt = strchr(p, '>');
        if (!gt) return 0;
        {
            char tag[512];
            size_t tl = (size_t)(gt - p);
            if (tl >= sizeof(tag)) tl = sizeof(tag) - 1;
            memcpy(tag, p, tl);
            tag[tl] = 0;
            if (!xml_get_attr(tag, "name", nm, sizeof(nm))) { p = gt + 1; continue; }
            if (strcmp(nm, elemname) != 0) { p = gt + 1; continue; }
        }
        vs = gt + 1;
        {
            char close[64];
            snprintf(close, sizeof(close), "</%s>", elem);
            ve = strstr(vs, close);
            if (!ve) return 0;
        }
        n = (size_t)(ve - vs);
        while (n > 0 && (*vs == ' ' || *vs == '\t' || *vs == '\r' || *vs == '\n')) {
            vs++; n--;
        }
        while (n > 0 && (vs[n-1] == ' ' || vs[n-1] == '\t' || vs[n-1] == '\r' ||
                vs[n-1] == '\n')) {
            n--;
        }
        if (n >= outsz) n = outsz - 1;
        memcpy(out, vs, n);
        out[n] = 0;
        return 1;
    }
    return 0;
}

/*
 * Delimita el primer mensaje XML completo en buf[0..len).
 * Devuelve: 1 completo (*msglen = largo), 0 incompleto, -1 basura (avanzar 1).
 * Salta <?...?>, <!--...-->, y texto fuera de tags.
 */
static int xml_split_msg(const char *buf, int len, int *msglen)
{
    const char *p = buf;
    int i, avail = len;
    while (avail > 0 && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++; avail--;
    }
    if (avail <= 0) return 0;
    if (p[0] != '<') return -1;
    if (avail >= 2 && p[1] == '?') {
        const char *e = strstr(p, "?>");
        if (!e) return 0;
        *msglen = (int)(e - buf) + 2;
        return 1;
    }
    if (avail >= 4 && !strncmp(p, "<!--", 4)) {
        const char *e = strstr(p, "-->");
        if (!e) return 0;
        *msglen = (int)(e - buf) + 3;
        return 1;
    }
    /* nombre del tag */
    i = 1;
    if (i < avail && p[i] == '/') return -1; /* cierre huérfano: descartar línea */
    while (i < avail && p[i] != '>' && p[i] != ' ' && p[i] != '\t' &&
           p[i] != '\r' && p[i] != '\n' && p[i] != '/')
        i++;
    if (i >= avail) return 0;
    {
        char tag[48];
        int tl = i - 1;
        char close[56];
        const char *e;
        if (tl <= 0 || tl >= (int)sizeof(tag)) return -1;
        memcpy(tag, p + 1, (size_t)tl);
        tag[tl] = 0;
        /* buscar fin del tag de apertura */
        e = memchr(p, '>', (size_t)avail);
        if (!e) return 0;
        if (e > p && *(e - 1) == '/') { /* autocerrado <.../> */
            *msglen = (int)(e - buf) + 1;
            return 1;
        }
        snprintf(close, sizeof(close), "</%s>", tag);
        e = strstr(e, close);
        if (!e) {
            /* mensaje truncado o tag desconocido sin cierre: si el buffer está
               lleno, descartar hasta '>' para no bloquearse */
            if (len >= RX_BUF_SZ - 1) {
                const char *g = strchr(p, '>');
                *msglen = g ? (int)(g - buf) + 1 : len;
                return 1;
            }
            return 0;
        }
        *msglen = (int)(e - buf) + (int)strlen(close);
        return 1;
    }
}

/* Decodifica el píxel 8-bit en la columna sx de una fila raw12 LSB-first.
   Equivalente al loop original (x par=p0 del triplet, x impar=p1=b2).
   Va acá arriba para que el selftest lo alcance. */
static unsigned pix8(const uint8_t *r, int sx)
{
    unsigned base = (unsigned)(sx >> 1) * 3;
    if (sx & 1)
        return (unsigned)r[base + 2];
    return (unsigned)(((r[base + 1] & 0x0F) << 4) | (r[base] >> 4));
}

#ifdef INDI_SELFTEST
/* Harness de host: gcc -DINDI_SELFTEST -o /tmp/indi_test utils/indi_mini.c -lm */
static int failures = 0;
#define TCHECK(c, ...) do { \
        if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); \
                    printf("\n"); failures++; } \
    } while (0)

int main(void)
{
    /* base64: "Man" -> "TWFu", streaming en partes */
    {
        b64_state_t s;
        char out[16];
        int n;
        memset(&s, 0, sizeof(s));
        n = b64_push(&s, (uint8_t *)"Ma", 2, out);
        TCHECK(n == 0, "push parcial debe dar 0, dio %d", n);
        n = b64_push(&s, (uint8_t *)"n", 1, out);
        TCHECK(n == 4 && !memcmp(out, "TWFu", 4), "Man->TWFu");
        n = b64_finish(&s, out);
        TCHECK(n == 0, "sin resto, finish=0");
        memset(&s, 0, sizeof(s));
        n = b64_push(&s, (uint8_t *)"M", 1, out);
        TCHECK(n == 0, "1 byte no emite");
        n = b64_finish(&s, out);
        TCHECK(n == 4 && !memcmp(out, "TQ==", 4), "M->TQ==");
    }
    /* gain map */
    {
        unsigned a, d;
        gain_db_to_regs(0, &a, &d);
        TCHECK(a == 1024 && d == 1024, "0dB -> 1024/1024 (%u/%u)", a, d);
        gain_db_to_regs(6.02, &a, &d);
        TCHECK(a >= 2000 && a <= 2100 && d >= 1000 && d <= 1050,
               "6dB -> ~2048/1024 (%u/%u)", a, d);
        gain_db_to_regs(100, &a, &d);
        TCHECK(a == 32768 && d >= 16000 && d <= 16100,
               "clamp 54dB (%u/%u)", a, d);
        gain_db_to_regs(-5, &a, &d);
        TCHECK(a == 1024 && d == 1024, "clamp min (%u/%u)", a, d);
    }
    /* fits header */
    {
        uint8_t hdr[FITS_HDR_SZ];
        char t[FITS_HDR_SZ + 1];
        build_fits_header(hdr, 1920, 1080, 5.0, 2.0, "LIGHT", 34.5,
                          "2026-01-01T00:00:00", 1);
        memcpy(t, hdr, FITS_HDR_SZ);
        t[FITS_HDR_SZ] = 0;
        TCHECK(!memcmp(t, "SIMPLE  = T", 11), "card SIMPLE");
        TCHECK(strstr(t, "NAXIS1  = 1920") != NULL, "NAXIS1");
        TCHECK(strstr(t, "EXPTIME = 5.000000") != NULL, "EXPTIME");
        TCHECK(strstr(t, "XBINNING= 1") != NULL, "XBINNING");
        TCHECK(strstr(t, "XPIXSZ  = 2.90") != NULL, "XPIXSZ (%s)", strstr(t, "XPIXSZ"));
        TCHECK(strstr(t, "XBAYROFF= 0") != NULL, "XBAYROFF");
        TCHECK(strstr(t, "BAYERPAT= 'RGGB'") != NULL, "BAYERPAT");
        TCHECK(strstr(t, "END") != NULL, "END");
        TCHECK(!memchr(hdr, 0, FITS_HDR_SZ), "sin NULs en header");
    }
    /* pix8: triplet LSB-first [b0,b1,b2] -> p0=(b1&F)<<4|(b0>>4), p1=b2 */
    {
        uint8_t r[6] = { 0xA0, 0x0B, 0xC5, 0x30, 0x0D, 0xE2 };
        TCHECK(pix8(r, 0) == 0xBA && pix8(r, 1) == 0xC5 &&
               pix8(r, 2) == 0xD3 && pix8(r, 3) == 0xE2,
               "pix8 %02x %02x %02x %02x",
               pix8(r, 0), pix8(r, 1), pix8(r, 2), pix8(r, 3));
    }
    /* xml attrs/elems */
    {
        const char *m = "<newNumberVector device=\"IMX662 CCD\" "
                        "name=\"CCD_EXPOSURE\"><oneNumber name=\"CCD_EXPOSURE_VALUE\">"
                        "5.0</oneNumber></newNumberVector>";
        char v[64], dv[64], nm[64];
        TCHECK(xml_get_attr(m, "device", dv, sizeof(dv)) &&
               !strcmp(dv, "IMX662 CCD"), "attr device");
        TCHECK(xml_get_attr(m, "name", nm, sizeof(nm)) &&
               !strcmp(nm, "CCD_EXPOSURE"), "attr name");
        TCHECK(xml_get_elem(m, "oneNumber", "CCD_EXPOSURE_VALUE", v, sizeof(v)) &&
               !strcmp(v, "5.0"), "elem value (%s)", v);
        TCHECK(!xml_get_elem(m, "oneNumber", "OTRO", v, sizeof(v)),
               "elem ausente");
    }
    /* formato Ekos: comillas simples + espacios en el valor */
    {
        const char *m = "<newSwitchVector device='IMX662 CCD' name='CONNECTION'>"
                        "  <oneSwitch name='CONNECT'>      On  </oneSwitch>"
                        "</newSwitchVector>";
        char v[64], dv[64], nm[64];
        TCHECK(xml_get_attr(m, "device", dv, sizeof(dv)) &&
               !strcmp(dv, "IMX662 CCD"), "attr device squote (%s)", dv);
        TCHECK(xml_get_attr(m, "name", nm, sizeof(nm)) &&
               !strcmp(nm, "CONNECTION"), "attr name squote (%s)", nm);
        TCHECK(xml_get_elem(m, "oneSwitch", "CONNECT", v, sizeof(v)) &&
               !strcmp(v, "On"), "elem trim (%s)", v);
    }
    /* split */
    {
        const char *m1 = "<enableBLOB device=\"d\" name=\"n\">Also</enableBLOB>";
        const char *m2 = "<getProperties version=\"1.7\"/>";
        char two[128];
        int ml, r;
        snprintf(two, sizeof(two), "%s\n%s", m1, m2);
        r = xml_split_msg(two, (int)strlen(two), &ml);
        TCHECK(r == 1 && ml == (int)strlen(m1), "split1 r=%d ml=%d", r, ml);
        r = xml_split_msg(two + ml, (int)strlen(two) - ml, &ml);
        TCHECK(r == 1 && ml == (int)strlen(m2) + 1,
               "split2 r=%d ml=%d", r, ml);
        r = xml_split_msg("<newNumber", 10, &ml);
        TCHECK(r == 0, "incompleto");
        r = xml_split_msg("basura<", 7, &ml);
        TCHECK(r == -1, "basura");
    }
    if (failures == 0) printf("SELFTEST OK\n");
    else printf("SELFTEST %d FAILURES\n", failures);
    return failures != 0;
}
#else /* ============================== hardware + servidor ============= */

#include "ot_type.h"
#include "ot_common.h"
#include "ot_common_video.h"
#include "ot_common_vb.h"
#include "ot_common_sys.h"
#include "ot_common_vi.h"
#include "ot_mipi_rx.h"
#include "ot_i2c.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_sys_mem.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vi.h"
#include "ss_mpi_sys_bind.h"
#include <sys/mman.h>
#include <sys/ioctl.h> /* _IO/_IOW/_IOWR para LSADC */
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define MIPI_DEV_NODE    "/dev/ot_mipi_rx"
#define I2C_DEV_NODE     "/dev/i2c-0"
#define I2C_DEV_ADDR     0x34
#define SNS0_CLK_HZ_PATH "/sys/module/open_sys_config/parameters/sns0_clk_hz"

#define VI_DEV           0
#define VI_PIPE          0
#define VI_CHN           0
#define REG_STANDBY      0x3000

/* OJO: XMSTA es 0x3002 (master start/stop). 0x3001 es REGHOLD, NO tocar acá. */
#define REG_XMSTA        0x3002
#define REG_INCK_SEL     0x3014
#define REG_DATARATE     0x3015
#define REG_VMAX_L       0x3028
#define REG_VMAX_M       0x3029
#define REG_VMAX_H       0x302A
#define REG_HMAX_L       0x302C
#define REG_HMAX_H       0x302D
#define REG_LANEMODE     0x3040

typedef struct {
    int lanes;
    int data_rate_x2;
    int raw_bit;
    uint32_t sensor_clock_hz;
    int incksel, datarate, lanemode;
    int port;
} MiniCfg;

static volatile sig_atomic_t g_stop;
static int g_i2c_fd = -1;

/* estado del CCD */
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_connected = 0;
static int g_exposing = 0;
static int g_abort = 0;
static double g_req_exp_s = 0;
static unsigned g_last_cap_vmax = 0xFFFFFFFF; /* fuerza flush en 1ra captura */

/* Workaround Ekos 3.8.0: no manda enableBLOB Also (queda en Never y encima
   pregunta "would you like to enable it?" sin efecto en el wire). Con
   --blob-force el driver envía el FITS a todo cliente activo aunque haya
   pedido Never. Si el cliente lo descarta, queda como antes (sin regresión);
   si lo acepta, la imagen llega. */
static int g_blob_force = 0;
static unsigned g_exp_lines = VMAX_DEFAULT - 8;
static unsigned g_vmax = VMAX_DEFAULT;
static unsigned g_again = 1024, g_dgain = 1024;
static double g_gain_db = 0.0;
static char g_frame_type[16] = "LIGHT";
static int g_sub_x = 0, g_sub_y = 0, g_sub_w = SENSOR_W, g_sub_h = SENSOR_H;
static int g_bin = 1; /* 1 o 2 (promedio 2x2 por soft, tarea binning) */
static int g_upload_mode = 0; /* 0=Client 1=Local 2=Both (switch estándar UPLOAD_MODE) */

/* Termistor NTC del módulo (LSADC CH1). Steinhart-Hart con divisor a VREF:
 * f = adc/1024 (10-bit), R = Rs*f/(1-f) (VREF se cancela: ratiométrico).
 * Defaults: NTC 100k β=3950 + serie 22k (f≈0.82 → ~25°C a 841 LSB).
 * Ajustar con --temp-r-series/--temp-r25/--temp-beta si difiere del ambiente.
 * Requiere open_adc.ko cargado (/dev/ot_lsadc); si no está, sin temperatura. */
static int g_adc_fd = -1;
static int g_have_temp = 0;
static double g_cached_temp_c = 25.0;
static double g_temp_rseries = 22000.0;
static double g_temp_r25 = 100000.0;
static double g_temp_beta = 3950.0;

/* clientes */
typedef struct {
    int fd;
    int blob_mode; /* 0=Never 1=Also 2=Only */
    int active;
    unsigned gen;  /* generación: valida jobs de exposición diferidos */
} client_t;
static client_t g_clients[MAX_CLIENTS];
static pthread_mutex_t g_cli_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_cli_gen;

#define CHECK(expr) do { \
        td_s32 r = (expr); \
        if (r != TD_SUCCESS) { \
            fprintf(stderr, "FAIL %s = 0x%x\n", #expr, r); \
            return -1; \
        } \
        printf("  ok  %s\n", #expr); \
    } while (0)

#define CHECK_OR_BUSY(expr) do { \
        td_s32 r = (expr); \
        if (r != TD_SUCCESS && (r & 0x1fff) != OT_ERR_BUSY) { \
            fprintf(stderr, "FAIL %s = 0x%x\n", #expr, r); \
            return -1; \
        } \
        printf("  ok  %s%s\n", #expr, (r == TD_SUCCESS) ? "" : "  (already up)"); \
    } while (0)

static void sig_handler(int sig)
{
    (void)sig;
    g_stop = 1;
    g_abort = 1; /* suelta al worker en grab_frame (slice<=2s) para que
                    libere su frame VB antes del teardown (si no, vb_exit
                    falla con busy y el pool viejo sobrevive al restart) */
}

/* ------------------------------------------------------------ I2C -- */

static int i2c_open(void)
{
    g_i2c_fd = open(I2C_DEV_NODE, O_RDWR);
    if (g_i2c_fd < 0) {
        fprintf(stderr, "FAIL open %s: %s\n", I2C_DEV_NODE, strerror(errno));
        return -1;
    }
    if (ioctl(g_i2c_fd, OT_I2C_SLAVE_FORCE, (I2C_DEV_ADDR >> 1)) < 0) {
        fprintf(stderr, "FAIL i2c slave force: %s\n", strerror(errno));
        close(g_i2c_fd);
        g_i2c_fd = -1;
        return -1;
    }
    return 0;
}

static int i2c_write_reg(td_u16 reg, td_u8 val)
{
    unsigned char buf[3];
    struct i2c_msg msg;
    struct i2c_rdwr_ioctl_data rdwr;
    buf[0] = (reg >> 8) & 0xFF;
    buf[1] = reg & 0xFF;
    buf[2] = val;
    msg.addr  = (I2C_DEV_ADDR >> 1);
    msg.flags = 0;
    msg.len   = 3;
    msg.buf   = buf;
    rdwr.msgs = &msg;
    rdwr.nmsgs = 1;
    if (ioctl(g_i2c_fd, I2C_RDWR, &rdwr) < 0)
        return -1;
    return 0;
}

static td_u8 i2c_read_reg(td_u16 reg)
{
    unsigned char rbuf[2] = { (reg >> 8) & 0xFF, reg & 0xFF };
    unsigned char val = 0;
    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data rdwr;
    msgs[0].addr  = (I2C_DEV_ADDR >> 1);
    msgs[0].flags = 0;
    msgs[0].len   = 2;
    msgs[0].buf   = rbuf;
    msgs[1].addr  = (I2C_DEV_ADDR >> 1);
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = 1;
    msgs[1].buf   = &val;
    rdwr.msgs = msgs;
    rdwr.nmsgs = 2;
    if (ioctl(g_i2c_fd, I2C_RDWR, &rdwr) < 0)
        return 0xFF;
    return val;
}

/* ------------------------------------------------- control sensor -- */

static void sensor_write_vmax(unsigned vmax)
{
    if (vmax < VMAX_DEFAULT) vmax = VMAX_DEFAULT;
    if (vmax > 0xFFFFF) vmax = 0xFFFFF;
    g_vmax = vmax;
    i2c_write_reg(REG_GROUP_HOLD, 0x01);
    i2c_write_reg(REG_VMAX_L, (td_u8)(vmax & 0xFF));
    i2c_write_reg(REG_VMAX_M, (td_u8)((vmax >> 8) & 0xFF));
    i2c_write_reg(REG_VMAX_H, (td_u8)((vmax >> 16) & 0x0F));
    i2c_write_reg(REG_GROUP_HOLD, 0x00);
}

static void sensor_write_exposure(unsigned exp_lines)
{
    unsigned shr;
    if (exp_lines > g_vmax - 4) exp_lines = g_vmax - 4;
    if (exp_lines > EXP_MAX_LINES) exp_lines = EXP_MAX_LINES;
    if (exp_lines < EXP_MIN_LINES) exp_lines = EXP_MIN_LINES;
    g_exp_lines = exp_lines;
    shr = (g_vmax > exp_lines) ? (g_vmax - exp_lines) : 4;
    if (shr < 11) shr = 11;
    if (shr > g_vmax - 4) shr = g_vmax - 4;
    i2c_write_reg(REG_GROUP_HOLD, 0x01);
    i2c_write_reg(REG_EXP_LSB, (td_u8)(shr & 0xFF));
    i2c_write_reg(REG_EXP_MID, (td_u8)((shr >> 8) & 0xFF));
    i2c_write_reg(REG_EXP_MSB, (td_u8)((shr >> 16) & 0x0F));
    i2c_write_reg(REG_GROUP_HOLD, 0x00);
}

static void sensor_set_gain(unsigned again, unsigned dgain)
{
    double total, db;
    unsigned reg;
    if (again < 1024) again = 1024;
    if (dgain < 1024) dgain = 1024;
    if (again > 32768) again = 32768;
    if (dgain > 16384) dgain = 16384;
    g_again = again;
    g_dgain = dgain;
    total = (again / 1024.0) * (dgain / 1024.0);
    db = 20.0 * log10(total);
    if (db < 0) db = 0;
    reg = (unsigned)(db * 10.0 / 3.0 + 0.5);
    if (reg > 0x3FF) reg = 0x3FF;
    i2c_write_reg(REG_GROUP_HOLD, 0x01);
    i2c_write_reg(REG_GAIN_LSB, (td_u8)(reg & 0xFF));
    i2c_write_reg(REG_GAIN_MSB, (td_u8)((reg >> 8) & 0x07));
    i2c_write_reg(REG_GROUP_HOLD, 0x00);
}

static void sensor_apply_gain_db(double db)
{
    unsigned a, d;
    gain_db_to_regs(db, &a, &d);
    g_gain_db = db;
    sensor_set_gain(a, d);
}

/* NOTA: el IMX662 NO tiene sensor de temperatura (verificado en register map
   + SRM + datasheet). No hay función de lectura térmica. */

/* Lee el NTC del módulo. Devuelve °C o -273 si falla. No bloqueante largo. */
static double adc_read_temp_c(void)
{
    int mode = 0, chn = TEMP_ADC_CHN, v, i;
    long sum = 0;
    double f, r, t;
    if (g_adc_fd < 0)
        return -273.0;
    if (ioctl(g_adc_fd, LSADC_IOC_MODEL_SEL, &mode) < 0)
        return -273.0;
    if (ioctl(g_adc_fd, LSADC_IOC_CHN_ENABLE, &chn) < 0)
        return -273.0;
    for (i = 0; i < 3; i++) {
        if (ioctl(g_adc_fd, LSADC_IOC_START) < 0)
            break;
        usleep(20000);
        v = ioctl(g_adc_fd, LSADC_IOC_GET_CHNVAL, &chn);
        ioctl(g_adc_fd, LSADC_IOC_STOP);
        if (v < 0)
            break;
        sum += v;
    }
    ioctl(g_adc_fd, LSADC_IOC_CHN_DISABLE, &chn);
    if (i == 0)
        return -273.0;
    f = (sum / (double)i) / 1024.0;
    if (f <= 0.001 || f >= 0.999)
        return -273.0;
    r = g_temp_rseries * f / (1.0 - f);
    t = 1.0 / (1.0 / 298.15 + log(r / g_temp_r25) / g_temp_beta) - 273.15;
    return t;
}

static void adc_temp_init(void)
{
    double t;
    g_adc_fd = open(LSADC_DEV, O_RDWR);
    if (g_adc_fd < 0) {
        fprintf(stderr, "  temp NTC: %s ausente (insmod open_adc.ko?) — sin CCD_TEMPERATURE\n",
                LSADC_DEV);
        return;
    }
    t = adc_read_temp_c();
    if (t < -100.0) {
        fprintf(stderr, "  temp NTC: lectura inválida — sin CCD_TEMPERATURE\n");
        close(g_adc_fd);
        g_adc_fd = -1;
        return;
    }
    g_have_temp = 1;
    g_cached_temp_c = t;
    printf("  temp NTC: %.1f°C (CH%d, Rs=%.0f R25=%.0f β=%.0f)\n",
           t, TEMP_ADC_CHN, g_temp_rseries, g_temp_r25, g_temp_beta);
}

/* ------------------------------------------- init sensor (tablas) -- */
/* Portadas de astro_streamer.c (init manual verificado en device). */

static const struct { td_u16 reg; td_u8 val; } imx662_init_common[] = {
    {0x3000, 0x01}, {0x3001, 0x00}, {0x3002, 0x00},
    {0x3014, 0x00},
    {0x301A, 0x00}, {0x301B, 0x00}, {0x301C, 0x00},
    {0x301E, 0x01}, {0x3020, 0x00}, {0x3021, 0x00}, {0x3026, 0x04},
    {0x3030, 0x00}, {0x3031, 0x00}, {0x3032, 0x00},
    {0x303C, 0x08}, {0x303D, 0x00},
    {0x303E, 0x80}, {0x303F, 0x07},
    {0x3044, 0x08}, {0x3045, 0x00},
    {0x3046, 0x38}, {0x3047, 0x04},
    {0x3054, 0x0E}, {0x3055, 0x00}, {0x3056, 0x00},
    {0x3058, 0x8A}, {0x3059, 0x01}, {0x305A, 0x00},
    {0x3060, 0x16}, {0x3061, 0x01}, {0x3062, 0x00},
    {0x3064, 0xC4}, {0x3065, 0x0C}, {0x3066, 0x00},
    {0x3069, 0x00}, {0x306B, 0x00},
    {0x3070, 0x00}, {0x3071, 0x00}, {0x3072, 0x00}, {0x3073, 0x00},
    {0x3074, 0x00}, {0x3075, 0x00},
    {0x3081, 0x00},
    {0x308C, 0x00}, {0x308D, 0x01},
    {0x3094, 0x00}, {0x3095, 0x00}, {0x3096, 0x00}, {0x3097, 0x00},
    {0x309C, 0x00}, {0x309D, 0x00},
    {0x30A4, 0xAA}, {0x30A6, 0x0F},
    {0x30CC, 0x00}, {0x30CD, 0x00},
    {0x30DC, 0x32}, {0x30DD, 0x40},
    {0x3400, 0x01}, {0x3444, 0xAC},
    {0x3460, 0x21}, {0x3492, 0x08},
    {0x3B00, 0x39}, {0x3B23, 0x2D}, {0x3B45, 0x04},
    {0x3C0A, 0x1F}, {0x3C0B, 0x1E}, {0x3C38, 0x21},
    {0x3C40, 0x06}, {0x3C44, 0x00},
    {0x3CB6, 0xD8}, {0x3CC4, 0xDA},
    {0x3E24, 0x79}, {0x3E2C, 0x15}, {0x3EDC, 0x2D},
    {0x4498, 0x05}, {0x4499, 0x06}, {0x449A, 0x00}, {0x449B, 0x10},
    {0x449C, 0x19}, {0x449D, 0x00}, {0x449E, 0x32}, {0x449F, 0x01},
    {0x44A0, 0x92}, {0x44A2, 0x91}, {0x44A4, 0x8C}, {0x44A6, 0x87},
    {0x44A8, 0x82}, {0x44AA, 0x78}, {0x44AC, 0x6E}, {0x44AE, 0x69},
    {0x44B0, 0x92}, {0x44B2, 0x91}, {0x44B4, 0x8C}, {0x44B6, 0x87},
    {0x44B8, 0x82}, {0x44BA, 0x78}, {0x44BC, 0x6E}, {0x44BE, 0x69},
    {0x44C1, 0x01}, {0x44C2, 0x7F}, {0x44C3, 0x01}, {0x44C4, 0x7A},
    {0x44C5, 0x01}, {0x44C6, 0x7A}, {0x44C7, 0x01}, {0x44C8, 0x70},
    {0x44C9, 0x01}, {0x44CA, 0x6B}, {0x44CB, 0x01}, {0x44CC, 0x6B},
    {0x44CD, 0x01}, {0x44CE, 0x5C}, {0x44CF, 0x01}, {0x44D0, 0x7F},
    {0x44D1, 0x01}, {0x44D2, 0x7F}, {0x44D3, 0x01}, {0x44D4, 0x7A},
    {0x44D5, 0x01}, {0x44D6, 0x7A}, {0x44D7, 0x01}, {0x44D8, 0x70},
    {0x44D9, 0x01}, {0x44DA, 0x6B}, {0x44DB, 0x01}, {0x44DC, 0x6B},
    {0x44DD, 0x01}, {0x44DE, 0x5C}, {0x44DF, 0x01},
    {0x4534, 0x1C}, {0x4535, 0x03},
    {0x4538, 0x1C}, {0x4539, 0x1C}, {0x453A, 0x1C}, {0x453B, 0x1C},
    {0x453C, 0x1C}, {0x453D, 0x1C}, {0x453E, 0x1C}, {0x453F, 0x1C},
    {0x4540, 0x1C}, {0x4541, 0x03}, {0x4542, 0x03}, {0x4543, 0x03},
    {0x4544, 0x03}, {0x4545, 0x03}, {0x4546, 0x03}, {0x4547, 0x03},
    {0x4548, 0x03}, {0x4549, 0x03},
};

static const struct { td_u16 reg; td_u8 val; } imx662_init_mode[] = {
    {0x3015, 0x00},
    {0x3018, 0x04},
    {0x3040, 0x00},
    {0x3022, 0x01},
    {0x3023, 0x01},
    {0x301B, 0x00},
    {0x3A50, 0xFF},
    {0x3A51, 0x03},
    {0x3A52, 0x00},
    {0x3028, 0xE2},
    {0x3029, 0x04},
    {0x302A, 0x00},
    {0x302C, 0xBC},
    {0x302D, 0x07},
};

static int init_sensor_full(const MiniCfg *c)
{
    unsigned int i;
    int ok = 0, fail = 0;

    for (i = 0; i < sizeof(imx662_init_common)/sizeof(imx662_init_common[0]); i++) {
        if (i2c_write_reg(imx662_init_common[i].reg, imx662_init_common[i].val) == 0)
            ok++;
        else
            fail++;
        usleep(100);
    }
    printf("  Phase 1: %d ok %d fail\n", ok, fail);

    ok = fail = 0;
    for (i = 0; i < sizeof(imx662_init_mode)/sizeof(imx662_init_mode[0]); i++) {
        if (i2c_write_reg(imx662_init_mode[i].reg, imx662_init_mode[i].val) == 0)
            ok++;
        else
            fail++;
        usleep(100);
    }
    printf("  Phase 2: %d ok %d fail\n", ok, fail);

    if (c->incksel >= 0) i2c_write_reg(REG_INCK_SEL, (td_u8)c->incksel);
    if (c->datarate >= 0) i2c_write_reg(REG_DATARATE, (td_u8)c->datarate);
    if (c->lanemode >= 0) i2c_write_reg(REG_LANEMODE, (td_u8)c->lanemode);

    i2c_write_reg(0x3458, 0x00);
    i2c_write_reg(REG_STANDBY, 0x00);
    i2c_write_reg(REG_XMSTA, 0x00);
    usleep(25000);

    sensor_write_vmax(VMAX_DEFAULT);
    sensor_write_exposure(us_to_lines(33333));
    sensor_set_gain(g_again, g_dgain);
    fprintf(stderr, "  [readback] 30DC=%02X 3000=%02X 3014=%02X 3015=%02X 3040=%02X\n",
            i2c_read_reg(0x30DC), i2c_read_reg(0x3000),
            i2c_read_reg(REG_INCK_SEL), i2c_read_reg(REG_DATARATE),
            i2c_read_reg(REG_LANEMODE));
    return 0;
}

/* ------------------------------------------------------------ MIPI -- */

static int sensor_clock_select(uint32_t hz)
{
    char buf[16];
    int fd, len;
    if (hz == 0)
        return 0;
    fd = open(SNS0_CLK_HZ_PATH, O_WRONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "WARNING: %s absent — MCLK queda como cargó el loader\n",
                    SNS0_CLK_HZ_PATH);
            return 0;
        }
        fprintf(stderr, "FAIL open %s: %s\n", SNS0_CLK_HZ_PATH, strerror(errno));
        return -1;
    }
    len = snprintf(buf, sizeof(buf), "%u", hz);
    if (len < 0 || (size_t)len >= sizeof(buf) || write(fd, buf, (size_t)len) != len) {
        fprintf(stderr, "FAIL set sensor clock %u Hz: %s\n", hz, strerror(errno));
        close(fd);
        return -1;
    }
    printf("  ok  sensor clock %u Hz\n", hz);
    close(fd);
    return 0;
}

static int mipi_setup(const MiniCfg *c)
{
    combo_dev_attr_t attr;
    combo_dev_t dev = 0;
    sns_clk_source_t clk = 0;
    sns_rst_source_t rst = 0;
    lane_divide_mode_t hs = LANE_DIVIDE_MODE_0;
    int fd, i;

    memset(&attr, 0, sizeof(attr));
    attr.devno = dev;
    attr.input_mode = INPUT_MODE_MIPI;
    attr.data_rate = c->data_rate_x2 ? MIPI_DATA_RATE_X2 : MIPI_DATA_RATE_X1;
    attr.img_rect.x = 0;
    attr.img_rect.y = 0;
    attr.img_rect.width = SENSOR_W;
    attr.img_rect.height = SENSOR_H;
    attr.mipi_attr.input_data_type =
        (c->raw_bit == 10) ? DATA_TYPE_RAW_10BIT : DATA_TYPE_RAW_12BIT;
    attr.mipi_attr.wdr_mode = OT_MIPI_WDR_MODE_NONE;
    for (i = 0; i < MIPI_LANE_NUM; i++)
        attr.mipi_attr.lane_id[i] = (i < c->lanes) ? (short)i : (short)-1;

    fd = open(MIPI_DEV_NODE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "FAIL open %s: %s\n", MIPI_DEV_NODE, strerror(errno));
        return -1;
    }
#define mipi_ioctl(req, arg) do { \
        if (ioctl(fd, (req), (arg)) < 0) { \
            fprintf(stderr, "FAIL mipi ioctl %s: %s\n", #req, strerror(errno)); \
            close(fd); \
            return -1; \
        } \
    } while (0)

    mipi_ioctl(OT_MIPI_SET_HS_MODE, &hs);
    mipi_ioctl(OT_MIPI_DISABLE_MIPI_CLOCK, &dev);
    mipi_ioctl(OT_MIPI_RESET_MIPI, &dev);
    mipi_ioctl(OT_MIPI_DISABLE_SENSOR_CLOCK, &clk);
    mipi_ioctl(OT_MIPI_RESET_SENSOR, &rst);

    mipi_ioctl(OT_MIPI_SET_DEV_ATTR, &attr);

    mipi_ioctl(OT_MIPI_ENABLE_MIPI_CLOCK, &dev);
    mipi_ioctl(OT_MIPI_UNRESET_MIPI, &dev);
    mipi_ioctl(OT_MIPI_ENABLE_SENSOR_CLOCK, &clk);
    if (sensor_clock_select(c->sensor_clock_hz) != 0) {
        close(fd);
        return -1;
    }
    mipi_ioctl(OT_MIPI_UNRESET_SENSOR, &rst);
    usleep(500000);
#undef mipi_ioctl
    close(fd);
    return 0;
}

/* Reset clock-ON (replica cmos_isp_init del lib): obligatorio para I2C. */
static int sensor_mclk_reset(void)
{
    int fd = open(MIPI_DEV_NODE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "FAIL open %s: %s\n", MIPI_DEV_NODE, strerror(errno));
        return -1;
    }
    {
        lane_divide_mode_t hs = LANE_DIVIDE_MODE_0;
        sns_clk_source_t clk = 0;
        sns_rst_source_t rst = 0;
        (void)ioctl(fd, OT_MIPI_SET_HS_MODE, &hs);
        (void)ioctl(fd, OT_MIPI_ENABLE_SENSOR_CLOCK, &clk);
        (void)ioctl(fd, OT_MIPI_RESET_SENSOR, &rst);
        usleep(10000);
        (void)ioctl(fd, OT_MIPI_UNRESET_SENSOR, &rst);
        usleep(100000);
    }
    close(fd);
    return 0;
}

/* ---------------------------------------------------------- VB/SYS -- */

static int sys_setup(void)
{
    ot_vb_cfg vb;
    ot_vi_vpss_mode vi_vpss_mode;
    td_u64 raw_blk;
    td_s32 sys_ret, vb_ret;
    unsigned int i;

    raw_blk = (td_u64)SENSOR_W * SENSOR_H * 2 + 0x4000;

    memset(&vb, 0, sizeof(vb));
    vb.max_pool_cnt = 1;
    vb.common_pool[0].blk_size = raw_blk;
    vb.common_pool[0].blk_cnt = 8; /* 8x4MB=33MB (MMZ 64MB): margen contra
                                      vb_fail (con 4 se descartaba 1/3).
                                      NOTA: vb_exit da NOT_PERM (0xa001800d) en
                                      restart, y set_cfg posterior es no-op:
                                      el resize SOLO aplica en el primer run
                                      tras el boot. Verificado: pool sigue en 4
                                      hasta el próximo reboot. */

    sys_ret = ss_mpi_sys_exit();
    vb_ret = ss_mpi_vb_exit();
    fprintf(stderr, "  pre-clean: sys_exit=0x%x vb_exit=0x%x\n", sys_ret, vb_ret);

    memset(&vi_vpss_mode, 0, sizeof(vi_vpss_mode));
    for (i = 0; i < OT_VI_MAX_PIPE_NUM; i++)
        vi_vpss_mode.mode[i] = OT_VI_OFFLINE_VPSS_OFFLINE;

    CHECK_OR_BUSY(ss_mpi_vb_set_cfg(&vb));
    CHECK_OR_BUSY(ss_mpi_vb_init());
    CHECK_OR_BUSY(ss_mpi_sys_init());
    {
        td_s32 r = ss_mpi_vb_init_mod_common_pool(OT_VB_UID_VI);
        fprintf(stderr, "  init_mod_common_pool(VI): 0x%x (no-fatal)\n", r);
    }
    printf("  ok  VI-offline, 1 pool RAW %llu B x8\n",
           (unsigned long long)raw_blk);
    return 0;
}

/* --------------------------------------------------------------- VI -- */

static int vi_setup_raw(const MiniCfg *c)
{
    ot_vi_dev_attr dev_attr;
    ot_vi_pipe_attr pipe_attr;
    ot_vi_chn_attr chn_attr;

    ss_mpi_vi_disable_chn(VI_PIPE, VI_CHN);
    ss_mpi_vi_stop_pipe(VI_PIPE);
    ss_mpi_vi_destroy_pipe(VI_PIPE);
    ss_mpi_vi_unbind(VI_DEV, VI_PIPE);
    ss_mpi_vi_disable_dev(VI_DEV);
    usleep(50000);

    memset(&dev_attr, 0, sizeof(dev_attr));
    dev_attr.intf_mode = OT_VI_INTF_MODE_MIPI;
    dev_attr.work_mode = OT_VI_WORK_MODE_MULTIPLEX_1;
    dev_attr.component_mask[0] = 0xFFF0000;
    dev_attr.scan_mode = OT_VI_SCAN_PROGRESSIVE;
    dev_attr.data_type = OT_VI_DATA_TYPE_RAW;
    dev_attr.in_size.width = SENSOR_W;
    dev_attr.in_size.height = SENSOR_H;
    dev_attr.data_rate = c->data_rate_x2 ? OT_DATA_RATE_X2 : OT_DATA_RATE_X1;
    CHECK(ss_mpi_vi_set_dev_attr(VI_DEV, &dev_attr));
    CHECK(ss_mpi_vi_enable_dev(VI_DEV));
    CHECK(ss_mpi_vi_bind(VI_DEV, VI_PIPE));

    memset(&pipe_attr, 0, sizeof(pipe_attr));
    pipe_attr.isp_bypass = TD_TRUE;
    pipe_attr.size.width = SENSOR_W;
    pipe_attr.size.height = SENSOR_H;
    pipe_attr.pixel_format = (c->raw_bit == 10) ?
        OT_PIXEL_FORMAT_RGB_BAYER_10BPP : OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
    pipe_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    pipe_attr.frame_rate_ctrl.src_frame_rate = OT_VI_INVALID_FRAME_RATE;
    pipe_attr.frame_rate_ctrl.dst_frame_rate = OT_VI_INVALID_FRAME_RATE;
    CHECK(ss_mpi_vi_create_pipe(VI_PIPE, &pipe_attr));
    {
        td_s32 r = ss_mpi_vb_init_mod_common_pool(OT_VB_UID_VI);
        fprintf(stderr, "  init_mod_common_pool(VI): 0x%x (no-fatal)\n", r);
    }
    CHECK(ss_mpi_vi_start_pipe(VI_PIPE));

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.size.width = SENSOR_W;
    chn_attr.size.height = SENSOR_H;
    chn_attr.pixel_format = (c->raw_bit == 10) ?
        OT_PIXEL_FORMAT_RGB_BAYER_10BPP : OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
    chn_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    chn_attr.video_format = OT_VIDEO_FORMAT_LINEAR;
    chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    chn_attr.depth = 1;
    chn_attr.frame_rate_ctrl.src_frame_rate = OT_VI_INVALID_FRAME_RATE;
    chn_attr.frame_rate_ctrl.dst_frame_rate = OT_VI_INVALID_FRAME_RATE;
    CHECK(ss_mpi_vi_set_chn_attr(VI_PIPE, VI_CHN, &chn_attr));
    CHECK(ss_mpi_vi_enable_chn(VI_PIPE, VI_CHN));
    printf("  ok  VI RAW (isp_bypass, bayer %dbpp)\n", c->raw_bit);
    return 0;
}

static void mpp_cleanup(void)
{
    td_s32 sys_ret, vb_ret;
    printf("== teardown ==\n");
    ss_mpi_vi_disable_chn(VI_PIPE, VI_CHN);
    ss_mpi_vi_stop_pipe(VI_PIPE);
    ss_mpi_vi_destroy_pipe(VI_PIPE);
    ss_mpi_vi_unbind(VI_DEV, VI_PIPE);
    ss_mpi_vi_disable_dev(VI_DEV);
    if (g_i2c_fd >= 0) {
        close(g_i2c_fd);
        g_i2c_fd = -1;
    }
    sys_ret = ss_mpi_sys_exit();
    vb_ret = ss_mpi_vb_exit();
    fprintf(stderr, "  teardown: sys_exit=0x%x vb_exit=0x%x\n", sys_ret, vb_ret);
}

/* ------------------------------------------------------ socket out -- */

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int cprintf(int fd, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if (n >= (int)sizeof(tmp))
        n = (int)sizeof(tmp) - 1;
    return write_all(fd, tmp, (size_t)n);
}

static void iso_now(char *out, size_t n)
{
    struct timeval tv;
    struct tm tmv;
    gettimeofday(&tv, NULL);
    gmtime_r(&tv.tv_sec, &tmv);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* ------------------------------------------------------ INDI defs -- */

static void send_all_defs(int fd)
{
    char ts[32];
    iso_now(ts, sizeof(ts));
    cprintf(fd, "<defTextVector device=\"" DEVICE_NAME "\" name=\"DRIVER_INFO\" "
            "label=\"Driver\" group=\"General\" state=\"Ok\" perm=\"ro\" "
            "timestamp=\"%s\">"
            "<defText name=\"DRIVER_NAME\" label=\"Name\">IMX662 CCD</defText>"
            "<defText name=\"DRIVER_EXEC\" label=\"Exec\">indi_mini</defText>"
            "<defText name=\"DRIVER_VERSION\" label=\"Version\">" DRIVER_VERSION "</defText>"
            "<defText name=\"DRIVER_INTERFACE\" label=\"Interface\">2</defText>"
            "</defTextVector>\n", ts);
    cprintf(fd, "<defSwitchVector device=\"" DEVICE_NAME "\" name=\"CONNECTION\" "
            "label=\"Connection\" group=\"Main\" state=\"%s\" perm=\"rw\" "
            "rule=\"OneOfMany\" timestamp=\"%s\">"
            "<defSwitch name=\"CONNECT\" label=\"Connect\">%s</defSwitch>"
            "<defSwitch name=\"DISCONNECT\" label=\"Disconnect\">%s</defSwitch>"
            "</defSwitchVector>\n",
            g_connected ? "Ok" : "Idle", ts,
            g_connected ? "On" : "Off", g_connected ? "Off" : "On");
    /* Switch estándar INDI::CCD: Ekos lo lee (getUploadMode) y avisa si falta.
       Es informativo: el envío de BLOBs lo sigue mandando el enableBLOB. */
    {
        int um;
        pthread_mutex_lock(&g_dev_lock);
        um = g_upload_mode;
        pthread_mutex_unlock(&g_dev_lock);
        cprintf(fd, "<defSwitchVector device=\"" DEVICE_NAME "\" name=\"UPLOAD_MODE\" "
                "label=\"Upload\" group=\"Main\" state=\"Ok\" perm=\"rw\" "
                "rule=\"OneOfMany\" timestamp=\"%s\">"
                "<defSwitch name=\"UPLOAD_CLIENT\" label=\"Client\">%s</defSwitch>"
                "<defSwitch name=\"UPLOAD_LOCAL\" label=\"Local\">%s</defSwitch>"
                "<defSwitch name=\"UPLOAD_BOTH\" label=\"Both\">%s</defSwitch>"
                "</defSwitchVector>\n", ts,
                um == 0 ? "On" : "Off", um == 1 ? "On" : "Off",
                um == 2 ? "On" : "Off");
    }
    cprintf(fd, "<defNumberVector device=\"" DEVICE_NAME "\" name=\"CCD_INFO\" "
            "label=\"CCD\" group=\"Main\" state=\"Ok\" perm=\"ro\" timestamp=\"%s\">"
            "<defNumber name=\"CCD_MAX_X\" label=\"Max X\" min=\"0\" max=\"1920\" step=\"1\">1920</defNumber>"
            "<defNumber name=\"CCD_MAX_Y\" label=\"Max Y\" min=\"0\" max=\"1080\" step=\"1\">1080</defNumber>"
            "<defNumber name=\"CCD_PIXEL_SIZE\" label=\"Pixel (um)\" min=\"0\" max=\"99\" step=\"0.01\">%.1f</defNumber>"
            "<defNumber name=\"CCD_PIXEL_SIZE_X\" label=\"Pixel X (um)\" min=\"0\" max=\"99\" step=\"0.01\">%.1f</defNumber>"
            "<defNumber name=\"CCD_PIXEL_SIZE_Y\" label=\"Pixel Y (um)\" min=\"0\" max=\"99\" step=\"0.01\">%.1f</defNumber>"
            "<defNumber name=\"CCD_BITSPERPIXEL\" label=\"Bits\" min=\"0\" max=\"32\" step=\"1\">16</defNumber>"
            "</defNumberVector>\n", ts, PIXEL_UM, PIXEL_UM, PIXEL_UM);
    cprintf(fd, "<defNumberVector device=\"" DEVICE_NAME "\" name=\"CCD_EXPOSURE\" "
            "label=\"Expose\" group=\"Main\" state=\"%s\" perm=\"rw\" timeout=\"60\" timestamp=\"%s\">"
            "<defNumber name=\"CCD_EXPOSURE_VALUE\" label=\"Duration (s)\" "
            "min=\"0.001\" max=\"%.1f\" step=\"0.01\">%.3f</defNumber>"
            "</defNumberVector>\n",
            g_exposing ? "Busy" : "Ok", ts, EXP_MAX_S, g_req_exp_s);
    cprintf(fd, "<defSwitchVector device=\"" DEVICE_NAME "\" name=\"CCD_ABORT_EXPOSURE\" "
            "label=\"Abort\" group=\"Main\" state=\"Ok\" perm=\"wo\" rule=\"OneOfMany\" timestamp=\"%s\">"
            "<defSwitch name=\"ABORT\" label=\"Abort\">Off</defSwitch>"
            "</defSwitchVector>\n", ts);
    cprintf(fd, "<defSwitchVector device=\"" DEVICE_NAME "\" name=\"CCD_FRAME_TYPE\" "
            "label=\"Frame type\" group=\"Image\" state=\"Ok\" perm=\"rw\" rule=\"OneOfMany\" timestamp=\"%s\">"
            "<defSwitch name=\"FRAME_LIGHT\" label=\"Light\">%s</defSwitch>"
            "<defSwitch name=\"FRAME_BIAS\" label=\"Bias\">%s</defSwitch>"
            "<defSwitch name=\"FRAME_DARK\" label=\"Dark\">%s</defSwitch>"
            "<defSwitch name=\"FRAME_FLAT\" label=\"Flat\">%s</defSwitch>"
            "</defSwitchVector>\n", ts,
            !strcmp(g_frame_type, "LIGHT") ? "On" : "Off",
            !strcmp(g_frame_type, "BIAS") ? "On" : "Off",
            !strcmp(g_frame_type, "DARK") ? "On" : "Off",
            !strcmp(g_frame_type, "FLAT") ? "On" : "Off");
    cprintf(fd, "<defNumberVector device=\"" DEVICE_NAME "\" name=\"CCD_FRAME\" "
            "label=\"Frame\" group=\"Image\" state=\"Ok\" perm=\"rw\" timestamp=\"%s\">"
            "<defNumber name=\"X\" label=\"Left\" min=\"0\" max=\"1918\" step=\"2\">%d</defNumber>"
            "<defNumber name=\"Y\" label=\"Top\" min=\"0\" max=\"1078\" step=\"2\">%d</defNumber>"
            "<defNumber name=\"WIDTH\" label=\"Width\" min=\"2\" max=\"1920\" step=\"2\">%d</defNumber>"
            "<defNumber name=\"HEIGHT\" label=\"Height\" min=\"2\" max=\"1080\" step=\"2\">%d</defNumber>"
            "</defNumberVector>\n", ts, g_sub_x, g_sub_y, g_sub_w, g_sub_h);
    cprintf(fd, "<defNumberVector device=\"" DEVICE_NAME "\" name=\"CCD_BINNING\" "
            "label=\"Binning\" group=\"Image\" state=\"Ok\" perm=\"rw\" timestamp=\"%s\">"
            "<defNumber name=\"HOR_BIN\" label=\"X\" min=\"1\" max=\"2\" step=\"1\">%d</defNumber>"
            "<defNumber name=\"VER_BIN\" label=\"Y\" min=\"1\" max=\"2\" step=\"1\">%d</defNumber>"
            "</defNumberVector>\n", ts, g_bin, g_bin);
    cprintf(fd, "<defNumberVector device=\"" DEVICE_NAME "\" name=\"CCD_GAIN\" "
            "label=\"Gain\" group=\"Image\" state=\"Ok\" perm=\"rw\" timestamp=\"%s\">"
            "<defNumber name=\"GAIN\" label=\"Gain (dB)\" min=\"0\" max=\"54\" step=\"0.1\">%.1f</defNumber>"
            "</defNumberVector>\n", ts, g_gain_db);
    /* Sin CCD_TEMPERATURE si no hay NTC (IMX662 sin termómetro; ver arriba).
       Con NTC del módulo (LSADC CH1) se anuncia y actualiza por exposición. */
    if (g_have_temp)
        cprintf(fd, "<defNumberVector device=\"" DEVICE_NAME "\" name=\"CCD_TEMPERATURE\" "
                "label=\"Temp\" group=\"Main\" state=\"Ok\" perm=\"ro\" timestamp=\"%s\">"
                "<defNumber name=\"CCD_TEMPERATURE_VALUE\" label=\"C\" min=\"-40\" max=\"85\" step=\"0.1\">%.2f</defNumber>"
                "</defNumberVector>\n", ts, g_cached_temp_c);
    cprintf(fd, "<defBLOBVector device=\"" DEVICE_NAME "\" name=\"CCD1\" "
            "label=\"Image\" group=\"Image\" state=\"Idle\" perm=\"ro\" timestamp=\"%s\">"
            "<defBLOB name=\"CCD1\" label=\"Image\"/>"
            "</defBLOBVector>\n", ts);
}

static void send_msg(int fd, const char *text)
{
    char ts[32];
    iso_now(ts, sizeof(ts));
    cprintf(fd, "<message device=\"" DEVICE_NAME "\" timestamp=\"%s\" "
            "message=\"%s\"/>\n", ts, text);
}

/* ----------------------------------------------- captura + BLOB -- */

/* Espera un frame con timeout por slices (para poder abortar).
 * 0=ok (*fi válido), -1=timeout/stop, -2=abort. */
static int grab_frame(ot_video_frame_info *fi, unsigned long timeout_ms)
{
    unsigned long waited = 0;
    for (;;) {
        unsigned long slice = timeout_ms - waited > 2000 ?
                              2000 : timeout_ms - waited;
        td_s32 ret;
        memset(fi, 0, sizeof(*fi));
        ret = ss_mpi_vi_get_chn_frame(VI_PIPE, VI_CHN, fi, (td_u32)slice);
        if (ret == TD_SUCCESS)
            return 0;
        pthread_mutex_lock(&g_dev_lock);
        {
            int ab = g_abort;
            pthread_mutex_unlock(&g_dev_lock);
            if (ab)
                return -2;
        }
        waited += slice;
        if (waited >= timeout_ms || g_stop)
            return -1;
    }
}

/* Emite el frame actual como FITS base64 en streaming a los clientes con
 * blob_mode != Never. Devuelve 0 si la CAPTURA salió bien (haya o no
 * receptores BLOB: en modo Never el cliente igual da por completa la
 * exposición); -1/-2 solo si falló el grab o el mapeo. */
static int stream_fits_blob(double exptime_s)
{
    ot_video_frame_info frame_info;
    unsigned lines = us_to_lines((unsigned long long)(exptime_s * 1e6));
    unsigned long timeout_ms;
    int mem_fd = -1;
    int sent_ok = 0;

    if (lines < EXP_MIN_LINES) lines = EXP_MIN_LINES;
    if (lines > EXP_MAX_LINES) lines = EXP_MAX_LINES;
    if (lines + 8 > g_vmax)
        sensor_write_vmax(lines + 8);
    else if (g_vmax != VMAX_DEFAULT && lines + 8 <= VMAX_DEFAULT)
        sensor_write_vmax(VMAX_DEFAULT);
    sensor_write_exposure(lines);

    /* esperar frames frescos con el nuevo VMAX (periodo puede ser largo).
       Si el timing cambió desde la última captura, el pipe (depth=1) puede
       entregar un frame integrado con el timing ANTERIOR: flush corto (toma
       el frame ya encolado sin esperar un periodo entero) + 1 descarte de
       un periodo completo. Si el timing no cambió, captura directa. */
    timeout_ms = (unsigned long)(lines_to_us(g_vmax) / 1000) * 2 + 5000;
    {
        ot_video_frame_info tmp;
        if (g_vmax != g_last_cap_vmax) {
            int grc;
            if (grab_frame(&tmp, 500) == 0)
                ss_mpi_vi_release_chn_frame(VI_PIPE, VI_CHN, &tmp);
            grc = grab_frame(&tmp, timeout_ms);
            if (grc != 0) {
                fprintf(stderr, "[indi] descarte rc=%d\n", grc);
                return grc; /* -2 = abort: que el worker reporte "aborted" */
            }
            ss_mpi_vi_release_chn_frame(VI_PIPE, VI_CHN, &tmp);
        }
        {
            int grc = grab_frame(&frame_info, timeout_ms);
            if (grc != 0) {
                fprintf(stderr, "[indi] captura final rc=%d (exp %.2fs)\n",
                        grc, exptime_s);
                return grc; /* -2 = abort */
            }
        }
        g_last_cap_vmax = g_vmax;
    }

    mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (mem_fd < 0) {
        ss_mpi_vi_release_chn_frame(VI_PIPE, VI_CHN, &frame_info);
        return -1;
    }
    {
        const ot_video_frame *vf = &frame_info.video_frame;
        td_u32 stride = vf->stride[0];
        td_u64 phys = vf->phys_addr[0];
        td_u64 map_size = (td_u64)stride * vf->height;
        td_u64 page_phys = phys & ~0xFFFULL;
        td_u64 page_off = phys - page_phys;
        td_u64 map_len = ((map_size + page_off + 0xFFF) & ~0xFFFULL);
        uint8_t *src;
        void *map = mmap(NULL, (size_t)map_len, PROT_READ, MAP_SHARED, mem_fd,
                         (off_t)page_phys);
        uint8_t hdr[FITS_HDR_SZ];
        char date[32], ts[32];
        struct timeval tv;
        struct tm tmv;
        int bin = g_bin; /* snapshot: dims del header y del loop consistentes */
        int w = g_sub_w / bin, h = g_sub_h / bin;
        unsigned long blob_sz = FITS_HDR_SZ + (unsigned long)w * h * 2;
        double gain_lin = (g_again / 1024.0) * (g_dgain / 1024.0);
        int fds[MAX_CLIENTS], nfds = 0, i;

        if (map == MAP_FAILED) {
            close(mem_fd);
            ss_mpi_vi_release_chn_frame(VI_PIPE, VI_CHN, &frame_info);
            return -1;
        }
        src = (uint8_t *)map + page_off;

        gettimeofday(&tv, NULL);
        gmtime_r(&tv.tv_sec, &tmv);
        snprintf(date, sizeof(date), "%04d-%02d-%02dT%02d:%02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        build_fits_header(hdr, w, h, exptime_s, gain_lin, g_frame_type,
                          g_have_temp ? g_cached_temp_c : -1000.0, date, bin);
        iso_now(ts, sizeof(ts));

        pthread_mutex_lock(&g_cli_lock);
        for (i = 0; i < MAX_CLIENTS; i++)
            if (g_clients[i].active &&
                (g_blob_force || g_clients[i].blob_mode != 0))
                fds[nfds++] = g_clients[i].fd;
        pthread_mutex_unlock(&g_cli_lock);

        for (i = 0; i < nfds; i++) {
            b64_state_t st;
            /* peor caso: fila full 1920*2=3840B → 5120 chars base64 */
            char out[8192];
            int y, n;
            memset(&st, 0, sizeof(st));
            if (cprintf(fds[i],
                        "<setBLOBVector device=\"" DEVICE_NAME "\" name=\"CCD1\" "
                        "state=\"Ok\" timestamp=\"%s\" timeout=\"60\">"
                        "<oneBLOB name=\"CCD1\" size=\"%lu\" format=\".fits\" "
                        "encoding=\"base64\">", ts, blob_sz) != 0)
                continue;
            n = b64_push(&st, hdr, FITS_HDR_SZ, out);
            if (n > 0 && write_all(fds[i], out, (size_t)n) != 0)
                continue;
            {
                /* fila a fila: raw12 LSB-first → BE16, con crop por soft.
                   bin=1: píxeles directos. bin=2: promedio 2x2 (4x menos datos,
                   ideal para foco/encuadre; la fase Bayer se preserva porque
                   crop y bin son pares, pero el color promediado ya no es
                   RGGB puro: para fotometría usar bin 1x1). */
                uint8_t row[3840];
                int ok_row = 1;
                for (y = 0; y < h && ok_row; y++) {
                    int x;
                    if (bin == 1) {
                        const uint8_t *r = src + (unsigned long)(g_sub_y + y) * stride;
                        for (x = 0; x < w; x += 2) {
                            unsigned idx = (unsigned)((g_sub_x + x) * 3) / 2;
                            uint16_t p0 = (uint16_t)(((r[idx + 1] & 0x0F) << 4) |
                                                      (r[idx] >> 4));
                            uint16_t p1 = (uint16_t)r[idx + 2];
                            /* p0/p1 son 8-bit en contenedor 16-bit BE (FITS std) */
                            row[x * 2] = 0;
                            row[x * 2 + 1] = (uint8_t)p0;
                            row[x * 2 + 2] = 0;
                            row[x * 2 + 3] = (uint8_t)p1;
                        }
                    } else {
                        const uint8_t *r0 = src + (unsigned long)(g_sub_y + 2 * y) * stride;
                        const uint8_t *r1 = r0 + stride;
                        for (x = 0; x < w; x++) {
                            unsigned sx = (unsigned)(g_sub_x + 2 * x);
                            unsigned s = pix8(r0, (int)sx) + pix8(r0, (int)sx + 1) +
                                         pix8(r1, (int)sx) + pix8(r1, (int)sx + 1);
                            row[x * 2] = 0;
                            row[x * 2 + 1] = (uint8_t)((s + 2) / 4);
                        }
                    }
                    /* bin=1: x+=2 escribe 4 bytes por iteración: hasta w*2 bytes.
                       bin=2: 2 bytes por iteración: w*2 bytes. */
                    n = b64_push(&st, row, w * 2, out);
                    if (n > 0 && write_all(fds[i], out, (size_t)n) != 0)
                        ok_row = 0;
                }
                if (!ok_row)
                    continue;
            }
            n = b64_finish(&st, out);
            if (n > 0 && write_all(fds[i], out, (size_t)n) != 0)
                continue;
            if (cprintf(fds[i], "</oneBLOB></setBLOBVector>\n") == 0)
                sent_ok = 1;
        }
        munmap(map, (size_t)map_len);
    }
    close(mem_fd);
    ss_mpi_vi_release_chn_frame(VI_PIPE, VI_CHN, &frame_info);
    /* Captura OK aunque nadie haya pedido el BLOB (modo Never de Ekos):
       la exposición se completó; solo el envío se omite. */
    (void)sent_ok;
    return 0;
}

/* -------------------------------------------------- manejo cliente -- */

typedef struct { int slot; unsigned gen; double seconds; } exp_job_t;

/* Worker de exposición: corre en thread propio para que el thread del cliente
 * siga parseando (ABORT/otras props) mientras integra. */
static void *exposure_worker(void *arg)
{
    exp_job_t *job = arg;
    int fd = -1;
    int rc = stream_fits_blob(job->seconds);
    char ts[32];

    if (g_have_temp) {
        double t = adc_read_temp_c();
        if (t > -100.0)
            g_cached_temp_c = t;
    }
    pthread_mutex_lock(&g_dev_lock);
    g_exposing = 0;
    {
        int ab = g_abort;
        g_abort = 0;
        pthread_mutex_unlock(&g_dev_lock);
        pthread_mutex_lock(&g_cli_lock);
        if (job->slot >= 0 && job->slot < MAX_CLIENTS &&
            g_clients[job->slot].active &&
            g_clients[job->slot].gen == job->gen)
            fd = g_clients[job->slot].fd;
        pthread_mutex_unlock(&g_cli_lock);
        iso_now(ts, sizeof(ts));
        if (fd < 0) {
            fprintf(stderr, "[indi] worker: cliente ido, reply omitido\n");
        } else if (rc == -2 || ab) {
            cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                    "name=\"CCD_EXPOSURE\" state=\"Alert\" timestamp=\"%s\">"
                    "<oneNumber name=\"CCD_EXPOSURE_VALUE\">0</oneNumber>"
                    "</setNumberVector>\n", ts);
            send_msg(fd, "exposure aborted");
        } else if (rc == 0) {
            cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                    "name=\"CCD_EXPOSURE\" state=\"Ok\" timestamp=\"%s\">"
                    "<oneNumber name=\"CCD_EXPOSURE_VALUE\">0</oneNumber>"
                    "</setNumberVector>\n", ts);
            if (g_have_temp)
                cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                        "name=\"CCD_TEMPERATURE\" state=\"Ok\" timestamp=\"%s\">"
                        "<oneNumber name=\"CCD_TEMPERATURE_VALUE\">%.2f</oneNumber>"
                        "</setNumberVector>\n", ts, g_cached_temp_c);
        } else {
            cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                    "name=\"CCD_EXPOSURE\" state=\"Alert\" timestamp=\"%s\">"
                    "<oneNumber name=\"CCD_EXPOSURE_VALUE\">0</oneNumber>"
                    "</setNumberVector>\n", ts);
            send_msg(fd, "frame capture failed");
        }
    }
    free(job);
    return NULL;
}

static void handle_exposure(int fd, double seconds)
{
    char ts[32];
    int slot = -1;

    if (seconds < 0.001) seconds = 0.001;
    if (seconds > EXP_MAX_S) seconds = EXP_MAX_S;

    pthread_mutex_lock(&g_dev_lock);
    if (!g_connected) {
        pthread_mutex_unlock(&g_dev_lock);
        iso_now(ts, sizeof(ts));
        cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                "name=\"CCD_EXPOSURE\" state=\"Alert\" timestamp=\"%s\">"
                "<oneNumber name=\"CCD_EXPOSURE_VALUE\">%.3f</oneNumber>"
                "</setNumberVector>\n", ts, seconds);
        send_msg(fd, "not connected");
        return;
    }
    if (g_exposing) {
        double cur = g_req_exp_s;
        pthread_mutex_unlock(&g_dev_lock);
        /* Repetir Busy (no solo mensaje): si el cliente reintentó sin ver el
           primer Busy, igual queda sincronizado en vez de esperar a ciegas. */
        iso_now(ts, sizeof(ts));
        cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                "name=\"CCD_EXPOSURE\" state=\"Busy\" timestamp=\"%s\">"
                "<oneNumber name=\"CCD_EXPOSURE_VALUE\">%.3f</oneNumber>"
                "</setNumberVector>\n", ts, cur);
        send_msg(fd, "exposure already in progress");
        return;
    }
    g_exposing = 1;
    g_abort = 0;
    g_req_exp_s = seconds;
    pthread_mutex_unlock(&g_dev_lock);

    pthread_mutex_lock(&g_cli_lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].active && g_clients[i].fd == fd)
            slot = i;
    pthread_mutex_unlock(&g_cli_lock);

    iso_now(ts, sizeof(ts));
    cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
            "name=\"CCD_EXPOSURE\" state=\"Busy\" timestamp=\"%s\">"
            "<oneNumber name=\"CCD_EXPOSURE_VALUE\">%.3f</oneNumber>"
            "</setNumberVector>\n", ts, seconds);
    fprintf(stderr, "[indi] exposición %.3fs tipo=%s sub=%d,%d %dx%d bin=%d gain=%.1fdB\n",
            seconds, g_frame_type, g_sub_x, g_sub_y, g_sub_w, g_sub_h, g_bin, g_gain_db);
    {
        exp_job_t *job = malloc(sizeof(*job));
        pthread_t th;
        pthread_attr_t at;
        if (!job) {
            pthread_mutex_lock(&g_dev_lock);
            g_exposing = 0;
            pthread_mutex_unlock(&g_dev_lock);
            send_msg(fd, "no memory");
            return;
        }
        job->slot = slot;
        job->gen = (slot >= 0) ? g_clients[slot].gen : 0;
        job->seconds = seconds;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 65536);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, exposure_worker, job) != 0) {
            free(job);
            pthread_mutex_lock(&g_dev_lock);
            g_exposing = 0;
            pthread_mutex_unlock(&g_dev_lock);
            send_msg(fd, "worker spawn failed");
        }
        pthread_attr_destroy(&at);
    }
}

static void dispatch_msg(int fd, const char *msg)
{
    char dev[64], name[64], val[256], ts[32];
    char tag[32];
    int ti;
    for (ti = 0; ti < 31 && msg[ti+1] && msg[ti+1] != '>' && msg[ti+1] != ' ' &&
         msg[ti+1] != '\t' && msg[ti+1] != '\r' && msg[ti+1] != '\n' &&
         msg[ti+1] != '/'; ti++)
        tag[ti] = msg[ti+1];
    tag[ti] = 0;
    {
        /* dump inbound (acotado): imprescindible para diagnosticar clientes */
        char dbg[256];
        int di = 0;
        for (const char *q = msg; *q && di < (int)sizeof(dbg) - 1; q++) {
            if (*q == '\r' || *q == '\n')
                continue;
            dbg[di++] = *q;
        }
        dbg[di] = 0;
        fprintf(stderr, "[indi] RX fd=%d: %s\n", fd, dbg);
    }

    if (!strncmp(msg, "<!--", 4) || msg[1] == '?')
        return;
    if (!strncmp(msg, "<getProperties", 14)) {
        if (xml_get_attr(msg, "device", dev, sizeof(dev)) &&
            strcmp(dev, DEVICE_NAME) != 0)
            return; /* es para otro driver */
        pthread_mutex_lock(&g_dev_lock);
        pthread_mutex_unlock(&g_dev_lock);
        send_all_defs(fd);
        return;
    }
    if (!strncmp(msg, "<enableBLOB", 11)) {
        if (xml_get_attr(msg, "device", dev, sizeof(dev)) &&
            strcmp(dev, DEVICE_NAME) != 0)
            return;
        /* contenido: Never/Also/Only (puede venir con espacios) */
        {
            const char *gt = strchr(msg, '>');
            const char *lt;
            int mode = 1;
            if (gt) {
                lt = strstr(gt, "</");
                if (lt) {
                    if (strstr(gt, "Never") && strstr(gt, "Never") < lt) mode = 0;
                    else if (strstr(gt, "Only") && strstr(gt, "Only") < lt) mode = 2;
                    else mode = 1;
                }
            }
            pthread_mutex_lock(&g_cli_lock);
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (g_clients[i].active && g_clients[i].fd == fd)
                    g_clients[i].blob_mode = mode;
            pthread_mutex_unlock(&g_cli_lock);
            fprintf(stderr, "[indi] fd=%d blob_mode=%d\n", fd, mode);
        }
        return;
    }
    if (!strncmp(msg, "<newSwitchVector", 16)) {
        if (!xml_get_attr(msg, "device", dev, sizeof(dev)) ||
            strcmp(dev, DEVICE_NAME) != 0)
            return;
        if (!xml_get_attr(msg, "name", name, sizeof(name)))
            return;
        iso_now(ts, sizeof(ts));
        if (!strcmp(name, "CONNECTION")) {
            int want_on = 0, want_off = 0;
            if (xml_get_elem(msg, "oneSwitch", "CONNECT", val, sizeof(val)))
                want_on = !strcmp(val, "On");
            if (xml_get_elem(msg, "oneSwitch", "DISCONNECT", val, sizeof(val)))
                want_off = !strcmp(val, "On");
            pthread_mutex_lock(&g_dev_lock);
            if (want_on) g_connected = 1;
            if (want_off) g_connected = 0;
            {
                int c = g_connected;
                pthread_mutex_unlock(&g_dev_lock);
                cprintf(fd, "<setSwitchVector device=\"" DEVICE_NAME "\" "
                        "name=\"CONNECTION\" state=\"%s\" timestamp=\"%s\">"
                        "<oneSwitch name=\"CONNECT\">%s</oneSwitch>"
                        "<oneSwitch name=\"DISCONNECT\">%s</oneSwitch>"
                        "</setSwitchVector>\n", c ? "Ok" : "Idle", ts,
                        c ? "On" : "Off", c ? "Off" : "On");
            }
            return;
        }
        if (!strcmp(name, "UPLOAD_MODE")) {
            static const char *keys[3] = { "UPLOAD_CLIENT", "UPLOAD_LOCAL",
                                           "UPLOAD_BOTH" };
            int i, um = -1;
            for (i = 0; i < 3; i++)
                if (xml_get_elem(msg, "oneSwitch", keys[i], val, sizeof(val)) &&
                    !strcmp(val, "On"))
                    um = i;
            pthread_mutex_lock(&g_dev_lock);
            if (um >= 0) g_upload_mode = um;
            um = g_upload_mode;
            pthread_mutex_unlock(&g_dev_lock);
            cprintf(fd, "<setSwitchVector device=\"" DEVICE_NAME "\" "
                    "name=\"UPLOAD_MODE\" state=\"Ok\" timestamp=\"%s\">"
                    "<oneSwitch name=\"UPLOAD_CLIENT\">%s</oneSwitch>"
                    "<oneSwitch name=\"UPLOAD_LOCAL\">%s</oneSwitch>"
                    "<oneSwitch name=\"UPLOAD_BOTH\">%s</oneSwitch>"
                    "</setSwitchVector>\n", ts,
                    um == 0 ? "On" : "Off", um == 1 ? "On" : "Off",
                    um == 2 ? "On" : "Off");
            return;
        }
        if (!strcmp(name, "CCD_ABORT_EXPOSURE")) {
            if (xml_get_elem(msg, "oneSwitch", "ABORT", val, sizeof(val)) &&
                !strcmp(val, "On")) {
                pthread_mutex_lock(&g_dev_lock);
                g_abort = 1;
                pthread_mutex_unlock(&g_dev_lock);
                cprintf(fd, "<setSwitchVector device=\"" DEVICE_NAME "\" "
                        "name=\"CCD_ABORT_EXPOSURE\" state=\"Ok\" timestamp=\"%s\">"
                        "<oneSwitch name=\"ABORT\">Off</oneSwitch>"
                        "</setSwitchVector>\n", ts);
            }
            return;
        }
        if (!strcmp(name, "CCD_FRAME_TYPE")) {
            const char *types[4] = { "LIGHT", "BIAS", "DARK", "FLAT" };
            const char *keys[4] = { "FRAME_LIGHT", "FRAME_BIAS",
                                    "FRAME_DARK", "FRAME_FLAT" };
            int i;
            pthread_mutex_lock(&g_dev_lock);
            for (i = 0; i < 4; i++)
                if (xml_get_elem(msg, "oneSwitch", keys[i], val, sizeof(val)) &&
                    !strcmp(val, "On"))
                    snprintf(g_frame_type, sizeof(g_frame_type), "%s", types[i]);
            pthread_mutex_unlock(&g_dev_lock);
            send_all_defs(fd);
            return;
        }
        return;
    }
    if (!strncmp(msg, "<newNumberVector", 16)) {
        if (!xml_get_attr(msg, "device", dev, sizeof(dev)) ||
            strcmp(dev, DEVICE_NAME) != 0)
            return;
        if (!xml_get_attr(msg, "name", name, sizeof(name)))
            return;
        if (!strcmp(name, "CCD_EXPOSURE")) {
            if (xml_get_elem(msg, "oneNumber", "CCD_EXPOSURE_VALUE",
                             val, sizeof(val)))
                handle_exposure(fd, atof(val));
            return;
        }
        if (!strcmp(name, "CCD_GAIN")) {
            if (xml_get_elem(msg, "oneNumber", "GAIN", val, sizeof(val))) {
                double db = atof(val);
                pthread_mutex_lock(&g_dev_lock);
                sensor_apply_gain_db(db);
                pthread_mutex_unlock(&g_dev_lock);
                iso_now(ts, sizeof(ts));
                cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                        "name=\"CCD_GAIN\" state=\"Ok\" timestamp=\"%s\">"
                        "<oneNumber name=\"GAIN\">%.1f</oneNumber>"
                        "</setNumberVector>\n", ts, g_gain_db);
            }
            return;
        }
        if (!strcmp(name, "CCD_FRAME")) {
            int x = g_sub_x, y = g_sub_y, w = g_sub_w, h = g_sub_h;
            if (xml_get_elem(msg, "oneNumber", "X", val, sizeof(val)))
                x = atoi(val);
            if (xml_get_elem(msg, "oneNumber", "Y", val, sizeof(val)))
                y = atoi(val);
            if (xml_get_elem(msg, "oneNumber", "WIDTH", val, sizeof(val)))
                w = atoi(val);
            if (xml_get_elem(msg, "oneNumber", "HEIGHT", val, sizeof(val)))
                h = atoi(val);
            iso_now(ts, sizeof(ts));
            if (x < 0 || y < 0 || w < 2 || h < 2 || x + w > SENSOR_W ||
                y + h > SENSOR_H || (x & 1) || (y & 1) || (w & 1) || (h & 1)) {
                cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                        "name=\"CCD_FRAME\" state=\"Alert\" timestamp=\"%s\">"
                        "<oneNumber name=\"X\">%d</oneNumber>"
                        "<oneNumber name=\"Y\">%d</oneNumber>"
                        "<oneNumber name=\"WIDTH\">%d</oneNumber>"
                        "<oneNumber name=\"HEIGHT\">%d</oneNumber>"
                        "</setNumberVector>\n", ts, g_sub_x, g_sub_y,
                        g_sub_w, g_sub_h);
                send_msg(fd, "subframe invalido (par, dentro de 1920x1080)");
            } else {
                pthread_mutex_lock(&g_dev_lock);
                g_sub_x = x; g_sub_y = y; g_sub_w = w; g_sub_h = h;
                pthread_mutex_unlock(&g_dev_lock);
                cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                        "name=\"CCD_FRAME\" state=\"Ok\" timestamp=\"%s\">"
                        "<oneNumber name=\"X\">%d</oneNumber>"
                        "<oneNumber name=\"Y\">%d</oneNumber>"
                        "<oneNumber name=\"WIDTH\">%d</oneNumber>"
                        "<oneNumber name=\"HEIGHT\">%d</oneNumber>"
                        "</setNumberVector>\n", ts, x, y, w, h);
            }
            return;
        }
        if (!strcmp(name, "CCD_BINNING")) {
            int hb = 1, vb = 1, nb = -1, cur;
            if (xml_get_elem(msg, "oneNumber", "HOR_BIN", val, sizeof(val)))
                hb = atoi(val);
            if (xml_get_elem(msg, "oneNumber", "VER_BIN", val, sizeof(val)))
                vb = atoi(val);
            if (hb == vb && (hb == 1 || hb == 2))
                nb = hb;
            pthread_mutex_lock(&g_dev_lock);
            if (nb > 0) g_bin = nb;
            cur = g_bin;
            pthread_mutex_unlock(&g_dev_lock);
            iso_now(ts, sizeof(ts));
            if (nb < 0) {
                cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                        "name=\"CCD_BINNING\" state=\"Alert\" timestamp=\"%s\">"
                        "<oneNumber name=\"HOR_BIN\">%d</oneNumber>"
                        "<oneNumber name=\"VER_BIN\">%d</oneNumber>"
                        "</setNumberVector>\n", ts, cur, cur);
                send_msg(fd, "solo binning 1x1 o 2x2 (cuadrado)");
            } else {
                cprintf(fd, "<setNumberVector device=\"" DEVICE_NAME "\" "
                        "name=\"CCD_BINNING\" state=\"Ok\" timestamp=\"%s\">"
                        "<oneNumber name=\"HOR_BIN\">%d</oneNumber>"
                        "<oneNumber name=\"VER_BIN\">%d</oneNumber>"
                        "</setNumberVector>\n", ts, cur, cur);
            }
            return;
        }
        return;
    }
}

static void *client_thread(void *arg)
{
    int fd = *(int *)arg;
    char rx[RX_BUF_SZ];
    int used = 0;
    free(arg);

    fprintf(stderr, "[indi] cliente fd=%d\n", fd);
    while (!g_stop) {
        ssize_t n = read(fd, rx + used, sizeof(rx) - 1 - used);
        if (n == 0) {
            fprintf(stderr, "[indi] fd=%d read EOF\n", fd);
            break;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "[indi] fd=%d read err %d\n", fd, errno);
            break;
        }
        used += (int)n;
        /* extraer mensajes completos */
        for (;;) {
            int ml = 0, r;
            if (used <= 0)
                break;
            r = xml_split_msg(rx, used, &ml);
            if (r == 0)
                break; /* incompleto: leer más */
            if (r < 0) {
                memmove(rx, rx + 1, (size_t)--used);
                continue;
            }
            {
                char msg[RX_BUF_SZ];
                int skip = 0, mlen;
                if (ml >= (int)sizeof(msg))
                    ml = (int)sizeof(msg) - 1;
                /* xml_split_msg incluye el whitespace previo en ml:
                   el dispatch espera que msg empiece en '<' */
                while (skip < ml && (rx[skip] == ' ' || rx[skip] == '\t' ||
                       rx[skip] == '\r' || rx[skip] == '\n'))
                    skip++;
                mlen = ml - skip;
                memcpy(msg, rx + skip, (size_t)mlen);
                msg[mlen] = 0;
                memmove(rx, rx + ml, (size_t)(used - ml));
                used -= ml;
                dispatch_msg(fd, msg);
            }
            if (g_stop)
                break;
        }
        if (used >= (int)sizeof(rx) - 1)
            used = 0; /* resync ante basura */
    }
    pthread_mutex_lock(&g_cli_lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].active && g_clients[i].fd == fd)
            g_clients[i].active = 0;
    pthread_mutex_unlock(&g_cli_lock);
    close(fd);
    fprintf(stderr, "[indi] cliente fd=%d desconectado\n", fd);
    return NULL;
}

static int tcp_listen(int port)
{
    int fd, one = 1;
    struct sockaddr_in addr;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ----------------------------------------------------------------- -- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [flags]\n"
        "  --preset validated|waybeam  base sensor (default validated)\n"
        "  --lanes N                   1..4 (default preset)\n"
        "  --mclk-hz HZ                MCLK (default preset, 0=no tocar)\n"
        "  --incksel N --datarate N --lanemode N  overrides (-1=tabla)\n"
        "  --gain-db DB                ganancia inicial dB (default 0)\n"
        "  --temp-r-series OHM         NTC: R serie (default 22000)\n"
        "  --temp-r25 OHM              NTC: R a 25°C (default 100000)\n"
        "  --temp-beta B               NTC: beta (default 3950)\n"
        "  --port P                    puerto INDI (default 7624)\n"
        "  --logfile PATH              log stderr a archivo (para daemon)\n"
        "  --blob-force                enviar BLOB aunque el cliente pida Never\n",
        prog);
}

int main(int argc, char **argv)
{
    MiniCfg cfg;
    int i, sock = -1;
    const char *logfile = NULL;

    memset(&cfg, 0, sizeof(cfg));
    cfg.lanes = 1;
    cfg.data_rate_x2 = 0;
    cfg.raw_bit = 12;
    cfg.sensor_clock_hz = 27000000;
    cfg.incksel = 0x03;
    cfg.datarate = 0x05;
    cfg.lanemode = 0x00;
    cfg.port = 7624;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--preset")) {
            if (++i >= argc) { usage(argv[0]); return 1; }
            if (!strcmp(argv[i], "validated")) {
                cfg.lanes = 1; cfg.sensor_clock_hz = 27000000;
                cfg.incksel = 0x03; cfg.datarate = 0x05; cfg.lanemode = 0x00;
            } else if (!strcmp(argv[i], "waybeam")) {
                cfg.lanes = 4; cfg.sensor_clock_hz = 37125000;
                cfg.incksel = 0x01; cfg.datarate = 0x03; cfg.lanemode = 0x03;
            } else { usage(argv[0]); return 1; }
        } else if (!strcmp(argv[i], "--lanes")) {
            cfg.lanes = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--mclk-hz")) {
            cfg.sensor_clock_hz = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--incksel")) {
            cfg.incksel = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--datarate")) {
            cfg.datarate = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--lanemode")) {
            cfg.lanemode = (int)strtol(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--gain-db")) {
            g_gain_db = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--temp-r-series")) {
            g_temp_rseries = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--temp-r25")) {
            g_temp_r25 = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--temp-beta")) {
            g_temp_beta = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--port")) {
            cfg.port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--logfile")) {
            if (++i >= argc) { usage(argv[0]); return 1; }
            logfile = argv[i];
        } else if (!strcmp(argv[i], "--blob-force")) {
            g_blob_force = 1;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (cfg.lanes < 1 || cfg.lanes > 4) {
        fprintf(stderr, "lanes debe ser 1..4\n");
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    if (logfile) {
        /* start-stop-daemon -b cierra stdio al daemonizar: reabrir stderr
           para que el log de diagnóstico ([indi] RX/...) llegue a disco */
        if (freopen(logfile, "a", stderr))
            setvbuf(stderr, NULL, _IONBF, 0);
    }
    memset(g_clients, 0, sizeof(g_clients));

    printf("== indi_mini " DRIVER_VERSION " (" DEVICE_NAME ") ==\n");
    printf("  preset: %d lanes MCLK=%u INCK=0x%02X DR=0x%02X LANE=0x%02X port=%d\n",
           cfg.lanes, cfg.sensor_clock_hz, cfg.incksel, cfg.datarate,
           cfg.lanemode, cfg.port);

    if (sys_setup() != 0) { mpp_cleanup(); return 1; }
    if (mipi_setup(&cfg) != 0) { mpp_cleanup(); return 1; }
    if (vi_setup_raw(&cfg) != 0) { mpp_cleanup(); return 1; }
    if (i2c_open() != 0) { mpp_cleanup(); return 1; }
    sensor_mclk_reset();
    if (init_sensor_full(&cfg) != 0) { mpp_cleanup(); return 1; }
    sensor_apply_gain_db(g_gain_db);
    adc_temp_init(); /* no fatal: sin NTC no hay CCD_TEMPERATURE */
    printf("  ok  pipeline RAW + INDI en puerto %d\n", cfg.port);

    sock = tcp_listen(cfg.port);
    if (sock < 0) {
        fprintf(stderr, "FAIL tcp listen %d\n", cfg.port);
        mpp_cleanup();
        return 1;
    }

    while (!g_stop) {
        fd_set rfds;
        struct timeval tv = { 1, 0 };
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        if (select(sock + 1, &rfds, NULL, NULL, &tv) <= 0)
            continue;
        {
            int cfd = accept(sock, NULL, NULL);
            int slot = -1;
            pthread_t th;
            pthread_attr_t at;
            int *pfd;
            if (cfd < 0)
                continue;
            pthread_mutex_lock(&g_cli_lock);
            for (i = 0; i < MAX_CLIENTS; i++)
                if (!g_clients[i].active) { slot = i; break; }
            if (slot >= 0) {
                g_clients[slot].fd = cfd;
                g_clients[slot].blob_mode = 1; /* Also por defecto */
                g_clients[slot].active = 1;
                g_clients[slot].gen = ++g_cli_gen;
            }
            pthread_mutex_unlock(&g_cli_lock);
            if (slot < 0) {
                send_msg(cfd, "servidor lleno (max 4 clientes)");
                close(cfd);
                continue;
            }
            pfd = malloc(sizeof(*pfd));
            if (!pfd) { close(cfd); continue; }
            *pfd = cfd;
            pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, 65536);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&th, &at, client_thread, pfd) != 0) {
                close(cfd);
                free(pfd);
                pthread_mutex_lock(&g_cli_lock);
                g_clients[slot].active = 0;
                pthread_mutex_unlock(&g_cli_lock);
            }
            pthread_attr_destroy(&at);
        }
    }

    close(sock);
    /* Esperar exposición en curso (acotado 5s) para que el worker libere su
       frame VB antes del teardown. Sin esto, vb_exit falla con busy y el
       pool viejo sobrevive al restart (set_cfg posterior = no-op). */
    {
        int waited = 0;
        while (waited < 5000) {
            int exp;
            pthread_mutex_lock(&g_dev_lock);
            exp = g_exposing;
            pthread_mutex_unlock(&g_dev_lock);
            if (!exp)
                break;
            usleep(100000);
            waited += 100;
        }
        if (waited >= 5000)
            fprintf(stderr, "  teardown: worker no liberó (sigue con pool viejo)\n");
    }
    mpp_cleanup();
    return 0;
}
#endif /* INDI_SELFTEST */
