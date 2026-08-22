/*
 * astro_streamer.c — port del pipeline waybeam_venc al firmware del usuario.
 *
 * Reproduce el pipeline HARDWARE-VERIFICADO de waybeam (mismo módulo
 * SIP-K662C6S + Hi3516CV610): MIPI completo (con ENABLE_MIPI_CLOCK),
 * VI-online + ISP 3A (via dlopen libsns_imx662.so), VPSS escalador,
 * VENC H.265 CBR, salida AnnexB por TCP.
 *
 * Diferencias vs. waybeam:
 *  - No daemon: testeo directo, flags de CLI.
 *  - Override de registros del sensor post-init via /dev/i2c-0, para poder
 *    conmutar INCK_SEL/DATARATE/LANEMODE entre la config validada del usuario
 *    (1 lane, 27MHz) y la de waybeam (4 lanes, 37.125MHz).
 *  - Salida por TCP (puerto default 5999) con header de 12 bytes.
 *
 * Compilación (desde utils/):
 *   SDK=../output/host/opt/ext-toolchain/sdk
 *   CC=../output/host/bin/arm-openipc-linux-musleabi-gcc
 *   KO=../output/build/hisilicon-opensdk-ff20187b/kernel/include/hi3516cv6xx
 *   $CC -O2 -o astro_streamer astro_streamer.c \
 *     -I$SDK/include -I$KO -I$KO/exp_inc -I$KO/isp_ext_inc \
 *     -L$SDK/lib -Wl,-rpath,$SDK/lib \
 *     -lss_mpi -lss_mpi_isp -lss_mpi_ae -lss_mpi_awb -lot_mpi_isp \
 *     -lss_mpi_sysmem -lss_mpi_sysbind -lot_osal -lsecurec -lpthread -ldl -lm
 *
 * Uso (device, tras reboot limpio y carga de módulos con imx662):
 *   /tmp/astro_streamer --lanes 4 --mclk-hz 37125000 --incksel 0x01 \
 *       --datarate 0x03 --lanemode 0x03 --port 5999
 *   # config validada (1 lane/27MHz) para comparar:
 *   /tmp/astro_streamer --preset validated --port 5999
 *   # solo medir fps de encoder (sin TCP), 10 seg:
 *   /tmp/astro_streamer --lanes 4 --bench 10
 *
 * Receiver (PC):
 *   recv_wb.py — header 12B: [0..1]='WB', [2..3]=flags LE, [4..7]=len LE,
 *               [8..11]=pts LE, luego len bytes de H265 AnnexB.
 *   (o: socat TCP:192.168.1.16:5999 file:h265.raw,append + ffplay)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "ot_type.h"
#include "ot_common.h"
#include "ot_common_video.h"
#include "ot_common_vb.h"
#include "ot_common_sys.h"
#include "ot_common_vi.h"
#include "ot_common_vpss.h"
#include "ot_common_venc.h"
#include "ot_common_isp.h"
#include "ot_common_3a.h"
#include "ot_mipi_rx.h"
#include "ot_sns_ctrl.h"
#include "ot_i2c.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_sys_mem.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vi.h"
#include "ss_mpi_vpss.h"
#include "ss_mpi_venc.h"
#include "ss_mpi_sys_bind.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_ae.h"
#include "ss_mpi_awb.h"

#define MIPI_DEV_NODE    "/dev/ot_mipi_rx"
#define I2C_DEV_NODE     "/dev/i2c-0"
#define I2C_DEV_ADDR     0x34       /* 8-bit (7-bit 0x1a) */
#define SNS_LIB_PATH     "/usr/lib/sensors/libsns_imx662.so"
#define SNS_OBJ_SYMBOL   "g_sns_imx662_obj"
#define IMX662_SNS_ID    662

#define VI_DEV           0
#define VI_PIPE          0
#define VI_CHN           0
#define VPSS_GRP         0
#define VPSS_CHN         0
#define VENC_CHN         0

#define WB_MAGIC0        'W'
#define WB_MAGIC1        'B'
#define WB_HDR_LEN       28

#define AS_HDR_LEN       48
#define AS_MAGIC0        'A'
#define AS_MAGIC1        'S'
#define SNS0_CLK_HZ_PATH "/sys/module/open_sys_config/parameters/sns0_clk_hz"

/* Modo RAW: registros de control directo del sensor */
#define REG_EXP_LSB     0x3050
#define REG_EXP_MID     0x3051
#define REG_EXP_MSB     0x3052
/* IMX662: GAIN[10:0] es UN registro combinado (analog+digital). Valor = dB*10/3.
   No hay again/dgain separados (0x306C/0x306D NO son ganancia, leen 0). */
#define REG_GAIN_LSB    0x3070   /* GAIN[7:0]  */
#define REG_GAIN_MSB    0x3071   /* GAIN[10:8] */
#define REG_TEMP_MSB    0x014A
#define REG_TEMP_LSB    0x014B
#define REG_GROUP_HOLD  0x3001
#define VMAX_DEFAULT    1250
#define EXP_MIN_LINES   11
#define EXP_MAX_LINES   0xFFFF7
#define RAW12_BUF_SIZE  (IMG_STRIDE * 1080)
#define IMG_STRIDE      ((1920 * 12 + 127) / 128 * 128 / 8)

enum { MODE_RAW = 0, MODE_H265 = 1 };

/* Line time exacto: 1980/74.25MHz = 26.6667 us */
static td_u64 us_to_lines(td_u64 us) { return (us * 3) / 80; }
static td_u64 lines_to_us(td_u64 lines) { return (lines * 80) / 3; }

#define REG_STANDBY      0x3000
#define REG_XMSTA        0x3001
#define REG_INCK_SEL     0x3014
#define REG_DATARATE     0x3015
#define REG_VMAX_L       0x3028
#define REG_VMAX_M       0x3029
#define REG_VMAX_H       0x302A
#define REG_HMAX_L       0x302C
#define REG_HMAX_H       0x302D
#define REG_LANEMODE     0x3040

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

typedef struct {
    unsigned int width, height;      /* captura del sensor */
    float fps;
    int lanes;                       /* 1..4 */
    int data_rate_x2;
    int bayer;                       /* ot_isp_bayer_format 0..3 */
    int raw_bit;                     /* 10 o 12 */
    uint32_t sensor_clock_hz;        /* MCLK; 0 = no tocar el knob */
    unsigned int out_width, out_height;
    int vi_online;
    int i2c_bus;
    /* overrides post-lib-init; -1 = sin override */
    int incksel;
    int datarate;
    int lanemode;
    int vmax;
    int hmax;
    uint32_t bitrate_kbps;
    uint32_t gop_sec;
    int port;                        /* TCP salida; 0 = bench sin TCP */
    int ctrl_port;                   /* TCP control AE; 0 = desactivado */
    int bench_sec;
    int blk_raw, blk_yuv, blk_out;   /* conteos VB */
    int no_sys_clean;
    int mode;                        /* MODE_RAW | MODE_H265 */
} TestConfig;

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_switching;   /* switch_mode() en curso */
static volatile sig_atomic_t g_kick_client; /* cerrar data client en switch */
static int g_mode = MODE_H265;              /* modo activo */
static TestConfig g_cfg;                    /* copia para switch_mode() */
static pthread_t g_isp_thread;
static int g_isp_thread_ok;
static int g_i2c_fd = -1;
static void *g_sns_handle;
static ot_isp_sns_obj *g_sns_obj;
static ot_isp_3a_alg_lib g_ae_lib = { .id = VI_PIPE, .lib_name = "ot_ae_lib" };
static ot_isp_3a_alg_lib g_awb_lib = { .id = VI_PIPE, .lib_name = "ot_awb_lib" };

static uint64_t g_frames, g_bytes, g_idr;

/* --- estado RAW (astrofotografia) --- */
static td_u32 g_exposure_lines = VMAX_DEFAULT - 8;
static td_u32 g_vmax = VMAX_DEFAULT;
static td_u32 g_again = 1024;
static td_u32 g_dgain = 1024;
static int g_burst_remaining = 0;
static char g_frame_type[8] = "LIGHT";
static char g_object[64] = "UNKNOWN";
static int g_save_next = 0;
static char g_save_filename[256] = "/tmp/frame.fits";
static td_u64 g_last_temp_us = 0;
static td_s32 g_cached_temp = 0;

/* --- control AE manual (canal ctrl) --- */
static volatile int g_manual;          /* 1 = AE manual */
static td_u32 g_man_exp_us = 10000;    /* exposicion manual (us) */
static td_u32 g_man_again = 1024;      /* 22.10: 1024 = 1x */
static td_u32 g_man_dgain = 1024;      /* 22.10: 1024 = 1x */
static int g_ae_enabled = 0;           /* AE registrado y corriendo (solo H265) */
static td_u32 g_max_exp_us = 33333;    /* ~90% del periodo de frame (30fps) */
static pthread_t g_ctrl_thread;
static int g_ctrl_thread_arg;

static void sig_handler(int sig)
{
    fprintf(stderr, "[signal] recibido sig=%d -> g_stop=1\n", sig);
    fflush(stderr);
    g_stop = 1;
}

static int switch_mode(int new_mode, const TestConfig *c);

static void *isp_thread_fn(void *arg)
{
    (void)arg;
    td_s32 ret = ss_mpi_isp_run(VI_PIPE);
    printf("[isp] ss_mpi_isp_run returned 0x%x\n", ret);
    return NULL;
}

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* ----------------------------------------------------------------- I2C -- */

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

/* ------------------------------------------------------- control RAW sensor -- */

static void sensor_write_vmax(td_u32 vmax)
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

/* Sony convention: SHR = VMAX - exposure (V4L2 ref).  SHR>=11 minimo. */
static void sensor_write_exposure(td_u32 exp_lines)
{
    if (exp_lines > g_vmax - 4) exp_lines = g_vmax - 4;
    if (exp_lines > EXP_MAX_LINES) exp_lines = EXP_MAX_LINES;
    if (exp_lines < EXP_MIN_LINES) exp_lines = EXP_MIN_LINES;
    g_exposure_lines = exp_lines;
    td_u32 shr = (g_vmax > exp_lines) ? (g_vmax - exp_lines) : 4;
    if (shr < 11) shr = 11;
    if (shr > g_vmax - 4) shr = g_vmax - 4;
    i2c_write_reg(REG_GROUP_HOLD, 0x01);
    i2c_write_reg(REG_EXP_LSB, (td_u8)(shr & 0xFF));
    i2c_write_reg(REG_EXP_MID, (td_u8)((shr >> 8) & 0xFF));
    i2c_write_reg(REG_EXP_MSB, (td_u8)((shr >> 16) & 0x0F));
    i2c_write_reg(REG_GROUP_HOLD, 0x00);
}

static void sensor_set_exposure_us(td_u64 us)
{
    td_u32 lines = (td_u32)us_to_lines(us);
    if (lines < EXP_MIN_LINES) lines = EXP_MIN_LINES;
    if (lines > EXP_MAX_LINES) lines = EXP_MAX_LINES;
    if (lines + 8 > g_vmax)
        sensor_write_vmax(lines + 8);
    sensor_write_exposure(lines);
}

static void sensor_set_gain(td_u32 again, td_u32 dgain)
{
    if (again < 1024) again = 1024;
    if (dgain < 1024) dgain = 1024;
    if (again > 32768) again = 32768;
    if (dgain > 16384) dgain = 16384;
    g_again = again;
    g_dgain = dgain;
    /* IMX662 GAIN[10:0] combinado. again/dgain son 22.10 (1024=1x).
       Ganancia lineal total = again/1024 * dgain/1024; dB = 20*log10(total);
       registro = dB*10/3 (paso 0.3 dB). Ej: 2x(6dB) -> 0x14. */
    double total = (again / 1024.0) * (dgain / 1024.0);
    double db = 20.0 * log10(total);
    if (db < 0) db = 0;
    td_u32 reg = (td_u32)(db * 10.0 / 3.0 + 0.5);
    if (reg > 0x3FF) reg = 0x3FF;   /* 11 bits */
    i2c_write_reg(REG_GROUP_HOLD, 0x01);
    i2c_write_reg(REG_GAIN_LSB, (td_u8)(reg & 0xFF));
    i2c_write_reg(REG_GAIN_MSB, (td_u8)((reg >> 8) & 0x07));
    i2c_write_reg(REG_GROUP_HOLD, 0x00);
}

static void sensor_set_iso(td_u32 iso)
{
    td_u32 gain = iso * 1024UL / 100;
    if (gain < 1024) gain = 1024;
    td_u32 again = gain, dgain = 1024;
    if (again > 32768) {
        dgain = again * 1024 / 32768;
        if (dgain > 16384) dgain = 16384;
        again = 32768;
    }
    sensor_set_gain(again, dgain);
}

static td_s32 sensor_read_temp_x100(void)
{
    td_u32 raw = ((td_u32)i2c_read_reg(REG_TEMP_MSB) << 8) |
                 (td_u32)i2c_read_reg(REG_TEMP_LSB);
    td_s32 s = (raw & 0x8000) ? (td_s32)raw - 0x10000 : (td_s32)raw;
    return s;
}

static td_s32 get_temp_cached(void)
{
    uint64_t now = now_us();
    if (g_last_temp_us == 0 || now - g_last_temp_us > 1000000ULL) {
        g_cached_temp = sensor_read_temp_x100();
        g_last_temp_us = now;
    }
    return g_cached_temp;
}

/* Override de la config del sensor DESPUES del init del lib. Entra en
 * standby, reescribe INCK/DATARATE/LANEMODE (+VMAX/HMAX opcional), sale de
 * standby y espera el lock del PLL. Debe ejecutarse antes de iniciar el
 * streaming (vias VENC) para que la relockeada no perturbe la entrega. */
static int sensor_override(const TestConfig *c)
{
    int changed = (c->incksel >= 0) || (c->datarate >= 0) ||
                  (c->lanemode >= 0) || (c->vmax > 0) || (c->hmax > 0);
    if (!changed)
        return 0;

    printf("  sensor override: standby -> INCK=%d DR=%d LANE=%d "
           "VMAX=%d HMAX=%d -> unstandby\n", c->incksel, c->datarate,
           c->lanemode, c->vmax, c->hmax);
    if (i2c_write_reg(REG_STANDBY, 0x01) != 0) {
        fprintf(stderr, "FAIL sensor standby\n");
        return -1;
    }
    usleep(2000);
    if (c->incksel >= 0 && i2c_write_reg(REG_INCK_SEL, (td_u8)c->incksel) != 0)
        return -1;
    if (c->datarate >= 0 && i2c_write_reg(REG_DATARATE, (td_u8)c->datarate) != 0)
        return -1;
    if (c->lanemode >= 0 && i2c_write_reg(REG_LANEMODE, (td_u8)c->lanemode) != 0)
        return -1;
    if (c->vmax > 0) {
        if (i2c_write_reg(REG_VMAX_L, (td_u8)(c->vmax & 0xFF)) != 0 ||
            i2c_write_reg(REG_VMAX_M, (td_u8)((c->vmax >> 8) & 0xFF)) != 0 ||
            i2c_write_reg(REG_VMAX_H, (td_u8)((c->vmax >> 16) & 0x0F)) != 0)
            return -1;
    }
    if (c->hmax > 0) {
        if (i2c_write_reg(REG_HMAX_L, (td_u8)(c->hmax & 0xFF)) != 0 ||
            i2c_write_reg(REG_HMAX_H, (td_u8)((c->hmax >> 8) & 0xFF)) != 0)
            return -1;
    }
    if (i2c_write_reg(REG_STANDBY, 0x00) != 0)
        return -1;
    usleep(100000);  /* PLL relock */
    i2c_write_reg(REG_XMSTA, 0x00);
    printf("  readback: 0x3014=0x%02X 0x3015=0x%02X 0x3040=0x%02X\n",
           i2c_read_reg(REG_INCK_SEL), i2c_read_reg(REG_DATARATE),
           i2c_read_reg(REG_LANEMODE));
    return 0;
}

/* ------------------------------------------------------------ sensor MCLK -- */

static int sensor_clock_select(uint32_t hz)
{
    char buf[16];
    int fd, len;

    if (hz == 0)
        return 0;
    fd = open(SNS0_CLK_HZ_PATH, O_WRONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "WARNING: %s absent (kernel sin knob) — MCLK "
                    "queda como cargo el loader; %u Hz NO se aplica\n",
                    SNS0_CLK_HZ_PATH, hz);
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

/* ------------------------------------------------------- init sensor RAW -- */

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

static int init_sensor_full(const TestConfig *c)
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
    printf("  Phase 1: %u regs: %d ok %d fail\n",
           (unsigned)(sizeof(imx662_init_common)/sizeof(imx662_init_common[0])),
           ok, fail);

    ok = fail = 0;
    for (i = 0; i < sizeof(imx662_init_mode)/sizeof(imx662_init_mode[0]); i++) {
        if (i2c_write_reg(imx662_init_mode[i].reg, imx662_init_mode[i].val) == 0)
            ok++;
        else
            fail++;
        usleep(100);
    }
    printf("  Phase 2: %u mode regs: %d ok %d fail\n",
           (unsigned)(sizeof(imx662_init_mode)/sizeof(imx662_init_mode[0])),
           ok, fail);

    /* Override INCK/DATARATE/LANEMODE (en standby, parity validated) */
    printf("  Override: INCK=0x%02X DR=0x%02X LANE=0x%02X\n",
           (unsigned)c->incksel, (unsigned)c->datarate, (unsigned)c->lanemode);
    if (c->incksel >= 0) i2c_write_reg(REG_INCK_SEL, (td_u8)c->incksel);
    if (c->datarate >= 0) i2c_write_reg(REG_DATARATE, (td_u8)c->datarate);
    if (c->lanemode >= 0) i2c_write_reg(REG_LANEMODE, (td_u8)c->lanemode);

    i2c_write_reg(0x3458, 0x00);
    i2c_write_reg(REG_STANDBY, 0x00);
    i2c_write_reg(REG_XMSTA, 0x00);
    usleep(25000);

    sensor_write_vmax(VMAX_DEFAULT);
    sensor_set_exposure_us(33333);
    sensor_set_gain(g_again, g_dgain);
    fprintf(stderr, "  [readback] 30DC=%02X 3000=%02X 3001=%02X 3014=%02X "
            "3015=%02X 3040=%02X 30B0=%02X\n",
            i2c_read_reg(0x30DC), i2c_read_reg(0x3000), i2c_read_reg(0x3001),
            i2c_read_reg(0x3014), i2c_read_reg(0x3015), i2c_read_reg(0x3040),
            i2c_read_reg(0x30B0));
    return 0;
}

/* ----------------------------------------------------------------- MIPI -- */

static int mipi_setup(const TestConfig *c)
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
    attr.img_rect.width = c->width;
    attr.img_rect.height = c->height;
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
    /* V4L2 ref: imx662_XCLR_MIN_DELAY_US = 500000 (sensor internal
       calibration after XCLR deassert before capture).  Necesario para
       el I2C directo en modo RAW (astro parity). */
    usleep(500000);
#undef mipi_ioctl
    close(fd);
    return 0;
}

/* Replica EXACTA de imx662_enable_mclk() del lib sensor (libsns_imx662.so ->
   cmos_isp_init).  El MCLK se habilita ANTES del reset del sensor (clock ON
   durante el pulso de reset).  mipi_setup resetea con clock OFF, lo que en
   modo RAW NO deja al sensor respondiendo al I2C directo (todos los writes
   fallan, chip ID=0x00).  Esta segunda secuencia —igual a la que usa el lib
   en modo H265 y que SI funciona— es la que realmente inicializa el sensor
   para acceso I2C directo.  Se llama en el path RAW justo antes de
   init_sensor_full(). */
static int sensor_mclk_reset(void)
{
    int fd = open(MIPI_DEV_NODE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "FAIL open %s: %s\n", MIPI_DEV_NODE, strerror(errno));
        return -1;
    }
    lane_divide_mode_t hs = LANE_DIVIDE_MODE_0;
    sns_clk_source_t clk = 0;
    sns_rst_source_t rst = 0;
    (void)ioctl(fd, OT_MIPI_SET_HS_MODE, &hs);
    (void)ioctl(fd, OT_MIPI_ENABLE_SENSOR_CLOCK, &clk);
    (void)ioctl(fd, OT_MIPI_RESET_SENSOR, &rst);
    usleep(10000);
    (void)ioctl(fd, OT_MIPI_UNRESET_SENSOR, &rst);
    usleep(100000);
    close(fd);
    return 0;
}

/* -------------------------------------------------------------- VB / SYS -- */

static int sys_setup(const TestConfig *c)
{
    ot_vb_cfg vb;
    ot_vi_vpss_mode vi_vpss_mode;
    td_u64 raw_blk, yuv_blk, out_blk;
    td_s32 sys_ret, vb_ret;
    unsigned int i;

    raw_blk = (td_u64)c->width * c->height * 2 + 0x4000;
    yuv_blk = (td_u64)c->width * c->height * 3 / 2 + 0x4000;
    out_blk = (td_u64)c->out_width * c->out_height * 3 / 2 + 0x4000;

    memset(&vb, 0, sizeof(vb));
    vb.max_pool_cnt = 2;
    vb.common_pool[0].blk_size = raw_blk;
    vb.common_pool[0].blk_cnt = (td_u32)(c->blk_raw > 0 ? c->blk_raw : 4);
    vb.common_pool[1].blk_size = yuv_blk;
    vb.common_pool[1].blk_cnt = (td_u32)(c->blk_yuv > 0 ? c->blk_yuv : 4);
    if (out_blk < yuv_blk) {
        vb.max_pool_cnt = 3;
        vb.common_pool[2].blk_size = out_blk;
        vb.common_pool[2].blk_cnt = (td_u32)(c->blk_out > 0 ? c->blk_out : 4);
    }

    if (!c->no_sys_clean) {
        sys_ret = ss_mpi_sys_exit();
        vb_ret = ss_mpi_vb_exit();
        printf("  pre-clean: sys_exit=0x%x vb_exit=0x%x\n", sys_ret, vb_ret);
    }

    memset(&vi_vpss_mode, 0, sizeof(vi_vpss_mode));
    for (i = 0; i < OT_VI_MAX_PIPE_NUM; i++)
        vi_vpss_mode.mode[i] = OT_VI_OFFLINE_VPSS_OFFLINE;
    if (c->vi_online)
        vi_vpss_mode.mode[VI_PIPE] = OT_VI_ONLINE_VPSS_OFFLINE;

    CHECK_OR_BUSY(ss_mpi_vb_set_cfg(&vb));
    CHECK_OR_BUSY(ss_mpi_vb_init());
    if (c->vi_online) {
        CHECK(ss_mpi_sys_init());
        CHECK(ss_mpi_sys_set_vi_vpss_mode(&vi_vpss_mode));
        CHECK(ss_mpi_sys_set_vi_aiisp_mode(VI_PIPE, OT_VI_AIISP_MODE_DEFAULT));
        memset(&vi_vpss_mode, 0, sizeof(vi_vpss_mode));
        CHECK(ss_mpi_sys_get_vi_vpss_mode(&vi_vpss_mode));
        if (vi_vpss_mode.mode[VI_PIPE] != OT_VI_ONLINE_VPSS_OFFLINE) {
            fprintf(stderr, "FAIL VI pipe %d mode readback = %d\n",
                    VI_PIPE, vi_vpss_mode.mode[VI_PIPE]);
            return -1;
        }
    } else {
        CHECK_OR_BUSY(ss_mpi_sys_init());
    }
    /* init_mod_common_pool ANTES del reset del sensor (parity astro_streamer):
       astro los llama antes de enable_mclk_and_reset_sensor y el I2C directo
       funciona con un solo reset clock-off.  waybeam los hacia despues
       (dentro de vi_setup_raw) y el sensor no responde al I2C sin un 2do
       reset clock-on. */
    {
        td_s32 r;
        r = ss_mpi_vb_init_mod_common_pool(OT_VB_UID_VI);
        fprintf(stderr, "  init_mod_common_pool(VI): 0x%x (no-fatal)\n", r);
        r = ss_mpi_vb_init_mod_common_pool(OT_VB_UID_VPSS);
        fprintf(stderr, "  init_mod_common_pool(VPSS): 0x%x (no-fatal)\n", r);
        r = ss_mpi_vb_init_mod_common_pool(OT_VB_UID_VENC);
        fprintf(stderr, "  init_mod_common_pool(VENC): 0x%x (no-fatal)\n", r);
    }
    printf("  ok  VI/ISP mode: %s\n", c->vi_online ? "online" : "offline");
    return 0;
}

/* ------------------------------------------------------------------ VI -- */

static int vi_setup(const TestConfig *c)
{
    ot_vi_dev_attr dev_attr;
    ot_vi_pipe_attr pipe_attr;

    ss_mpi_vi_disable_chn(VI_PIPE, VI_CHN);
    ss_mpi_vi_stop_pipe(VI_PIPE);
    ss_mpi_vi_destroy_pipe(VI_PIPE);
    ss_mpi_vi_unbind(VI_DEV, VI_PIPE);
    ss_mpi_vi_disable_dev(VI_DEV);

    memset(&dev_attr, 0, sizeof(dev_attr));
    dev_attr.intf_mode = OT_VI_INTF_MODE_MIPI;
    dev_attr.work_mode = OT_VI_WORK_MODE_MULTIPLEX_1;
    dev_attr.component_mask[0] = 0xFFC00000;
    dev_attr.component_mask[1] = 0x0;
    dev_attr.scan_mode = OT_VI_SCAN_PROGRESSIVE;
    dev_attr.ad_chn_id[0] = -1;
    dev_attr.ad_chn_id[1] = -1;
    dev_attr.ad_chn_id[2] = -1;
    dev_attr.ad_chn_id[3] = -1;
    dev_attr.data_seq = OT_VI_DATA_SEQ_YUYV;
    dev_attr.data_type = OT_VI_DATA_TYPE_RAW;
    dev_attr.data_reverse = TD_FALSE;
    dev_attr.in_size.width = c->width;
    dev_attr.in_size.height = c->height;
    dev_attr.data_rate = c->data_rate_x2 ? OT_DATA_RATE_X2 : OT_DATA_RATE_X1;

    CHECK(ss_mpi_vi_set_dev_attr(VI_DEV, &dev_attr));
    CHECK(ss_mpi_vi_enable_dev(VI_DEV));
    CHECK(ss_mpi_vi_bind(VI_DEV, VI_PIPE));

    memset(&pipe_attr, 0, sizeof(pipe_attr));
    pipe_attr.pipe_bypass_mode = OT_VI_PIPE_BYPASS_NONE;
    pipe_attr.isp_bypass = TD_FALSE;
    pipe_attr.size.width = c->width;
    pipe_attr.size.height = c->height;
    pipe_attr.pixel_format = (c->raw_bit == 10) ?
        OT_PIXEL_FORMAT_RGB_BAYER_10BPP : OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
    pipe_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    pipe_attr.frame_rate_ctrl.src_frame_rate = OT_VI_INVALID_FRAME_RATE;
    pipe_attr.frame_rate_ctrl.dst_frame_rate = OT_VI_INVALID_FRAME_RATE;

    CHECK(ss_mpi_vi_create_pipe(VI_PIPE, &pipe_attr));
    CHECK(ss_mpi_vi_start_pipe(VI_PIPE));
    return 0;
}

static int vi_start_chn(const TestConfig *c)
{
    ot_vi_chn_attr chn_attr;

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.size.width = c->width;
    chn_attr.size.height = c->height;
    chn_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    chn_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    chn_attr.video_format = OT_VIDEO_FORMAT_LINEAR;
    chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    chn_attr.mirror_en = TD_FALSE;
    chn_attr.flip_en = TD_FALSE;
    chn_attr.depth = 0;
    chn_attr.frame_rate_ctrl.src_frame_rate = OT_VI_INVALID_FRAME_RATE;
    chn_attr.frame_rate_ctrl.dst_frame_rate = OT_VI_INVALID_FRAME_RATE;

    CHECK(ss_mpi_vi_set_chn_attr(VI_PIPE, VI_CHN, &chn_attr));
    CHECK(ss_mpi_vi_enable_chn(VI_PIPE, VI_CHN));
    return 0;
}

/* VI raw (ISP bypass): pipe isp_bypass=TRUE, chn bayer 12bpp.  Usado en
 * modo RAW para astrofotografia (control directo de exposición/ganancia). */
static int vi_setup_raw(const TestConfig *c)
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
    dev_attr.in_size.width = c->width;
    dev_attr.in_size.height = c->height;
    dev_attr.data_rate = c->data_rate_x2 ? OT_DATA_RATE_X2 : OT_DATA_RATE_X1;
    CHECK(ss_mpi_vi_set_dev_attr(VI_DEV, &dev_attr));
    CHECK(ss_mpi_vi_enable_dev(VI_DEV));
    CHECK(ss_mpi_vi_bind(VI_DEV, VI_PIPE));

    memset(&pipe_attr, 0, sizeof(pipe_attr));
    pipe_attr.isp_bypass = TD_TRUE;
    pipe_attr.size.width = c->width;
    pipe_attr.size.height = c->height;
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
    chn_attr.size.width = c->width;
    chn_attr.size.height = c->height;
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

/* ---------------------------------------------------------- sensor / ISP -- */

static int sensor_setup(int i2c_bus)
{
    ot_isp_sns_commbus bus;

    g_sns_handle = dlopen(SNS_LIB_PATH, RTLD_NOW | RTLD_GLOBAL);
    if (g_sns_handle == NULL) {
        fprintf(stderr, "dlopen %s: %s\n", SNS_LIB_PATH, dlerror());
        return -1;
    }
    g_sns_obj = (ot_isp_sns_obj *)dlsym(g_sns_handle, SNS_OBJ_SYMBOL);
    if (g_sns_obj == NULL) {
        fprintf(stderr, "dlsym %s: %s\n", SNS_OBJ_SYMBOL, dlerror());
        return -1;
    }
    printf("  ok  dlopen %s -> %s @ %p\n", SNS_LIB_PATH, SNS_OBJ_SYMBOL,
           (void *)g_sns_obj);

    if (g_sns_obj->pfn_set_bus_info != NULL) {
        bus.i2c_dev = (td_s8)i2c_bus;
        CHECK(g_sns_obj->pfn_set_bus_info(VI_PIPE, bus));
    }
    CHECK(ss_mpi_ae_register(VI_PIPE, &g_ae_lib));
    CHECK(ss_mpi_awb_register(VI_PIPE, &g_awb_lib));
    CHECK(g_sns_obj->pfn_register_callback(VI_PIPE, &g_ae_lib, &g_awb_lib));
    g_ae_enabled = 1;
    return 0;
}

static int isp_setup(const TestConfig *c)
{
    ot_isp_pub_attr pub;
    ot_isp_bind_attr bind;

    memset(&pub, 0, sizeof(pub));
    pub.wnd_rect.x = 0;
    pub.wnd_rect.y = 0;
    pub.wnd_rect.width = c->width;
    pub.wnd_rect.height = c->height;
    pub.sns_size.width = c->width;
    pub.sns_size.height = c->height;
    pub.frame_rate = c->fps;
    pub.bayer_format = (ot_isp_bayer_format)c->bayer;
    pub.wdr_mode = OT_WDR_MODE_NONE;
    pub.sns_mode = 0;

    memset(&bind, 0, sizeof(bind));
    bind.sns_id = IMX662_SNS_ID;
    bind.ae_lib = g_ae_lib;
    bind.awb_lib = g_awb_lib;
    CHECK(ss_mpi_isp_set_bind_attr(VI_PIPE, &bind));

    CHECK(ss_mpi_isp_mem_init(VI_PIPE));
    CHECK(ss_mpi_isp_set_pub_attr(VI_PIPE, &pub));
    CHECK(ss_mpi_isp_init(VI_PIPE));
    return 0;
}

static int configure_output_color(void)
{
    ot_isp_csc_attr csc;
    td_s32 ret;

    memset(&csc, 0, sizeof(csc));
    ret = ss_mpi_isp_get_csc_attr(VI_PIPE, &csc);
    if (ret != TD_SUCCESS) {
        fprintf(stderr, "FAIL ss_mpi_isp_get_csc_attr = 0x%x\n", ret);
        return -1;
    }
    csc.enable = TD_TRUE;
    csc.satu = 60;
    csc.contr = 53;
    ret = ss_mpi_isp_set_csc_attr(VI_PIPE, &csc);
    if (ret != TD_SUCCESS) {
        fprintf(stderr, "FAIL ss_mpi_isp_set_csc_attr = 0x%x\n", ret);
        return -1;
    }
    printf("  output CSC: satu=%u contrast=%u full-range=%d\n",
           csc.satu, csc.contr, !csc.limited_range_en);
    return 0;
}

static int enable_sensor_ccm(void)
{
    ot_isp_color_matrix_attr attr;
    td_s32 ret;

    memset(&attr, 0, sizeof(attr));
    ret = ss_mpi_isp_get_ccm_attr(VI_PIPE, &attr);
    if (ret != TD_SUCCESS) {
        fprintf(stderr, "WARNING: ccm get = 0x%x (no CCM)\n", ret);
        return 0;
    }
    if (attr.auto_attr.ccm_tab_num < 3) {
        printf("  WARNING: sensor dio %u anchors CCM — skip\n",
               attr.auto_attr.ccm_tab_num);
        return 0;
    }
    attr.op_type = OT_OP_MODE_AUTO;
    attr.auto_attr.iso_act_en = TD_FALSE;
    attr.auto_attr.temp_act_en = TD_FALSE;
    ret = ss_mpi_isp_set_ccm_attr(VI_PIPE, &attr);
    if (ret != TD_SUCCESS) {
        fprintf(stderr, "WARNING: ccm set = 0x%x\n", ret);
        return 0;
    }
    printf("  sensor CCM enabled: %u anchors\n", attr.auto_attr.ccm_tab_num);
    return 0;
}

/* ----------------------------------------------------------------- VPSS -- */

static int vpss_setup(const TestConfig *c)
{
    ot_vpss_grp_attr grp_attr;
    ot_vpss_chn_attr chn_attr;
    ot_mpp_chn src;
    ot_mpp_chn dst;

    memset(&grp_attr, 0, sizeof(grp_attr));
    grp_attr.max_width = c->width;
    grp_attr.max_height = c->height;
    grp_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    grp_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    grp_attr.dei_mode = OT_VPSS_DEI_MODE_OFF;
    grp_attr.frame_rate.src_frame_rate = -1;
    grp_attr.frame_rate.dst_frame_rate = -1;
    CHECK(ss_mpi_vpss_create_grp(VPSS_GRP, &grp_attr));

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.width = c->out_width;
    chn_attr.height = c->out_height;
    chn_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    chn_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    chn_attr.video_format = OT_VIDEO_FORMAT_LINEAR;
    chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    chn_attr.chn_mode = OT_VPSS_CHN_MODE_USER;
    chn_attr.depth = 0;
    chn_attr.frame_rate.src_frame_rate = -1;
    chn_attr.frame_rate.dst_frame_rate = -1;
    CHECK(ss_mpi_vpss_set_chn_attr(VPSS_GRP, VPSS_CHN, &chn_attr));
    CHECK(ss_mpi_vpss_enable_chn(VPSS_GRP, VPSS_CHN));
    CHECK(ss_mpi_vpss_start_grp(VPSS_GRP));

    src.mod_id = OT_ID_VI;
    src.dev_id = VI_PIPE;
    src.chn_id = VI_CHN;
    dst.mod_id = OT_ID_VPSS;
    dst.dev_id = VPSS_GRP;
    dst.chn_id = 0;
    CHECK(ss_mpi_sys_bind(&src, &dst));
    printf("  ok  VPSS %ux%u -> %ux%u%s\n", c->width, c->height,
           c->out_width, c->out_height,
           (c->out_width == c->width && c->out_height == c->height) ?
               " (1:1)" : " (scaled)");
    return 0;
}

/* ----------------------------------------------------------------- VENC -- */

static int venc_start(const TestConfig *c)
{
    ot_venc_chn_attr attr;
    ot_venc_h265_vui vui;
    ot_venc_start_param start;
    ot_mpp_chn src, dst;
    uint32_t gop;
    td_s32 ret;

    gop = (c->gop_sec <= 0) ? (uint32_t)(2 * c->fps + 0.5f) :
                              (uint32_t)(c->gop_sec * c->fps + 0.5f);
    if (gop == 0)
        gop = 1;

    memset(&attr, 0, sizeof(attr));
    attr.venc_attr.type = OT_PT_H265;
    attr.venc_attr.max_pic_width = c->out_width;
    attr.venc_attr.max_pic_height = c->out_height;
    attr.venc_attr.buf_size =
        ((c->out_width * c->out_height * 3 / 4) + 63) & ~63u;
    attr.venc_attr.profile = 0;
    attr.venc_attr.is_by_frame = TD_TRUE;
    attr.venc_attr.pic_width = c->out_width;
    attr.venc_attr.pic_height = c->out_height;
    attr.venc_attr.h265_attr.rcn_ref_share_buf_en = TD_TRUE;
    attr.venc_attr.h265_attr.frame_buf_ratio = 75;
    attr.rc_attr.rc_mode = OT_VENC_RC_MODE_H265_CBR;
    attr.rc_attr.h265_cbr.gop = gop;
    attr.rc_attr.h265_cbr.stats_time = 1;
    attr.rc_attr.h265_cbr.src_frame_rate = (td_u32)c->fps;
    attr.rc_attr.h265_cbr.dst_frame_rate = (td_u32)c->fps;
    attr.rc_attr.h265_cbr.bit_rate = c->bitrate_kbps;
    attr.gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
    attr.gop_attr.normal_p.ip_qp_delta = 0;

    ret = ss_mpi_venc_create_chn(VENC_CHN, &attr);
    if (ret != TD_SUCCESS) {
        fprintf(stderr, "FAIL ss_mpi_venc_create_chn = 0x%x\n", ret);
        return -1;
    }

    memset(&vui, 0, sizeof(vui));
    ret = ss_mpi_venc_get_h265_vui(VENC_CHN, &vui);
    if (ret != TD_SUCCESS) {
        fprintf(stderr, "FAIL ss_mpi_venc_get_h265_vui = 0x%x\n", ret);
        return -1;
    }
    vui.vui_time_info.timing_info_present_flag = 1;
    vui.vui_time_info.num_units_in_tick = 1000;
    vui.vui_time_info.time_scale = (td_u32)(c->fps * 1000);
    vui.vui_time_info.num_ticks_poc_diff_one_minus1 = 0;
    vui.vui_video_signal.video_signal_type_present_flag = 1;
    vui.vui_video_signal.video_format = 5;
    vui.vui_video_signal.video_full_range_flag = 1;
    vui.vui_video_signal.colour_description_present_flag = 1;
    vui.vui_video_signal.colour_primaries = 1;
    vui.vui_video_signal.transfer_characteristics = 1;
    vui.vui_video_signal.matrix_coefficients = 1;
    if (ss_mpi_venc_set_h265_vui(VENC_CHN, &vui) != TD_SUCCESS) {
        fprintf(stderr, "FAIL ss_mpi_venc_set_h265_vui\n");
        return -1;
    }

    memset(&start, 0, sizeof(start));
    start.recv_pic_num = -1;
    if (ss_mpi_venc_start_chn(VENC_CHN, &start) != TD_SUCCESS) {
        fprintf(stderr, "FAIL ss_mpi_venc_start_chn\n");
        return -1;
    }

    src.mod_id = OT_ID_VPSS;
    src.dev_id = VPSS_GRP;
    src.chn_id = VPSS_CHN;
    dst.mod_id = OT_ID_VENC;
    dst.dev_id = 0;
    dst.chn_id = VENC_CHN;
    if (ss_mpi_sys_bind(&src, &dst) != TD_SUCCESS) {
        fprintf(stderr, "FAIL bind VPSS->VENC\n");
        return -1;
    }
    printf("  ok  VENC H.265 %ux%u@%u CBR=%u kbps GOP=%u\n",
           c->out_width, c->out_height, (unsigned int)c->fps,
           c->bitrate_kbps, gop);
    return 0;
}

/* NAL H265: tipo 19 (IDR_W_RADL) o 20 (IDR_N_LP) => IDR */
static int frame_is_idr(const uint8_t *p, size_t len)
{
    size_t i;
    for (i = 0; i + 4 < len; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            uint8_t h = p[i + 4];
            uint8_t t = (uint8_t)((h >> 1) & 0x3F);
            if (t == 19 || t == 20)
                return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- TCP out -- */

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            /* El socket del cliente es O_NONBLOCK (para el select de comandos);
               un write grande puede devolver EAGAIN temporalmente. Reintentar
               en vez de cortar (sino el frame se envia parcial y se cierra). */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timeval tv = { 0, 20000 };
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(fd, &wfds);
                if (select(fd + 1, NULL, &wfds, NULL, &tv) < 0 && errno != EINTR)
                    return -1;
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int tcp_listen(int port)
{
    int fd;
    struct sockaddr_in addr;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Header RAW de 48 bytes (magic "AS") + payload raw12.  Compatible con
 * recv_astro.py (struct "<2sHHHIIQIIIIiB3x"). */
static int send_as_header(int fd, uint32_t w, uint32_t h, uint32_t stride,
                          uint32_t size, uint32_t frame_index,
                          uint64_t ts_us, uint32_t exp_us, uint32_t again,
                          uint32_t dgain, uint32_t vmax, int32_t temp_x100,
                          uint8_t capture_flag)
{
    uint8_t hdr[AS_HDR_LEN];
    int o = 0;
    memset(hdr, 0, sizeof(hdr));
    hdr[o++] = AS_MAGIC0; hdr[o++] = AS_MAGIC1;
    hdr[o++] = w & 0xFF;  hdr[o++] = (w >> 8) & 0xFF;
    hdr[o++] = h & 0xFF;  hdr[o++] = (h >> 8) & 0xFF;
    hdr[o++] = stride & 0xFF; hdr[o++] = (stride >> 8) & 0xFF;
    memcpy(hdr + o, &size, 4); o += 4;
    memcpy(hdr + o, &frame_index, 4); o += 4;
    memcpy(hdr + o, &ts_us, 8); o += 8;
    memcpy(hdr + o, &exp_us, 4); o += 4;
    memcpy(hdr + o, &again, 4); o += 4;
    memcpy(hdr + o, &dgain, 4); o += 4;
    memcpy(hdr + o, &vmax, 4); o += 4;
    memcpy(hdr + o, &temp_x100, 4); o += 4;
    hdr[o++] = capture_flag;
    return write_all(fd, hdr, AS_HDR_LEN);
}

static void fits_card(char *header, int *pos, const char *key, const char *val)
{
    snprintf(header + *pos, 81, "%-8s= %s", key, val);
    *pos = ((*pos / 80) + 1) * 80;
}

static int write_fits(const char *filename, const uint8_t *raw,
                      uint32_t width, uint32_t height, uint32_t stride,
                      uint32_t exp_lines, uint32_t again, uint32_t dgain)
{
    FILE *fp = fopen(filename, "wb");
    if (!fp) return -1;

    char header[2880];
    memset(header, ' ', sizeof(header));
    int pos = 0;
    char card[81];
    fits_card(header, &pos, "SIMPLE", "T");
    fits_card(header, &pos, "BITPIX", "16");
    fits_card(header, &pos, "NAXIS", "2");
    snprintf(card, 81, "%u", width);  fits_card(header, &pos, "NAXIS1", card);
    snprintf(card, 81, "%u", height); fits_card(header, &pos, "NAXIS2", card);
    fits_card(header, &pos, "BZERO", "0.0");
    fits_card(header, &pos, "BSCALE", "1.0");
    snprintf(card, 81, "%.6f", (double)lines_to_us(exp_lines) / 1e6);
    fits_card(header, &pos, "EXPTIME", card);
    snprintf(card, 81, "%.4f", (double)again * (double)dgain / 1024.0 / 1024.0);
    fits_card(header, &pos, "GAIN", card);
    snprintf(card, 81, "%u", again); fits_card(header, &pos, "AGAIN", card);
    snprintf(card, 81, "%u", dgain); fits_card(header, &pos, "DGAIN", card);
    fits_card(header, &pos, "BAYERPAT", "'RGGB'");
    fits_card(header, &pos, "INSTRUME", "'IMX662 Hi3516CV610'");
    fits_card(header, &pos, "FRAME", g_frame_type);
    snprintf(card, 81, "'%s'", g_object); fits_card(header, &pos, "OBJECT", card);
    fits_card(header, &pos, "XBINNING", "1");
    fits_card(header, &pos, "YBINNING", "1");
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    gmtime_r(&ts.tv_sec, &tmv);
    char dob[32];
    strftime(dob, sizeof(dob), "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(card, 81, "'%s'", dob); fits_card(header, &pos, "DATE-OBS", card);
    memset(header + pos, ' ', 80);
    memcpy(header + pos, "END", 3);
    fwrite(header, 1, 2880, fp);

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = raw + (uint64_t)y * stride;
        for (uint32_t x = 0; x < width; x += 2) {
            uint32_t idx = (x * 3) / 2;
            if (idx + 2 >= stride) break;
            uint16_t p0 = ((uint16_t)row[idx] << 4) |
                          ((uint16_t)(row[idx + 1] >> 4) & 0x0F);
            uint16_t p1 = (((uint16_t)(row[idx + 1] & 0x0F)) << 8) |
                          (uint16_t)row[idx + 2];
            uint8_t b0, b1;
            b0 = (p0 >> 8) & 0xFF; b1 = p0 & 0xFF;
            fwrite(&b0, 1, 1, fp); fwrite(&b1, 1, 1, fp);
            b0 = (p1 >> 8) & 0xFF; b1 = p1 & 0xFF;
            fwrite(&b0, 1, 1, fp); fwrite(&b1, 1, 1, fp);
        }
    }
    long pos_now = ftell(fp);
    long remainder = pos_now % 2880;
    if (remainder != 0) {
        char pad[2880];
        memset(pad, 0, sizeof(pad));
        fwrite(pad, 1, 2880 - remainder, fp);
    }
    fclose(fp);
    fprintf(stderr, "  FITS saved: %s (%ux%u, EXP=%u lines = %.3f s)\n",
            filename, width, height, exp_lines,
            (double)lines_to_us(exp_lines) / 1e6);
    return 0;
}

/* Comandos RAW (modo astro): control directo del sensor por I2C. */
static void process_command(const char *cmd)
{
    if (cmd[0] == 'E' && cmd[1] == ' ') {
        sensor_write_exposure((td_u32)atoi(cmd + 2));
    } else if (cmd[0] == 'T' && cmd[1] == ' ') {
        sensor_set_exposure_us((td_u64)atoll(cmd + 2));
    } else if (cmd[0] == 'M' && cmd[1] == ' ') {
        sensor_set_exposure_us((td_u64)atoll(cmd + 2) * 1000ULL);
    } else if (cmd[0] == 'A' && cmd[1] == ' ') {
        sensor_set_gain((td_u32)atoi(cmd + 2), g_dgain);
    } else if (cmd[0] == 'D' && cmd[1] == ' ') {
        sensor_set_gain(g_again, (td_u32)atoi(cmd + 2));
    } else if (cmd[0] == 'I' && cmd[1] == ' ') {
        sensor_set_iso((td_u32)atoi(cmd + 2));
    } else if (cmd[0] == 'V' && cmd[1] == ' ') {
        sensor_write_vmax((td_u32)atoi(cmd + 2));
    } else if (cmd[0] == 'P') {
        sensor_write_vmax(VMAX_DEFAULT);
        sensor_set_exposure_us(33333);
    } else if (cmd[0] == 'S') {
        g_save_next = 1;
        if (strlen(cmd) > 2)
            snprintf(g_save_filename, sizeof(g_save_filename), "%s", cmd + 2);
        fprintf(stderr, "  Will save next frame as FITS (%s)\n", g_save_filename);
    } else if (cmd[0] == 'B' && cmd[1] == ' ') {
        int n = atoi(cmd + 2);
        if (n < 0) n = 0;
        if (n > 1000) n = 1000;
        g_burst_remaining = n;
        fprintf(stderr, "  Burst: flagging next %d frames\n", n);
    } else if (cmd[0] == 'F' && cmd[1] == ' ') {
        snprintf(g_frame_type, sizeof(g_frame_type), "%s", cmd + 2);
    } else if (cmd[0] == 'C' && cmd[1] == ' ') {
        snprintf(g_object, sizeof(g_object), "%s", cmd + 2);
    } else if (cmd[0] == 'G' && cmd[1] == ' ') {
        td_u16 reg = (td_u16)strtol(cmd + 2, NULL, 0);
        fprintf(stderr, "  Read 0x%04X = 0x%02X\n", reg, i2c_read_reg(reg));
    } else if (cmd[0] == 'R') {
        fprintf(stderr, "  RAW E=%u VMAX=%u A=%u D=%u TEMP=%.2fC\n",
                g_exposure_lines, g_vmax, g_again, g_dgain,
                (double)get_temp_cached() / 100.0);
    } else if (cmd[0] == '?') {
        fprintf(stderr, "  E=%u VMAX=%u A=%u D=%u TYPE=%s OBJ=%s MODE=%s\n",
                g_exposure_lines, g_vmax, g_again, g_dgain, g_frame_type,
                g_object, g_mode == MODE_H265 ? "H265" : "RAW");
    }
}

/* ----------------------------------------------------------------- loop -- */

/* ------------------------------------------------------------------ loop -- */

/* Loop de datos unificado: sirve frames en el modo actual (RAW bayer o
 * H.265 AnnexB).  Accept select-based para salir con SIGTERM sin colgar. */
static void data_loop(const TestConfig *c, int server_fd)
{
    fprintf(stderr, "[data_loop] ENTER server_fd=%d g_mode=%d g_stop=%d\n",
            server_fd, g_mode, (int)g_stop);
    fflush(stderr);
    int mem_fd = -1, mem_mmaped = 0;
    void *mem_map = NULL;
    td_u64 mem_map_len = 0, mem_map_phys = 0;
    int cur_mode = -1;
    int venc_fd = -1;
    td_u32 frame_index = 0;
    td_s32 ret;

    /* Modo bench: sin TCP, medir fps del pipeline actual durante bench_sec s */
    if (server_fd < 0) {
        uint64_t t0 = now_us();
        uint64_t last_report = t0;
        uint64_t frames_at_report = 0;
        while (g_stop == 0) {
            uint64_t now = now_us();
            if (c->bench_sec > 0 &&
                now - t0 >= (uint64_t)c->bench_sec * 1000000) {
                g_stop = 1;
                break;
            }
            if (g_mode == MODE_H265) {
                if (venc_fd < 0) venc_fd = ss_mpi_venc_get_fd(VENC_CHN);
                fd_set rfds;
                struct timeval tv = { 1, 0 };
                FD_ZERO(&rfds);
                FD_SET(venc_fd, &rfds);
                if (select(venc_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
                    if (g_stop) break;
                    continue;
                }
                ot_venc_chn_status status;
                ot_venc_stream stream;
                memset(&status, 0, sizeof(status));
                if (ss_mpi_venc_query_status(VENC_CHN, &status) != TD_SUCCESS ||
                    status.cur_packs == 0) continue;
                memset(&stream, 0, sizeof(stream));
                stream.pack = calloc(status.cur_packs, sizeof(*stream.pack));
                if (!stream.pack) break;
                stream.pack_cnt = status.cur_packs;
                if (ss_mpi_venc_get_stream(VENC_CHN, &stream, 1000) != TD_SUCCESS) {
                    free(stream.pack);
                    continue;
                }
                size_t total = 0;
                int i;
                for (i = 0; i < (int)stream.pack_cnt; i++) total += stream.pack[i].len;
                for (i = 0; i < (int)stream.pack_cnt; i++)
                    if (frame_is_idr(stream.pack[i].addr, stream.pack[i].len)) g_idr++;
                g_frames++;
                g_bytes += total;
                ss_mpi_venc_release_stream(VENC_CHN, &stream);
                free(stream.pack);
            } else {
                ot_video_frame_info frame_info;
                memset(&frame_info, 0, sizeof(frame_info));
                ret = ss_mpi_vi_get_chn_frame(VI_PIPE, VI_CHN, &frame_info, 3000);
                if (ret != TD_SUCCESS) {
                    if (g_stop) break;
                    continue;
                }
                g_frames++;
                g_bytes += (td_u64)frame_info.video_frame.stride[0] *
                            frame_info.video_frame.height;
                ss_mpi_vi_release_chn_frame(VI_PIPE, VI_CHN, &frame_info);
            }
            uint64_t report_now = now_us();
            if (report_now - last_report >= 1000000) {
                fprintf(stderr, "[bench] fps=%llu bytes=%llu\n",
                        (unsigned long long)(g_frames - frames_at_report),
                        (unsigned long long)g_bytes);
                frames_at_report = g_frames;
                last_report = report_now;
            }
        }
        fprintf(stderr, "[bench] total frames=%llu bytes=%llu idr=%llu\n",
                (unsigned long long)g_frames, (unsigned long long)g_bytes,
                (unsigned long long)g_idr);
        return;
    }

    while (g_stop == 0) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = -1;

        for (;;) {
            fd_set rfds;
            struct timeval tv = { 0, 200000 };
            FD_ZERO(&rfds);
            FD_SET(server_fd, &rfds);
            int r = select(server_fd + 1, &rfds, NULL, NULL, &tv);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (r == 0) {
                /* Sin cliente: drenar VENC (H265) para no acumular backlog.
                   Usar g_mode (no cur_mode): cur_mode arranca en -1 y solo se
                   actualiza al conectar un cliente, asi que si no drenamos por
                   g_mode el buffer VENC se llena y el pipeline se cuelga. */
                if (g_mode == MODE_H265) {
                    for (int n = 0; n < 64; n++) {
                        ot_venc_chn_status status;
                        memset(&status, 0, sizeof(status));
                        if (ss_mpi_venc_query_status(VENC_CHN, &status) != TD_SUCCESS ||
                            status.cur_packs == 0) break;
                        ot_venc_stream stream;
                        memset(&stream, 0, sizeof(stream));
                        stream.pack = calloc(status.cur_packs, sizeof(*stream.pack));
                        if (!stream.pack) break;
                        stream.pack_cnt = status.cur_packs;
                        if (ss_mpi_venc_get_stream(VENC_CHN, &stream, 100) == TD_SUCCESS)
                            (void)ss_mpi_venc_release_stream(VENC_CHN, &stream);
                        free(stream.pack);
                    }
                }
                if (g_stop) break;
                continue;
            }
            client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
            break;
        }
        if (client_fd < 0) {
            if (g_stop) break;
            continue;
        }

        fprintf(stderr, "  Client: %s (mode %s)\n", inet_ntoa(cli_addr.sin_addr),
                g_mode == MODE_H265 ? "H265" : "RAW");
        fflush(stderr);

        int h265_wait_idr = 0;
        if (g_mode == MODE_H265 && cur_mode == MODE_H265) {
            h265_wait_idr = 1;
            (void)ss_mpi_venc_request_idr(VENC_CHN, TD_TRUE);
        }

        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        char cmd_buf[512];
        int cmd_len = 0;
        int consecutive_fail = 0;
        g_burst_remaining = 0;

        while (g_stop == 0) {
            if (g_kick_client) {
                g_kick_client = 0;
                fprintf(stderr, "  [data] kick cliente por switch de modo\n");
                fflush(stderr);
                close(client_fd);
                client_fd = -1;
                break;
            }
            if (cur_mode != g_mode) {
                if (cur_mode == MODE_H265) venc_fd = -1;
                if (g_mode == MODE_H265) {
                    venc_fd = ss_mpi_venc_get_fd(VENC_CHN);
                    fprintf(stderr, "  [data] venc_fd=%d (H265)\n", venc_fd);
                    h265_wait_idr = 1;
                    (void)ss_mpi_venc_request_idr(VENC_CHN, TD_TRUE);
                } else {
                    if (mem_fd < 0) mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
                    fprintf(stderr, "  [data] mem_fd=%d (RAW)\n", mem_fd);
                }
                cur_mode = g_mode;
                consecutive_fail = 0;
                fflush(stderr);
            }

            if (g_mode == MODE_H265) {
                if (g_switching) { usleep(50000); continue; }
                if (venc_fd < 0) { usleep(50000); continue; }
                fd_set rfds;
                struct timeval tv = { 1, 0 };
                FD_ZERO(&rfds);
                FD_SET(venc_fd, &rfds);
                static int dbg_n = 0;
                int sr = select(venc_fd + 1, &rfds, NULL, NULL, &tv);
                if (sr <= 0) {
                    if (g_stop) break;
                    if (dbg_n < 5) { fprintf(stderr, "  [dbgH] select=%d venc_fd=%d\n", sr, venc_fd); dbg_n++; }
                    continue;
                }
                if (dbg_n < 5) { fprintf(stderr, "  [dbgH] select=%d (frame ready)\n", sr); dbg_n++; }

                ot_venc_chn_status status;
                ot_venc_stream stream;
                memset(&status, 0, sizeof(status));
                if (ss_mpi_venc_query_status(VENC_CHN, &status) != TD_SUCCESS ||
                    status.cur_packs == 0) {
                    if (dbg_n < 8) { fprintf(stderr, "  [dbgH] query fail cur_packs=%d\n", status.cur_packs); dbg_n++; }
                    continue;
                }
                memset(&stream, 0, sizeof(stream));
                stream.pack = calloc(status.cur_packs, sizeof(*stream.pack));
                if (!stream.pack) break;
                stream.pack_cnt = status.cur_packs;
                if (ss_mpi_venc_get_stream(VENC_CHN, &stream, 1000) != TD_SUCCESS) {
                    free(stream.pack);
                    continue;
                }

                size_t total = 0;
                int i;
                for (i = 0; i < (int)stream.pack_cnt; i++) total += stream.pack[i].len;
                if (total == 0) {
                    ss_mpi_venc_release_stream(VENC_CHN, &stream);
                    free(stream.pack);
                    continue;
                }
                uint8_t *frame = malloc(total);
                if (!frame) {
                    ss_mpi_venc_release_stream(VENC_CHN, &stream);
                    free(stream.pack);
                    break;
                }
                size_t off = 0;
                for (i = 0; i < (int)stream.pack_cnt; i++) {
                    memcpy(frame + off, stream.pack[i].addr, stream.pack[i].len);
                    off += stream.pack[i].len;
                }
                int is_idr = frame_is_idr(frame, total);
                g_frames++;
                g_bytes += total;
                if (is_idr) g_idr++;
                if (dbg_n < 10) { fprintf(stderr, "  [dbgH] frame total=%zu is_idr=%d wait=%d\n", total, is_idr, h265_wait_idr); dbg_n++; }

                if (h265_wait_idr && !is_idr) {
                    ss_mpi_venc_release_stream(VENC_CHN, &stream);
                    free(stream.pack);
                    free(frame);
                    continue;
                }
                h265_wait_idr = 0;

                uint8_t hdr[WB_HDR_LEN];
                hdr[0] = WB_MAGIC0; hdr[1] = WB_MAGIC1;
                hdr[2] = (uint8_t)(is_idr ? 1 : 0); hdr[3] = 0;
                hdr[4] = (uint8_t)(total & 0xFF);
                hdr[5] = (uint8_t)((total >> 8) & 0xFF);
                hdr[6] = (uint8_t)((total >> 16) & 0xFF);
                hdr[7] = (uint8_t)((total >> 24) & 0xFF);
                uint64_t pts = stream.pack_cnt ? stream.pack[0].pts : 0;
                hdr[8] = (uint8_t)(pts & 0xFF);
                hdr[9] = (uint8_t)((pts >> 8) & 0xFF);
                hdr[10] = (uint8_t)((pts >> 16) & 0xFF);
                hdr[11] = (uint8_t)((pts >> 24) & 0xFF);
                /* exposure telemetry (mirrors RAW AS header) */
                td_u32 cur_exp, cur_again, cur_dgain;
                if (!g_ae_enabled) {
                    /* manual: el sensor se escribe directo, usamos los valores mandados */
                    cur_exp   = g_man_exp_us;
                    cur_again = g_man_again;
                    cur_dgain = g_man_dgain;
                } else {
                    cur_exp = g_man_exp_us; cur_again = g_again; cur_dgain = g_dgain;
                    ot_isp_exp_info einfo;
                    if (ss_mpi_isp_query_exposure_info(VI_PIPE, &einfo) == TD_SUCCESS) {
                        cur_exp   = (td_u32)einfo.exp_time;
                        cur_again = (td_u32)einfo.a_gain;
                        cur_dgain = (td_u32)einfo.d_gain;
                    }
                }
                hdr[12] = (uint8_t)(cur_exp & 0xFF);
                hdr[13] = (uint8_t)((cur_exp >> 8) & 0xFF);
                hdr[14] = (uint8_t)((cur_exp >> 16) & 0xFF);
                hdr[15] = (uint8_t)((cur_exp >> 24) & 0xFF);
                hdr[16] = (uint8_t)(cur_again & 0xFF);
                hdr[17] = (uint8_t)((cur_again >> 8) & 0xFF);
                hdr[18] = (uint8_t)((cur_again >> 16) & 0xFF);
                hdr[19] = (uint8_t)((cur_again >> 24) & 0xFF);
                hdr[20] = (uint8_t)(cur_dgain & 0xFF);
                hdr[21] = (uint8_t)((cur_dgain >> 8) & 0xFF);
                hdr[22] = (uint8_t)((cur_dgain >> 16) & 0xFF);
                hdr[23] = (uint8_t)((cur_dgain >> 24) & 0xFF);
                hdr[24] = (uint8_t)(g_vmax & 0xFF);
                hdr[25] = (uint8_t)((g_vmax >> 8) & 0xFF);
                hdr[26] = (uint8_t)((g_vmax >> 16) & 0xFF);
                hdr[27] = (uint8_t)((g_vmax >> 24) & 0xFF);

                if (write_all(client_fd, hdr, WB_HDR_LEN) != 0 ||
                    write_all(client_fd, frame, total) != 0) {
                    fprintf(stderr, "  H265 TCP write fail\n");
                    ss_mpi_venc_release_stream(VENC_CHN, &stream);
                    free(stream.pack);
                    free(frame);
                    break;
                }
                ss_mpi_venc_release_stream(VENC_CHN, &stream);
                free(stream.pack);
                free(frame);
                continue;
            }

            /* ---------- RAW: comandos + frame bayer (header 48B "AS") ---------- */
            fd_set rfds;
            struct timeval tv = {0, 10000};
            FD_ZERO(&rfds);
            FD_SET(client_fd, &rfds);
            if (select(client_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
                char tmp[256];
                ssize_t n = recv(client_fd, tmp, sizeof(tmp) - 1, 0);
                if (n == 0) break;
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
                else if (n > 0) {
                    for (ssize_t i = 0; i < n; i++) {
                        if (tmp[i] == '\n' || tmp[i] == '\r') {
                            if (cmd_len > 0) {
                                cmd_buf[cmd_len] = '\0';
                                process_command(cmd_buf);
                                cmd_len = 0;
                            }
                        } else if (cmd_len < (int)sizeof(cmd_buf) - 1) {
                            cmd_buf[cmd_len++] = tmp[i];
                        }
                    }
                }
            }

            td_u32 timeout_ms = (td_u32)lines_to_us(g_vmax) / 1000 * 2 + 2000;
            if (timeout_ms < 3000) timeout_ms = 3000;

            ot_video_frame_info frame_info;
            memset(&frame_info, 0, sizeof(frame_info));
            ret = ss_mpi_vi_get_chn_frame(VI_PIPE, VI_CHN, &frame_info, timeout_ms);
            if (ret != TD_SUCCESS) {
                if (g_switching) { usleep(50000); continue; }
                consecutive_fail++;
                fprintf(stderr, "  [DBG] get_chn_frame fail 0x%x (fail=%d)\n",
                        ret, consecutive_fail);
                fflush(stderr);
                if (consecutive_fail > 3) break;
                continue;
            }
            consecutive_fail = 0;

            const ot_video_frame *vf = &frame_info.video_frame;
            if ((frame_index % 15) == 0) {
                fprintf(stderr, "  [DBG] frame idx=%u w=%u h=%u stride=%u phys=0x%llx\n",
                        frame_index, vf->width, vf->height, vf->stride[0],
                        (unsigned long long)vf->phys_addr[0]);
                fflush(stderr);
            }

            td_u32 stride = vf->stride[0];
            td_u32 height = vf->height;
            td_u64 phys = vf->phys_addr[0];
            td_u64 map_size = (td_u64)stride * height;
            td_u64 page_phys = phys & ~0xFFFULL;
            td_u64 page_off = phys - page_phys;
            td_u64 map_len = ((map_size + page_off + 0xFFF) & ~0xFFFULL);

            if (!mem_mmaped || mem_map_phys != page_phys || mem_map_len != map_len) {
                if (mem_mmaped) munmap(mem_map, mem_map_len);
                mem_map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, mem_fd, page_phys);
                if (mem_map == MAP_FAILED) {
                    ss_mpi_vi_release_chn_frame(VI_PIPE, VI_CHN, &frame_info);
                    break;
                }
                mem_map_phys = page_phys;
                mem_map_len = map_len;
                mem_mmaped = 1;
            }

            uint8_t *src = (uint8_t *)mem_map + page_off;

            if (g_save_next) {
                write_fits(g_save_filename, src, vf->width, height, stride,
                           g_exposure_lines, g_again, g_dgain);
                g_save_next = 0;
            }

            uint8_t cap_flag = (g_burst_remaining > 0) ? 1 : 0;
            if (g_burst_remaining > 0) g_burst_remaining--;

            if (send_as_header(client_fd, (uint32_t)vf->width, height, stride,
                               (uint32_t)map_size, frame_index++, now_us(),
                               (uint32_t)lines_to_us(g_exposure_lines),
                               g_again, g_dgain, g_vmax,
                               get_temp_cached(), cap_flag) < 0) {
                ss_mpi_vi_release_chn_frame(VI_PIPE, VI_CHN, &frame_info);
                break;
            }
            if (write_all(client_fd, src, (size_t)map_size) < 0) {
                ss_mpi_vi_release_chn_frame(VI_PIPE, VI_CHN, &frame_info);
                break;
            }

            ss_mpi_vi_release_chn_frame(VI_PIPE, VI_CHN, &frame_info);
            if ((frame_index % 30) == 0) {
                fprintf(stderr, "  sent %u frames | E=%u VMAX=%u A=%u D=%u\n",
                        frame_index, g_exposure_lines, g_vmax, g_again, g_dgain);
                fflush(stderr);
            }
        }

        fprintf(stderr, "  Disconnected\n");
        fflush(stderr);
        close(client_fd);
    }

    if (mem_mmaped) munmap(mem_map, mem_map_len);
    if (mem_fd >= 0) close(mem_fd);
    fprintf(stderr, "[data_loop] EXIT g_stop=%d\n", (int)g_stop);
    fflush(stderr);
}

/* -------------------------------------------------------- control AE -- */

static void ae_apply(void)
{
    /* En RAW (ISP bypass) no hay AE: process_command escribe el sensor directo.
       En H265, el AE auto pisa la ganancia; en manual lo deshabilitamos y
       escribimos el sensor directo (igual que RAW) para que A/D/T apliquen. */
    if (g_mode == MODE_H265) {
        if (g_manual) {
            if (g_ae_enabled) {
                ss_mpi_ae_unregister(VI_PIPE, &g_ae_lib);
                g_ae_enabled = 0;
                fprintf(stderr, "[ctrl] AE deshabilitado (manual)\n");
            }
            sensor_set_exposure_us(g_man_exp_us);
            sensor_set_gain(g_man_again, g_man_dgain);
            fprintf(stderr, "[ctrl] MANUAL exp=%uus again=%u dgain=%u\n",
                    g_man_exp_us, g_man_again, g_man_dgain);
        } else {
            if (!g_ae_enabled) {
                ss_mpi_ae_register(VI_PIPE, &g_ae_lib);
                g_sns_obj->pfn_register_callback(VI_PIPE, &g_ae_lib,
                                                 &g_awb_lib);
                g_ae_enabled = 1;
                fprintf(stderr, "[ctrl] AE rehabilitado (auto)\n");
            }
            ot_isp_exposure_attr attr;
            if (ss_mpi_isp_get_exposure_attr(VI_PIPE, &attr) == TD_SUCCESS) {
                attr.op_type = OT_OP_MODE_AUTO;
                attr.bypass = TD_FALSE;
                ss_mpi_isp_set_exposure_attr(VI_PIPE, &attr);
            }
            fprintf(stderr, "[ctrl] AUTO exp=%uus again=%u dgain=%u\n",
                    g_man_exp_us, g_man_again, g_man_dgain);
        }
        return;
    }
    /* RAW: dejar que process_command maneje el sensor (no hacer nada aca) */
}

static void ctrl_send(int cfd, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n >= (int)sizeof(tmp))
            n = (int)sizeof(tmp) - 1;
        (void)write(cfd, tmp, (size_t)n);
    }
}

static void ctrl_print_status(int cfd)
{
    ot_isp_exp_info info;

    memset(&info, 0, sizeof(info));
    if (ss_mpi_isp_query_exposure_info(VI_PIPE, &info) == TD_SUCCESS) {
        ctrl_send(cfd, "exp=%uus again=%u(%.2fx) dgain=%u(%.2fx) "
                "fps=%u iso=%u mode=%s\n",
                info.exp_time, info.a_gain, info.a_gain / 1024.0,
                info.d_gain, info.d_gain / 1024.0, info.fps, info.iso,
                g_manual ? "MANUAL" : "AUTO");
    } else {
        ctrl_send(cfd, "query_exposure_info fail\n");
    }
}

static void ctrl_handle_client(int cfd)
{
    char buf[256];
    size_t used = 0;

    ctrl_send(cfd, "MODE raw|h265 | E <us> | A <again> | D <dgain> | "
            "T auto | R status | X <reghex> | ? ayuda\n");
    while (!g_stop) {
        ssize_t n = read(cfd, buf + used, sizeof(buf) - 1 - used);
        if (n <= 0)
            break;
        used += (size_t)n;
        char *p;
        while (used > 0 && (p = memchr(buf, '\n', used)) != NULL) {
            size_t len = (size_t)(p - buf);
            if (len > 0 && buf[len - 1] == '\r')
                len--;
            if (len > 0) {
                buf[len] = 0;
                char *line = buf;
                if (g_mode == MODE_H265 &&
                    (line[0] == 'E' || line[0] == 'A' || line[0] == 'D' ||
                     line[0] == 'T')) {
                    if (line[0] == 'E' && line[1] == ' ') {
                        g_manual = 1;
                        g_man_exp_us = (td_u32)atoi(line + 2);
                        if (g_man_exp_us < 100)
                            g_man_exp_us = 100;
                        if (g_man_exp_us > g_max_exp_us)
                            g_man_exp_us = g_max_exp_us;
                        ae_apply();
                        ctrl_print_status(cfd);
                    } else if (line[0] == 'A' && line[1] == ' ') {
                        g_manual = 1;
                        g_man_again = (td_u32)atoi(line + 2);
                        if (g_man_again < 1024)
                            g_man_again = 1024;
                        if (g_man_again > 32768)
                            g_man_again = 32768;
                        ae_apply();
                        ctrl_print_status(cfd);
                    } else if (line[0] == 'D' && line[1] == ' ') {
                        g_manual = 1;
                        g_man_dgain = (td_u32)atoi(line + 2);
                        if (g_man_dgain < 1024)
                            g_man_dgain = 1024;
                        if (g_man_dgain > 16384)
                            g_man_dgain = 16384;
                        ae_apply();
                        ctrl_print_status(cfd);
                    } else if (line[0] == 'T' && strncmp(line, "T auto", 6) == 0) {
                        g_manual = 0;
                        ae_apply();
                        ctrl_print_status(cfd);
                    } else if (line[0] == 'T' && line[1] == ' ') {
                        g_manual = 1;
                        g_man_exp_us = (td_u32)atoi(line + 2);
                        if (g_man_exp_us < 100)
                            g_man_exp_us = 100;
                        if (g_man_exp_us > g_max_exp_us)
                            g_man_exp_us = g_max_exp_us;
                        ae_apply();
                        ctrl_print_status(cfd);
                    }
                } else if (strncmp(line, "MODE ", 5) == 0) {
                    int want = (!strncmp(line + 5, "h265", 4)) ? MODE_H265 : MODE_RAW;
                    if (switch_mode(want, &g_cfg) != 0)
                        ctrl_send(cfd, "MODE switch falló\n");
                } else if (line[0] == 'R') {
                    if (g_mode == MODE_H265)
                        ctrl_print_status(cfd);
                    else
                        process_command(line);
                } else if (line[0] == 'X' && line[1] == ' ') {
                    unsigned long r = strtoul(line + 2, NULL, 16);
                    if (r <= 0xFFFF) {
                        td_u8 b0 = i2c_read_reg((td_u16)r);
                        td_u8 b1 = i2c_read_reg((td_u16)(r + 1));
                        td_u8 b2 = i2c_read_reg((td_u16)(r + 2));
                        ctrl_send(cfd, "reg 0x%04lX = %02X %02X %02X\n",
                                r, b0, b1, b2);
                    }
                } else if (line[0] == 'W' && line[1] == ' ') {
                    unsigned long r = strtoul(line + 2, NULL, 16);
                    unsigned long v = strtoul(line + 2 + strcspn(line + 2, " ") + 1, NULL, 16);
                    int rc = i2c_write_reg((td_u16)r, (td_u8)v);
                    ctrl_send(cfd, "W 0x%04lX := 0x%02lX  rc=%d\n", r, v, rc);
                } else if (line[0] == '?') {
                    ctrl_send(cfd, "MODE raw|h265 | E <us> | A <again> 22.10 | "
                            "D <dgain> 22.10 | T auto | R status | X <reghex>\n");
                } else {
                    /* modo RAW: comandos del socket de datos (E/T/M/A/D/I/V/P/S/B/F/C/G) */
                    process_command(line);
                }
            }
            used -= len + 1;
            memmove(buf, p + 1, used);
        }
        if (used == sizeof(buf) - 1)
            used = 0;
    }
}

static void *ctrl_thread_fn(void *arg)
{
    int port = *(int *)arg;
    int sock = tcp_listen(port);

    if (sock < 0) {
        fprintf(stderr, "[ctrl] FAIL listen %d\n", port);
        return NULL;
    }
    fprintf(stderr, "[ctrl] servidor control en puerto %d\n", port);
    while (!g_stop) {
        fd_set rfds;
        struct timeval tv = { 1, 0 };
        int r;

        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        r = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR)
            continue;
        if (r > 0) {
            int cfd = accept(sock, NULL, NULL);
            if (cfd >= 0) {
                fprintf(stderr, "[ctrl] cliente conectado\n");
                ctrl_handle_client(cfd);
                close(cfd);
                fprintf(stderr, "[ctrl] cliente desconectado\n");
            }
        }
    }
    close(sock);
    return NULL;
}

/* -------------------------------------------------------------- teardown -- */

static void vi_teardown(void)
{
    ss_mpi_vi_disable_chn(VI_PIPE, VI_CHN);
    ss_mpi_vi_stop_pipe(VI_PIPE);
    ss_mpi_vi_destroy_pipe(VI_PIPE);
    ss_mpi_vi_unbind(VI_DEV, VI_PIPE);
    ss_mpi_vi_disable_dev(VI_DEV);
    usleep(200000);
}

static void h265_teardown(void)
{
    ot_mpp_chn src = { OT_ID_VI, VI_PIPE, VI_CHN };
    ot_mpp_chn dst = { OT_ID_VPSS, VPSS_GRP, 0 };
    ot_mpp_chn vsrc = { OT_ID_VPSS, VPSS_GRP, VPSS_CHN };
    ot_mpp_chn vdst = { OT_ID_VENC, 0, VENC_CHN };

    (void)ss_mpi_venc_stop_chn(VENC_CHN);
    (void)ss_mpi_venc_destroy_chn(VENC_CHN);
    (void)ss_mpi_sys_unbind(&vsrc, &vdst);
    (void)ss_mpi_sys_unbind(&src, &dst);
    (void)ss_mpi_vpss_stop_grp(VPSS_GRP);
    (void)ss_mpi_vpss_disable_chn(VPSS_GRP, VPSS_CHN);
    (void)ss_mpi_vpss_destroy_grp(VPSS_GRP);
    ss_mpi_isp_exit(VI_PIPE);
    if (g_isp_thread_ok) {
        pthread_join(g_isp_thread, NULL);
        g_isp_thread_ok = 0;
    }
    if (g_sns_obj != NULL && g_sns_obj->pfn_un_register_callback != NULL)
        g_sns_obj->pfn_un_register_callback(VI_PIPE, &g_ae_lib, &g_awb_lib);
    ss_mpi_awb_unregister(VI_PIPE, &g_awb_lib);
    ss_mpi_ae_unregister(VI_PIPE, &g_ae_lib);
    if (g_sns_handle != NULL) {
        dlclose(g_sns_handle);
        g_sns_handle = NULL;
        g_sns_obj = NULL;
    }
    usleep(200000);
}

/* Cambio de modo raw <-> h265 en runtime.  Teardown del pipeline actual y
 * rebuild en el otro.  El sensor/MCLK/MIPI no se tocan.  El VI se
 * reconstruye con la config del pipe (bypass vs ISP activo). */
static int switch_mode(int new_mode, const TestConfig *c)
{
    if (new_mode == g_mode) {
        fprintf(stderr, "[ctrl] MODE ya es %s\n",
                g_mode == MODE_H265 ? "H265" : "RAW");
        return 0;
    }

    fprintf(stderr, "[ctrl] switch a modo %s...\n",
            new_mode == MODE_H265 ? "H265" : "RAW");
    fflush(stderr);
    g_switching = 1;

    if (g_mode == MODE_H265) {
        h265_teardown();
        vi_teardown();
    } else {
        vi_teardown();
    }

    if (new_mode == MODE_H265) {
        if (vi_setup(c) != 0 || vi_start_chn(c) != 0 ||
            sensor_setup(c->i2c_bus) != 0 || isp_setup(c) != 0 ||
            configure_output_color() != 0)
            goto fail;
        if (sensor_override(c) != 0)
            goto fail;
        if (vpss_setup(c) != 0)
            goto fail;
        if (pthread_create(&g_isp_thread, NULL, isp_thread_fn, NULL) != 0)
            goto fail;
        g_isp_thread_ok = 1;
        (void)enable_sensor_ccm();
        if (venc_start(c) != 0)
            goto fail;
    } else {
        if (vi_setup_raw(c) != 0)
            goto fail;
        if (init_sensor_full(c) != 0)
            goto fail;
    }
    g_mode = new_mode;
    g_switching = 0;
    g_kick_client = 1;   /* fuerza reconnect limpio del viewer (evita desync) */
    fprintf(stderr, "[ctrl] modo activo: %s\n",
            g_mode == MODE_H265 ? "H265" : "RAW");
    return 0;
fail:
    fprintf(stderr, "[ctrl] switch a %s FAIL — estado inconsistente\n",
            new_mode == MODE_H265 ? "H265" : "RAW");
    g_switching = 0;
    return -1;
}

static void vpss_teardown(void)
{
    ot_mpp_chn src = { OT_ID_VI, VI_PIPE, VI_CHN };
    ot_mpp_chn dst = { OT_ID_VPSS, VPSS_GRP, 0 };
    ot_mpp_chn vsrc = { OT_ID_VPSS, VPSS_GRP, VPSS_CHN };
    ot_mpp_chn vdst = { OT_ID_VENC, 0, VENC_CHN };

    (void)ss_mpi_sys_unbind(&vsrc, &vdst);
    (void)ss_mpi_sys_unbind(&src, &dst);
    (void)ss_mpi_vpss_stop_grp(VPSS_GRP);
    (void)ss_mpi_vpss_disable_chn(VPSS_GRP, VPSS_CHN);
    (void)ss_mpi_vpss_destroy_grp(VPSS_GRP);
}

static void mpp_cleanup(void)
{
    td_s32 sys_ret, vb_ret;

    printf("== teardown ==\n");
    (void)ss_mpi_venc_stop_chn(VENC_CHN);
    (void)ss_mpi_venc_destroy_chn(VENC_CHN);
    vpss_teardown();
    ss_mpi_isp_exit(VI_PIPE);
    if (g_isp_thread_ok) {
        pthread_join(g_isp_thread, NULL);
        g_isp_thread_ok = 0;
    }
    if (g_sns_obj != NULL && g_sns_obj->pfn_un_register_callback != NULL)
        g_sns_obj->pfn_un_register_callback(VI_PIPE, &g_ae_lib, &g_awb_lib);
    ss_mpi_awb_unregister(VI_PIPE, &g_awb_lib);
    ss_mpi_ae_unregister(VI_PIPE, &g_ae_lib);
    ss_mpi_vi_disable_chn(VI_PIPE, VI_CHN);
    ss_mpi_vi_stop_pipe(VI_PIPE);
    ss_mpi_vi_destroy_pipe(VI_PIPE);
    ss_mpi_vi_unbind(VI_DEV, VI_PIPE);
    ss_mpi_vi_disable_dev(VI_DEV);
    if (g_sns_handle != NULL) {
        dlclose(g_sns_handle);
        g_sns_handle = NULL;
        g_sns_obj = NULL;
    }
    if (g_i2c_fd >= 0) {
        close(g_i2c_fd);
        g_i2c_fd = -1;
    }
    sys_ret = ss_mpi_sys_exit();
    vb_ret = ss_mpi_vb_exit();
    printf("  sys_exit=0x%x vb_exit=0x%x\n", sys_ret, vb_ret);
}

/* ---------------------------------------------------------------- start -- */

/* ------------------------------------------------------------------ main -- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [flags]\n"
        "  --preset waybeam|validated   config base (default waybeam)\n"
        "  --lanes N                    1..4 (MIPI RX + sensor LANEMODE)\n"
        "  --mclk-hz HZ                 MCLK sensor (27000000|37125000, 0=no tocar)\n"
        "  --incksel N                  override 0x3014 (-1=lib)\n"
        "  --datarate N                 override 0x3015 (-1=lib)\n"
        "  --lanemode N                 override 0x3040 (-1=lib)\n"
        "  --vmax N                     override VMAX (0=lib)\n"
        "  --hmax N                     override HMAX (0=lib)\n"
        "  --bitrate K                  kbps H265 (default 4000)\n"
        "  --gop S                      segundos GOP (default 2)\n"
        "  --out-w W --out-h H          salida VPSS (default = captura)\n"
        "  --mode raw|h265              pipeline inicial (default h265)\n"
        "  --offline                    VI offline (default: SI, online NO soportado)\n"
        "  --port P                     TCP salida (default 5999, 0=bench)\n"
        "  --ctrl-port P                TCP control AE (default 5998, 0=off)\n"
        "  --bench S                    medir fps S seg y salir limpio\n"
        "  --raw-blk/--yuv-blk/--out-blk N  conteos VB\n"
        "  --no-sys-clean               no llamar sys_exit/vb_exit pre-init\n"
        "  --i2c-bus N                  bus I2C (default 0)\n"
        "\n"
        "Ejemplos:\n"
        "  %s --lanes 4 --mclk-hz 37125000 --incksel 0x01 --datarate 0x03 --lanemode 0x03\n"
        "  %s --preset validated\n"
        "  %s --lanes 4 --bench 10\n",
        prog, prog, prog, prog);
}

static int parse_hex_or_dec(const char *s)
{
    return (int)strtol(s, NULL, 0);
}

int main(int argc, char **argv)
{
    TestConfig cfg;
    int sock = -1;
    int i;

    memset(&cfg, 0, sizeof(cfg));
    /* defaults: waybeam 1080p30 RAW12, 4 lanes, 37.125 MHz */
    cfg.width = 1920;
    cfg.height = 1080;
    cfg.fps = 30.0f;
    cfg.lanes = 4;
    cfg.data_rate_x2 = 0;
    cfg.bayer = 0;
    cfg.raw_bit = 12;
    cfg.sensor_clock_hz = 37125000;
    cfg.out_width = 1920;
    cfg.out_height = 1080;
    cfg.vi_online = 0;           /* VI-online NO soportado (NOT_PERM) */
    cfg.i2c_bus = 0;
    cfg.incksel = 0x01;
    cfg.datarate = 0x03;
    cfg.lanemode = 0x03;
    cfg.mode = MODE_H265;        /* default: pipeline H265 probado */
    cfg.vmax = 0;
    cfg.hmax = 0;
    cfg.bitrate_kbps = 4000;
    cfg.gop_sec = 2;
    cfg.port = 5999;
    cfg.ctrl_port = 5998;
    cfg.bench_sec = 0;
    cfg.blk_raw = 0;
    cfg.blk_yuv = 0;
    cfg.blk_out = 0;
    cfg.no_sys_clean = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--preset")) {
            if (i + 1 >= argc)
                goto bad;
            if (!strcmp(argv[i + 1], "validated")) {
                /* config validada del usuario: 1 lane, 27 MHz */
                cfg.lanes = 1;
                cfg.sensor_clock_hz = 27000000;
                cfg.incksel = 0x03;
                cfg.datarate = 0x05;
                cfg.lanemode = 0x00;
                i++;
            } else if (!strcmp(argv[i + 1], "waybeam")) {
                /* defaults ya son waybeam */
                i++;
            } else {
                goto bad;
            }
        } else if (!strcmp(argv[i], "--lanes")) {
            cfg.lanes = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--mclk-hz")) {
            cfg.sensor_clock_hz = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--incksel")) {
            cfg.incksel = parse_hex_or_dec(argv[++i]);
        } else if (!strcmp(argv[i], "--datarate")) {
            cfg.datarate = parse_hex_or_dec(argv[++i]);
        } else if (!strcmp(argv[i], "--lanemode")) {
            cfg.lanemode = parse_hex_or_dec(argv[++i]);
        } else if (!strcmp(argv[i], "--vmax")) {
            cfg.vmax = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--hmax")) {
            cfg.hmax = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--bitrate")) {
            cfg.bitrate_kbps = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--gop")) {
            cfg.gop_sec = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--out-w")) {
            cfg.out_width = (unsigned int)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--out-h")) {
            cfg.out_height = (unsigned int)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--mode")) {
            if (i + 1 >= argc)
                goto bad;
            if (!strncmp(argv[i + 1], "raw", 3))
                cfg.mode = MODE_RAW;
            else
                cfg.mode = MODE_H265;
            i++;
        } else if (!strcmp(argv[i], "--offline")) {
            cfg.vi_online = 0;
        } else if (!strcmp(argv[i], "--port")) {
            cfg.port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--ctrl-port")) {
            cfg.ctrl_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--bench")) {
            cfg.bench_sec = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--raw-blk")) {
            cfg.blk_raw = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--yuv-blk")) {
            cfg.blk_yuv = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--out-blk")) {
            cfg.blk_out = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-sys-clean")) {
            cfg.no_sys_clean = 1;
        } else if (!strcmp(argv[i], "--i2c-bus")) {
            cfg.i2c_bus = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            goto bad;
        }
    }

    if (cfg.lanes < 1 || cfg.lanes > 4) {
        fprintf(stderr, "lanes debe ser 1..4\n");
        return 1;
    }
    if (cfg.out_width == 0 || cfg.out_height == 0) {
        cfg.out_width = cfg.width;
        cfg.out_height = cfg.height;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    printf("== config ==\n");
    printf("  %ux%u@%ufps raw%d, %d lanes, MCLK=%u Hz, out %ux%u, %s, "
           "VENC %u kbps\n", cfg.width, cfg.height, (unsigned int)cfg.fps,
           cfg.raw_bit, cfg.lanes, cfg.sensor_clock_hz, cfg.out_width,
           cfg.out_height, cfg.vi_online ? "VI-online" : "VI-offline",
           cfg.bitrate_kbps);
    g_max_exp_us = (cfg.fps > 0) ?
                   (td_u32)(1000000.0 / cfg.fps * 0.9) : 33333;
    g_cfg = cfg;               /* copia para switch_mode() */
    g_mode = cfg.mode;

    printf("== pipeline (%s) ==\n", g_mode == MODE_H265 ? "H265" : "RAW");
    if (sys_setup(&cfg) != 0) {
        fprintf(stderr, "== pipeline FAIL ==\n");
        mpp_cleanup();
        return 1;
    }
    if (mipi_setup(&cfg) != 0) {
        fprintf(stderr, "== pipeline FAIL ==\n");
        mpp_cleanup();
        return 1;
    }
    if (g_mode == MODE_H265) {
        if (vi_setup(&cfg) != 0 || vi_start_chn(&cfg) != 0) {
            fprintf(stderr, "== pipeline FAIL ==\n");
            mpp_cleanup();
            return 1;
        }
        printf("== sensor/isp ==\n");
        if (i2c_open() != 0) {
            fprintf(stderr, "== pipeline FAIL ==\n");
            mpp_cleanup();
            return 1;
        }
        if (sensor_setup(cfg.i2c_bus) != 0 || isp_setup(&cfg) != 0 ||
            configure_output_color() != 0) {
            fprintf(stderr, "== pipeline FAIL ==\n");
            mpp_cleanup();
            return 1;
        }
        printf("== sensor override (lanes/clock) ==\n");
        if (sensor_override(&cfg) != 0) {
            fprintf(stderr, "== pipeline FAIL ==\n");
            mpp_cleanup();
            return 1;
        }
        printf("== vpss ==\n");
        if (vpss_setup(&cfg) != 0) {
            fprintf(stderr, "== pipeline FAIL ==\n");
            mpp_cleanup();
            return 1;
        }
        if (pthread_create(&g_isp_thread, NULL, isp_thread_fn, NULL) != 0) {
            fprintf(stderr, "FAIL pthread_create ISP\n");
            mpp_cleanup();
            return 1;
        }
        g_isp_thread_ok = 1;
        if (enable_sensor_ccm() != 0) {
            fprintf(stderr, "== pipeline FAIL ==\n");
            mpp_cleanup();
            return 1;
        }
        if (venc_start(&cfg) != 0) {
            fprintf(stderr, "== venc FAIL ==\n");
            mpp_cleanup();
            return 1;
        }
    } else {
        /* RAW: VI con ISP bypass + init del sensor MANUAL (init_sensor_full),
           IGUAL que astro_streamer (que produce imagen real, golden_sets).
           El init via lib (isp_setup) da frame blanco f0 0f ff en este MPP,
           por eso se usa el init manual verificado de astro. */
        if (vi_setup_raw(&cfg) != 0) {
            fprintf(stderr, "== pipeline FAIL ==\n");
            mpp_cleanup();
            return 1;
        }
        printf("== sensor i2c ==\n");
        if (i2c_open() != 0) {
            fprintf(stderr, "== pipeline FAIL ==\n");
            mpp_cleanup();
            return 1;
        }
        /* Segundo reset del sensor con MCLK habilitado (clock ON durante el
           pulso de reset) — replica de cmos_isp_init del lib.  El reset
           clock-OFF de mipi_setup NO deja al sensor respondiendo al I2C
           (NACK en este MPP); solo el clock-ON hace ACK. */
        sensor_mclk_reset();
        if (init_sensor_full(&cfg) != 0) {
            fprintf(stderr, "== pipeline FAIL ==\n");
            mpp_cleanup();
            return 1;
        }
        printf("  ok  RAW pipeline: %ux%u raw%d\n", cfg.width, cfg.height,
               cfg.raw_bit);
    }
    if (cfg.ctrl_port > 0 && cfg.bench_sec == 0) {
        g_ctrl_thread_arg = cfg.ctrl_port;
        if (pthread_create(&g_ctrl_thread, NULL, ctrl_thread_fn,
                           &g_ctrl_thread_arg) != 0) {
            fprintf(stderr, "FAIL pthread_create CTRL\n");
            mpp_cleanup();
            return 1;
        }
    }
    if (cfg.port > 0) {
        if (cfg.bench_sec > 0) {
            fprintf(stderr, "[bench] --bench dado, port=%d ignorado (bench puro)\n",
                    cfg.port);
            cfg.port = 0;
        } else {
            sock = tcp_listen(cfg.port);
            if (sock < 0) {
                fprintf(stderr, "FAIL tcp listen %d\n", cfg.port);
                mpp_cleanup();
                return 1;
            }
        }
    }

    data_loop(&cfg, sock);
    fprintf(stderr, "[main] data_loop retorno, g_stop=%d\n", (int)g_stop);
    fflush(stderr);
    if (sock >= 0)
        close(sock);
    if (cfg.ctrl_port > 0 && cfg.bench_sec == 0)
        pthread_join(g_ctrl_thread, NULL);
    printf("== done: frames=%llu bytes=%llu idr=%llu ==\n",
           (unsigned long long)g_frames, (unsigned long long)g_bytes,
           (unsigned long long)g_idr);
    mpp_cleanup();
    return 0;
bad:
    usage(argv[0]);
    return 1;
}