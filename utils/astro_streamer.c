/*
 * astro_streamer.c - IMX662 raw12 streamer with astrophotography controls.
 *
 * Extends vi_raw_capture.c with:
 *   - Long exposure via VMAX extension (up to ~28 s at 1080p)
 *   - Exposure in microseconds/milliseconds (auto VMAX)
 *   - Analog/digital gain + ISO convenience
 *   - Sensor temperature readback (best-effort, 0x014A)
 *   - Frame-type tagging (LIGHT/DARK/FLAT/BIAS) for calibration
 *   - Burst capture for stacking (frames flagged for PC-side save)
 *   - Extended frame header with per-frame metadata
 *
 * Protocol (control, text newline-terminated):
 *   E <lines>      - exposure in lines (direct to 0x3050-52), clamp to VMAX
 *   T <us>         - exposure in microseconds (auto-extends VMAX)
 *   M <ms>         - exposure in milliseconds
 *   A <gain>       - analog gain (1024 = 1x, max 32768)
 *   D <gain>       - digital gain (1024 = 1x, max 16384)
 *   I <iso>        - ISO: gain = iso*1024/100, capped, analog-first
 *   V <vmax>       - set VMAX directly (frame period control)
 *   P              - preview mode: VMAX=1250 (30 fps)
 *   S [name]       - save next frame as FITS on device (RAM limited!)
 *   B <n>          - burst: flag next n frames for PC-side capture
 *   F <type>       - frame type for metadata: LIGHT|DARK|FLAT|BIAS
 *   C <object>     - object name for FITS metadata
 *   R              - read sensor temperature
 *   ?              - query status
 *
 * Data stream (port 5000): 48-byte header + raw12 payload per frame.
 *
 * Built on the exact IMX662 init that achieves 30 fps:
 *   INCK_SEL=0x03 (27 MHz) + DATARATE_SEL=0x05 (891 Mbps)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include "ot_type.h"
#include "ot_common.h"
#include "ot_common_vi.h"
#include "ot_common_vb.h"
#include "ot_common_video.h"
#include "ot_common_sys.h"
#include "ot_common_vpss.h"
#include "ot_common_venc.h"
#include "ot_common_isp.h"
#include "ot_common_3a.h"
#include "ot_mipi_rx.h"
#include "ot_i2c.h"
#include "ot_sns_ctrl.h"
#include "ot_mpi_vi.h"
#include "ot_mpi_vb.h"
#include "ot_mpi_sys.h"
#include "ss_mpi_vi.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vpss.h"
#include "ss_mpi_venc.h"
#include "ss_mpi_sys_bind.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_ae.h"
#include "ss_mpi_awb.h"
#include <dlfcn.h>
#include <pthread.h>

#define VI_DEV_ID       0
#define VI_PIPE_ID      0
#define VI_CHN_ID       0
#define IMG_WIDTH       1920
#define IMG_HEIGHT      1080
#define I2C_DEV_ADDR    0x34
#define IMG_W_STRIDE    ((IMG_WIDTH * 12 + 127) / 128 * 128 / 8)
#define RAW12_BUF_SIZE  (IMG_W_STRIDE * IMG_HEIGHT)
#define TCP_PORT        5000
#define CTRL_PORT       5998
#define HEADER_SIZE     48
#define WB_HDR_LEN      12
#define WB_MAGIC0       'W'
#define WB_MAGIC1       'B'

/* H.265 mode pipeline ids */
#define VPSS_GRP        0
#define VPSS_CHN        0
#define VENC_CHN        0
#define SNS_LIB_PATH    "/usr/lib/sensors/libsns_imx662.so"
#define SNS_OBJ_SYMBOL  "g_sns_imx662_obj"
#define IMX662_SNS_ID   662

/* Mode selectors */
enum { MODE_RAW = 0, MODE_H265 = 1 };

/* Timing (confirmed on hardware) */
#define PIXEL_RATE      74250000ULL
#define HMAX_LINES      1980
#define VMAX_DEFAULT    1250
#define VMAX_MAX        0xFFFFF
#define EXP_MAX_LINES   (VMAX_MAX - 8)
#define EXP_MIN_LINES   11

/* Exact rational line time: 1980/74.25MHz = 26.6667 us.  us*3/80 = lines */
static td_u64 us_to_lines(td_u64 us) { return (us * 3) / 80; }
static td_u64 lines_to_us(td_u64 lines) { return (lines * 80) / 3; }

/* Exposure registers (direct integration time, SDK convention).
   With g_shrconv=1 the actual SHR register is VMAX - exposure (Sony/V4L2). */
#define REG_EXP_LSB     0x3050
#define REG_EXP_MID     0x3051
#define REG_EXP_MSB     0x3052
/* VMAX registers */
#define REG_VMAX_LSB    0x3028
#define REG_VMAX_MID    0x3029
#define REG_VMAX_MSB    0x302A
/* Analog gain registers */
#define REG_AGAIN_LSB   0x306C
#define REG_AGAIN_MSB   0x306D
/* Digital gain registers */
#define REG_DGAIN_LSB   0x3070
#define REG_DGAIN_MSB   0x3071
/* Temperature (best-effort, Sony common) */
#define REG_TEMP_MSB    0x014A
#define REG_TEMP_LSB    0x014B
/* Group hold */
#define REG_GROUP_HOLD  0x3001

static int g_i2c_fd = -1;
static volatile int g_running = 1;
static volatile int g_switching = 0;   /* switch_mode() en curso: no contar fallos VI */
static int g_incksel = 0x03;
static int g_datarate = 0x05;
static int g_lanemode = 0x00;
static int g_sweep = 0;
static int g_bench = 0;
static int g_vc_num = 0;
static int g_shrconv = 1;   /* Sony convention: SHR = VMAX - exposure (V4L2 ref) */
static int g_mode = MODE_RAW;
static int g_ctrl_port = CTRL_PORT;

/* H.265 / ISP state (only used in MODE_H265) */
static void *g_sns_handle = NULL;
static ot_isp_sns_obj *g_sns_obj = NULL;
static ot_isp_3a_alg_lib g_ae_lib = { .id = VI_PIPE_ID, .lib_name = "ot_ae_lib" };
static ot_isp_3a_alg_lib g_awb_lib = { .id = VI_PIPE_ID, .lib_name = "ot_awb_lib" };
static pthread_t g_isp_thread;
static int g_isp_thread_ok = 0;
static volatile int g_manual_ae = 0;
static td_u32 g_man_exp_us = 10000;
static td_u32 g_man_again = 1024;
static td_u32 g_man_dgain = 1024;
static td_u32 g_max_exp_us = 33333;
static uint64_t g_h265_frames = 0, g_h265_bytes = 0, g_h265_idr = 0;
static pthread_t g_ctrl_thread;
static int g_ctrl_thread_arg = CTRL_PORT;
static int g_bitrate_kbps = 4000;

static td_u64 now_us(void);

static td_u32 g_exposure_lines = 4000;  /* capped to VMAX on write */
static td_u32 g_vmax = VMAX_DEFAULT;
static td_u32 g_again = 1024;
static td_u32 g_dgain = 1024;
static int g_burst_remaining = 0;
static char g_frame_type[8] = "LIGHT";
static char g_object[64] = "UNKNOWN";
static int g_save_next = 0;
static char g_save_filename[256] = "/tmp/frame.fits";

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static td_s32 i2c_write_reg(td_u16 reg, td_u8 val)
{
    unsigned char buf[3];
    buf[0] = (reg >> 8) & 0xFF;
    buf[1] = reg & 0xFF;
    buf[2] = val;
    int ret = write(g_i2c_fd, buf, 3);
    if (ret < 0) return -1;
    return 0;
}

static td_u8 i2c_read_reg(td_u16 reg)
{
    unsigned char rbuf[2] = {(reg >> 8) & 0xFF, reg & 0xFF};
    unsigned char val = 0;
    write(g_i2c_fd, rbuf, 2);
    read(g_i2c_fd, &val, 1);
    return val;
}

static void sensor_write_vmax(td_u32 vmax)
{
    if (vmax < VMAX_DEFAULT) vmax = VMAX_DEFAULT;
    if (vmax > VMAX_MAX) vmax = VMAX_MAX;
    g_vmax = vmax;
    i2c_write_reg(REG_GROUP_HOLD, 0x01);
    i2c_write_reg(REG_VMAX_LSB, vmax & 0xFF);
    i2c_write_reg(REG_VMAX_MID, (vmax >> 8) & 0xFF);
    i2c_write_reg(REG_VMAX_MSB, (vmax >> 16) & 0x0F);
    i2c_write_reg(REG_GROUP_HOLD, 0x00);
}

static void sensor_write_exposure(td_u32 exp_lines)
{
    if (exp_lines > g_vmax - 4) exp_lines = g_vmax - 4;
    if (exp_lines > EXP_MAX_LINES) exp_lines = EXP_MAX_LINES;
    if (exp_lines < EXP_MIN_LINES) exp_lines = EXP_MIN_LINES;
    g_exposure_lines = exp_lines;

    /* Sony convention (V4L2 ref): longer exposure => smaller SHR.
       SHR = VMAX - exposure.  SHR>=11 minimum per datasheet. */
    td_u32 shr = exp_lines;
    if (g_shrconv) {
        shr = (g_vmax > exp_lines) ? (g_vmax - exp_lines) : 4;
        if (shr < 11) shr = 11;
        if (shr > g_vmax - 4) shr = g_vmax - 4;
    }

    i2c_write_reg(REG_GROUP_HOLD, 0x01);
    i2c_write_reg(REG_EXP_LSB, shr & 0xFF);
    i2c_write_reg(REG_EXP_MID, (shr >> 8) & 0xFF);
    i2c_write_reg(REG_EXP_MSB, (shr >> 16) & 0x0F);
    i2c_write_reg(REG_GROUP_HOLD, 0x00);
}

/* Set exposure in lines; auto-extends VMAX so frame period fits readout. */
static void sensor_set_exposure_lines(td_u32 exp_lines)
{
    if (exp_lines < EXP_MIN_LINES) exp_lines = EXP_MIN_LINES;
    if (exp_lines > EXP_MAX_LINES) exp_lines = EXP_MAX_LINES;

    /* Frame period must fit readout: VMAX >= lines + 8 */
    if (exp_lines + 8 > g_vmax)
        sensor_write_vmax(exp_lines + 8);

    sensor_write_exposure(exp_lines);
}

/* Set exposure in microseconds; extends VMAX as needed. */
static void sensor_set_exposure_us(td_u64 us)
{
    sensor_set_exposure_lines((td_u32)us_to_lines(us));
}

static void sensor_set_gain(td_u32 again, td_u32 dgain)
{
    if (again > 32768) again = 32768;
    if (dgain > 16384) dgain = 16384;
    g_again = again;
    g_dgain = dgain;
    i2c_write_reg(REG_GROUP_HOLD, 0x01);
    i2c_write_reg(REG_AGAIN_LSB, again & 0xFF);
    i2c_write_reg(REG_AGAIN_MSB, (again >> 8) & 0xFF);
    i2c_write_reg(REG_DGAIN_LSB, dgain & 0xFF);
    i2c_write_reg(REG_DGAIN_MSB, (dgain >> 8) & 0xFF);
    i2c_write_reg(REG_GROUP_HOLD, 0x00);
}

/* ISO: 100 => gain 1x. Analog up to 32x, then digital up to 16x. */
static void sensor_set_iso(td_u32 iso)
{
    td_u32 gain = iso * 1024UL / 100;
    if (gain < 1024) gain = 1024;
    td_u32 again = gain;
    td_u32 dgain = 1024;
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
    /* Signed 16-bit, 0.01 C per unit (Sony convention, best-effort) */
    td_s32 s = (raw & 0x8000) ? (td_s32)raw - 0x10000 : (td_s32)raw;
    return s;
}

/* Cached temp: read at most every 1s to avoid hammering the I2C bus. */
static td_u64 g_last_temp_us = 0;
static td_s32 g_cached_temp = 0;
static td_s32 get_temp_cached(void)
{
    td_u64 now = now_us();
    if (now - g_last_temp_us > 1000000ULL || g_last_temp_us == 0) {
        g_cached_temp = sensor_read_temp_x100();
        g_last_temp_us = now;
    }
    return g_cached_temp;
}

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

static td_s32 init_sensor_full(void)
{
    td_s32 ok = 0, fail = 0;
    td_u32 total;

    total = sizeof(imx662_init_common) / sizeof(imx662_init_common[0]);
    fprintf(stderr, "  Phase 1: %u regs... ", total);
    fflush(stderr);
    for (td_u32 i = 0; i < total; i++) {
        if (i2c_write_reg(imx662_init_common[i].reg, imx662_init_common[i].val) == 0) ok++;
        else fail++;
        usleep(100);
    }
    fprintf(stderr, "%d ok %d fail\n", ok, fail);

    int ok2 = 0, fail2 = 0;
    total = sizeof(imx662_init_mode) / sizeof(imx662_init_mode[0]);
    fprintf(stderr, "  Phase 2: %u mode regs (in standby)... ", total);
    fflush(stderr);
    for (td_u32 i = 0; i < total; i++) {
        if (i2c_write_reg(imx662_init_mode[i].reg, imx662_init_mode[i].val) == 0) ok2++;
        else fail2++;
        usleep(100);
    }
    fprintf(stderr, "%d ok %d fail\n", ok2, fail2);

    /* INCK/DATARATE must be set while sensor is in standby (0x3000=0x01)
       so the PLL locks against the real 27 MHz MCLK, and must NEVER be
       rewritten after streaming starts (kills the MIPI TX).  The mode
       table first entry sets 0x3015=0x00, so apply the override last. */
    fprintf(stderr, "  Override (in standby): INCK_SEL=0x%02X DATARATE_SEL=0x%02X\n",
            g_incksel, g_datarate);
    i2c_write_reg(0x3014, (td_u8)g_incksel);
    i2c_write_reg(0x3015, (td_u8)g_datarate);

    fprintf(stderr, "  Phase 3: exit standby + PLL lock...\n");
    /* V4L2 ref: disable digital clamp before streaming */
    i2c_write_reg(0x3458, 0x00);
    i2c_write_reg(0x3000, 0x00);
    i2c_write_reg(0x3001, 0x00);
    usleep(25000);

    sensor_write_vmax(VMAX_DEFAULT);
    sensor_set_exposure_lines(VMAX_DEFAULT - 8);  /* 30 fps default */
    sensor_set_gain(g_again, g_dgain);

    return (fail == 0 && fail2 == 0) ? 0 : -1;
}

#define SNS0_CLK_HZ_PATH "/sys/module/open_sys_config/parameters/sns0_clk_hz"

/* Fija el MCLK del sensor via knob del kernel (parity waybeam_test).
   CRITICO tras cold boot: sin esto el MCLK queda mal y el VI recibe
   frame blanco f0 0f ff (bug 2026-08-10). MCLK de la placa = 27MHz. */
static void sensor_clock_select(unsigned int hz)
{
    char buf[16];
    int fd;

    fd = open(SNS0_CLK_HZ_PATH, O_WRONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "  WARNING: %s absent (kernel sin knob) — MCLK "
                    "queda como cargo el loader\n", SNS0_CLK_HZ_PATH);
            return;
        }
        fprintf(stderr, "  FAIL open %s: %s\n", SNS0_CLK_HZ_PATH, strerror(errno));
        return;
    }
    int len = snprintf(buf, sizeof(buf), "%u", hz);
    if (len < 0 || (size_t)len >= sizeof(buf) || write(fd, buf, (size_t)len) != len) {
        fprintf(stderr, "  FAIL set sensor clock %u Hz: %s\n", hz, strerror(errno));
    } else {
        fprintf(stderr, "  sensor clock %u Hz set\n", hz);
    }
    close(fd);
}

static td_s32 enable_mclk_and_reset_sensor(void)
{
    /* Secuencia MIPI completa, parity waybeam_test: enable_mipi_clock es
       REQUERIDO (fix bug 2026-08-10 frame blanco f0 0f ff tras reboot).
       Secuencia driver de resume: hs_mode -> disable_mipi_clk -> reset_mipi
       -> disable_sensor_clk -> reset_sensor -> set_dev_attr -> enable_mipi_clk
       -> unreset_mipi -> enable_sensor_clk -> unreset_sensor. */
    td_s32 fd;
    lane_divide_mode_t lane_mode = LANE_DIVIDE_MODE_0;
    sns_clk_source_t clk_source = 0;
    sns_rst_source_t rst_source = 0;
    combo_dev_t dev = 0;

    fd = open("/dev/ot_mipi_rx", O_RDWR);
    if (fd < 0) return -1;

    ioctl(fd, OT_MIPI_SET_HS_MODE, &lane_mode);
    ioctl(fd, OT_MIPI_DISABLE_MIPI_CLOCK, &dev);
    ioctl(fd, OT_MIPI_RESET_MIPI, &dev);
    ioctl(fd, OT_MIPI_DISABLE_SENSOR_CLOCK, &clk_source);
    ioctl(fd, OT_MIPI_RESET_SENSOR, &rst_source);
    {
        combo_dev_attr_t dev_attr;
        memset(&dev_attr, 0, sizeof(dev_attr));
        dev_attr.devno = 0;
        dev_attr.input_mode = INPUT_MODE_MIPI;
        dev_attr.data_rate = MIPI_DATA_RATE_X1;
        dev_attr.img_rect.width = IMG_WIDTH;
        dev_attr.img_rect.height = IMG_HEIGHT;
        dev_attr.mipi_attr.input_data_type = DATA_TYPE_RAW_12BIT;
        dev_attr.mipi_attr.wdr_mode = OT_MIPI_WDR_MODE_NONE;
        dev_attr.mipi_attr.lane_id[0] = 0;
        dev_attr.mipi_attr.lane_id[1] = -1;
        dev_attr.mipi_attr.lane_id[2] = -1;
        dev_attr.mipi_attr.lane_id[3] = -1;
        ioctl(fd, OT_MIPI_SET_DEV_ATTR, &dev_attr);
    }
    ioctl(fd, OT_MIPI_ENABLE_MIPI_CLOCK, &dev);
    ioctl(fd, OT_MIPI_UNRESET_MIPI, &dev);
    ioctl(fd, OT_MIPI_ENABLE_SENSOR_CLOCK, &clk_source);
    sensor_clock_select(27000000);
    ioctl(fd, OT_MIPI_UNRESET_SENSOR, &rst_source);
    /* V4L2 ref: imx662_XCLR_MIN_DELAY_US = 500000 (sensor internal
       calibration after XCLR deassert before capture). */
    usleep(500000);
    close(fd);
    return 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

/* 48-byte little-endian header: magic + metadata */
static int send_frame_header(int fd, td_u32 w, td_u32 h, td_u32 stride,
                             td_u32 size, td_u32 frame_index,
                             td_u64 ts_us, td_u32 exp_us, td_u32 again,
                             td_u32 dgain, td_u32 vmax, td_s32 temp_x100,
                             td_u8 capture_flag)
{
    unsigned char hdr[HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));
    int o = 0;
    hdr[o++] = 'A'; hdr[o++] = 'S';
    hdr[o++] = w & 0xFF; hdr[o++] = (w >> 8) & 0xFF;
    hdr[o++] = h & 0xFF; hdr[o++] = (h >> 8) & 0xFF;
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
    return send_all(fd, hdr, HEADER_SIZE);
}

static void fits_card(char *header, int *pos, const char *key, const char *val)
{
    snprintf(header + *pos, 81, "%-8s= %s", key, val);
    *pos = ((*pos / 80) + 1) * 80;
}

static int write_fits(const char *filename, const td_u8 *raw_data,
                      td_u32 width, td_u32 height, td_u32 stride,
                      td_u32 exp_lines, td_u32 again, td_u32 dgain)
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
    snprintf(card, 81, "%d", width);  fits_card(header, &pos, "NAXIS1", card);
    snprintf(card, 81, "%d", height); fits_card(header, &pos, "NAXIS2", card);
    fits_card(header, &pos, "BZERO", "0.0");
    fits_card(header, &pos, "BSCALE", "1.0");

    double exp_us = (double)lines_to_us(exp_lines);
    snprintf(card, 81, "%.6f", exp_us / 1e6);
    fits_card(header, &pos, "EXPTIME", card);

    double gain_lin = (double)again * (double)dgain / 1024.0 / 1024.0;
    snprintf(card, 81, "%.4f", gain_lin);
    fits_card(header, &pos, "GAIN", card);
    snprintf(card, 81, "%u", again);
    fits_card(header, &pos, "AGAIN", card);
    snprintf(card, 81, "%u", dgain);
    fits_card(header, &pos, "DGAIN", card);

    fits_card(header, &pos, "BAYERPAT", "'RGGB'");
    fits_card(header, &pos, "INSTRUME", "'IMX662 Hi3516CV610'");
    fits_card(header, &pos, "FRAME", g_frame_type);
    snprintf(card, 81, "'%s'", g_object);
    fits_card(header, &pos, "OBJECT", card);
    fits_card(header, &pos, "XBINNING", "1");
    fits_card(header, &pos, "YBINNING", "1");

    /* DATE-OBS: UTC ISO8601 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    gmtime_r(&ts.tv_sec, &tmv);
    char dob[32];
    strftime(dob, sizeof(dob), "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(card, 81, "'%s'", dob);
    fits_card(header, &pos, "DATE-OBS", card);

    memset(header + pos, ' ', 80);
    memcpy(header + pos, "END", 3);
    fwrite(header, 1, 2880, fp);

    for (td_u32 y = 0; y < height; y++) {
        const td_u8 *row = raw_data + (td_u64)y * stride;
        for (td_u32 x = 0; x < width; x += 2) {
            td_u32 idx = (x * 3) / 2;
            if (idx + 2 >= stride) break;
            td_u16 p0 = ((td_u16)row[idx] << 4) | ((td_u16)(row[idx + 1] >> 4) & 0x0F);
            td_u16 p1 = (((td_u16)(row[idx + 1] & 0x0F)) << 8) | (td_u16)row[idx + 2];
            td_u8 b0, b1;
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
            filename, width, height, exp_lines, (double)lines_to_us(exp_lines) / 1e6);
    return 0;
}

static td_u64 now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (td_u64)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

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
        sensor_set_exposure_lines(VMAX_DEFAULT - 8);
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
        fprintf(stderr, "  Burst: flagging next %d frames for capture\n", n);
    } else if (cmd[0] == 'F' && cmd[1] == ' ') {
        snprintf(g_frame_type, sizeof(g_frame_type), "%s", cmd + 2);
        fprintf(stderr, "  Frame type = %s\n", g_frame_type);
    } else if (cmd[0] == 'C' && cmd[1] == ' ') {
        snprintf(g_object, sizeof(g_object), "%s", cmd + 2);
        fprintf(stderr, "  Object = %s\n", g_object);
    } else if (cmd[0] == 'R') {
        /* Response generated by main loop (includes TEMP) */
    } else if (cmd[0] == 'G' && cmd[1] == ' ') {
        td_u16 reg = (td_u16)strtol(cmd + 2, NULL, 0);
        fprintf(stderr, "  Read 0x%04X = 0x%02X\n", reg, i2c_read_reg(reg));
    } else if (cmd[0] == '?') {
        fprintf(stderr, "  E=%u VMAX=%u A=%u D=%u TYPE=%s OBJ=%s MODE=%s\n",
                g_exposure_lines, g_vmax, g_again, g_dgain, g_frame_type, g_object,
                g_mode == MODE_H265 ? "H265" : "RAW");
    }
}

/* =====================================================================
 * Modo H.265: VI + ISP 3A (libsns_imx662.so) + VPSS + VENC.
 * Pipeline portado de waybeam_test.c (verificado a 30fps en device).
 * ===================================================================== */

#define CHECK(r) do { if ((r) != TD_SUCCESS) { \
    fprintf(stderr, "FAIL %s line %d: 0x%x\n", __func__, __LINE__, (r)); \
    return -1; } } while (0)

#define CHECK_OR_BUSY(r) do { \
    td_s32 _r = (r); \
    if (_r != TD_SUCCESS && (_r & 0x1fff) != OT_ERR_BUSY) { \
        fprintf(stderr, "FAIL %s line %d: 0x%x\n", __func__, __LINE__, _r); \
        return -1; } } while (0)

static void *isp_thread_fn(void *arg)
{
    (void)arg;
    td_s32 r = ss_mpi_isp_run(VI_PIPE_ID);
    fprintf(stderr, "[isp] ss_mpi_isp_run returned 0x%x\n", r);
    return NULL;
}

/* VI raw (ISP bypass). pipe isp_bypass=TRUE, chn bayer 12bpp. */
static int vi_setup_raw(void)
{
    ot_vi_dev_attr dev_attr;
    ot_vi_pipe_attr pipe_attr;
    ot_vi_chn_attr chn_attr;

    ss_mpi_vi_disable_chn(VI_PIPE_ID, VI_CHN_ID);
    ss_mpi_vi_stop_pipe(VI_PIPE_ID);
    ss_mpi_vi_destroy_pipe(VI_PIPE_ID);
    ss_mpi_vi_unbind(VI_DEV_ID, VI_PIPE_ID);
    ss_mpi_vi_disable_dev(VI_DEV_ID);
    usleep(50000);

    memset(&dev_attr, 0, sizeof(dev_attr));
    dev_attr.intf_mode = OT_VI_INTF_MODE_MIPI;
    dev_attr.work_mode = OT_VI_WORK_MODE_MULTIPLEX_1;
    dev_attr.scan_mode = OT_VI_SCAN_PROGRESSIVE;
    dev_attr.data_type = OT_VI_DATA_TYPE_RAW;
    dev_attr.in_size.width = IMG_WIDTH;
    dev_attr.in_size.height = IMG_HEIGHT;
    dev_attr.data_rate = OT_DATA_RATE_X1;
    dev_attr.component_mask[0] = 0xFFF0000;
    CHECK(ss_mpi_vi_set_dev_attr(VI_DEV_ID, &dev_attr));
    CHECK(ss_mpi_vi_enable_dev(VI_DEV_ID));
    CHECK(ss_mpi_vi_bind(VI_DEV_ID, VI_PIPE_ID));

    memset(&pipe_attr, 0, sizeof(pipe_attr));
    pipe_attr.isp_bypass = TD_TRUE;
    pipe_attr.size.width = IMG_WIDTH;
    pipe_attr.size.height = IMG_HEIGHT;
    pipe_attr.pixel_format = OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
    pipe_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    pipe_attr.frame_rate_ctrl.src_frame_rate = -1;
    pipe_attr.frame_rate_ctrl.dst_frame_rate = -1;
    CHECK(ss_mpi_vi_create_pipe(VI_PIPE_ID, &pipe_attr));
    CHECK(ss_mpi_vi_start_pipe(VI_PIPE_ID));

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.size.width = IMG_WIDTH;
    chn_attr.size.height = IMG_HEIGHT;
    chn_attr.pixel_format = OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
    chn_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    chn_attr.video_format = OT_VIDEO_FORMAT_LINEAR;
    chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    chn_attr.depth = 1;
    chn_attr.frame_rate_ctrl.src_frame_rate = -1;
    chn_attr.frame_rate_ctrl.dst_frame_rate = -1;
    CHECK(ss_mpi_vi_set_chn_attr(VI_PIPE_ID, VI_CHN_ID, &chn_attr));
    CHECK(ss_mpi_vi_enable_chn(VI_PIPE_ID, VI_CHN_ID));
    fprintf(stderr, "  VI RAW (isp_bypass) OK\n");
    return 0;
}

/* VI con ISP activo (para modo H.265): pipe isp_bypass=FALSE, chn YUV420. */
static int vi_setup_h265(void)
{
    ot_vi_dev_attr dev_attr;
    ot_vi_pipe_attr pipe_attr;
    ot_vi_chn_attr chn_attr;

    ss_mpi_vi_disable_chn(VI_PIPE_ID, VI_CHN_ID);
    ss_mpi_vi_stop_pipe(VI_PIPE_ID);
    ss_mpi_vi_destroy_pipe(VI_PIPE_ID);
    ss_mpi_vi_unbind(VI_DEV_ID, VI_PIPE_ID);
    ss_mpi_vi_disable_dev(VI_DEV_ID);
    usleep(50000);

    memset(&dev_attr, 0, sizeof(dev_attr));
    dev_attr.intf_mode = OT_VI_INTF_MODE_MIPI;
    dev_attr.work_mode = OT_VI_WORK_MODE_MULTIPLEX_1;
    dev_attr.component_mask[0] = 0xFFC00000;
    dev_attr.scan_mode = OT_VI_SCAN_PROGRESSIVE;
    dev_attr.ad_chn_id[0] = -1;
    dev_attr.ad_chn_id[1] = -1;
    dev_attr.ad_chn_id[2] = -1;
    dev_attr.ad_chn_id[3] = -1;
    dev_attr.data_seq = OT_VI_DATA_SEQ_YUYV;
    dev_attr.data_type = OT_VI_DATA_TYPE_RAW;
    dev_attr.data_reverse = TD_FALSE;
    dev_attr.in_size.width = IMG_WIDTH;
    dev_attr.in_size.height = IMG_HEIGHT;
    dev_attr.data_rate = OT_DATA_RATE_X1;
    CHECK(ss_mpi_vi_set_dev_attr(VI_DEV_ID, &dev_attr));
    CHECK(ss_mpi_vi_enable_dev(VI_DEV_ID));
    CHECK(ss_mpi_vi_bind(VI_DEV_ID, VI_PIPE_ID));

    memset(&pipe_attr, 0, sizeof(pipe_attr));
    pipe_attr.pipe_bypass_mode = OT_VI_PIPE_BYPASS_NONE;
    pipe_attr.isp_bypass = TD_FALSE;
    pipe_attr.size.width = IMG_WIDTH;
    pipe_attr.size.height = IMG_HEIGHT;
    pipe_attr.pixel_format = OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
    pipe_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    pipe_attr.frame_rate_ctrl.src_frame_rate = -1;
    pipe_attr.frame_rate_ctrl.dst_frame_rate = -1;
    CHECK(ss_mpi_vi_create_pipe(VI_PIPE_ID, &pipe_attr));
    CHECK(ss_mpi_vi_start_pipe(VI_PIPE_ID));

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.size.width = IMG_WIDTH;
    chn_attr.size.height = IMG_HEIGHT;
    chn_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    chn_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    chn_attr.video_format = OT_VIDEO_FORMAT_LINEAR;
    chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    chn_attr.mirror_en = TD_FALSE;
    chn_attr.flip_en = TD_FALSE;
    chn_attr.depth = 0;
    chn_attr.frame_rate_ctrl.src_frame_rate = -1;
    chn_attr.frame_rate_ctrl.dst_frame_rate = -1;
    CHECK(ss_mpi_vi_set_chn_attr(VI_PIPE_ID, VI_CHN_ID, &chn_attr));
    CHECK(ss_mpi_vi_enable_chn(VI_PIPE_ID, VI_CHN_ID));
    fprintf(stderr, "  VI ISP (YUV420) OK\n");
    return 0;
}

static int h265_sensor_setup(void)
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
    fprintf(stderr, "  dlopen %s -> %s @ %p\n", SNS_LIB_PATH, SNS_OBJ_SYMBOL,
            (void *)g_sns_obj);
    if (g_sns_obj->pfn_set_bus_info != NULL) {
        bus.i2c_dev = 0;
        CHECK(g_sns_obj->pfn_set_bus_info(VI_PIPE_ID, bus));
    }
    CHECK(ss_mpi_ae_register(VI_PIPE_ID, &g_ae_lib));
    CHECK(ss_mpi_awb_register(VI_PIPE_ID, &g_awb_lib));
    CHECK(g_sns_obj->pfn_register_callback(VI_PIPE_ID, &g_ae_lib, &g_awb_lib));
    return 0;
}

static int h265_isp_setup(void)
{
    ot_isp_pub_attr pub;
    ot_isp_bind_attr bind;

    memset(&pub, 0, sizeof(pub));
    pub.wnd_rect.x = 0;
    pub.wnd_rect.y = 0;
    pub.wnd_rect.width = IMG_WIDTH;
    pub.wnd_rect.height = IMG_HEIGHT;
    pub.sns_size.width = IMG_WIDTH;
    pub.sns_size.height = IMG_HEIGHT;
    pub.frame_rate = 30;
    pub.bayer_format = (ot_isp_bayer_format)0;   /* RGGB */
    pub.wdr_mode = OT_WDR_MODE_NONE;
    pub.sns_mode = 0;

    memset(&bind, 0, sizeof(bind));
    bind.sns_id = IMX662_SNS_ID;
    bind.ae_lib = g_ae_lib;
    bind.awb_lib = g_awb_lib;
    CHECK(ss_mpi_isp_set_bind_attr(VI_PIPE_ID, &bind));
    CHECK(ss_mpi_isp_mem_init(VI_PIPE_ID));
    CHECK(ss_mpi_isp_set_pub_attr(VI_PIPE_ID, &pub));
    CHECK(ss_mpi_isp_init(VI_PIPE_ID));

    /* CSC de salida (igual que waybeam): sin esto el ISP no entrega
       YUV420 a la chn VI. */
    {
        ot_isp_csc_attr csc;
        memset(&csc, 0, sizeof(csc));
        CHECK(ss_mpi_isp_get_csc_attr(VI_PIPE_ID, &csc));
        csc.enable = TD_TRUE;
        csc.satu = 60;
        csc.contr = 53;
        CHECK(ss_mpi_isp_set_csc_attr(VI_PIPE_ID, &csc));
    }

    /* AWB: dejamos el AWB del lib activo (parity waybeam).  El bypass
       estático con ganancia neutra producía frame blanco puro (std=0)
       en el H265.  waybeam sin bypass da imagen real. */

    /* Override INCK/DATARATE post-init (el lib puede dejar INCK=0x01,
       nosotros queremos 0x03/0x05).  Standby -> regs -> unstandby. */
    i2c_write_reg(0x3000, 0x01);
    usleep(2000);
    i2c_write_reg(0x3014, (td_u8)g_incksel);
    i2c_write_reg(0x3015, (td_u8)g_datarate);
    i2c_write_reg(0x3040, (td_u8)g_lanemode);
    i2c_write_reg(0x3000, 0x00);
    usleep(100000);
    i2c_write_reg(0x3001, 0x00);
    fprintf(stderr, "  ISP init OK (CSC + INCK=%02X DR=%02X)\n",
            (unsigned)g_incksel, (unsigned)g_datarate);
    return 0;
}

static int h265_vpss_setup(void)
{
    ot_vpss_grp_attr grp_attr;
    ot_vpss_chn_attr chn_attr;
    ot_mpp_chn src, dst;

    memset(&grp_attr, 0, sizeof(grp_attr));
    grp_attr.max_width = IMG_WIDTH;
    grp_attr.max_height = IMG_HEIGHT;
    grp_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    grp_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    grp_attr.dei_mode = OT_VPSS_DEI_MODE_OFF;
    grp_attr.frame_rate.src_frame_rate = -1;
    grp_attr.frame_rate.dst_frame_rate = -1;
    CHECK(ss_mpi_vpss_create_grp(VPSS_GRP, &grp_attr));

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.width = IMG_WIDTH;
    chn_attr.height = IMG_HEIGHT;
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
    src.dev_id = VI_PIPE_ID;
    src.chn_id = VI_CHN_ID;
    dst.mod_id = OT_ID_VPSS;
    dst.dev_id = VPSS_GRP;
    dst.chn_id = 0;
    CHECK(ss_mpi_sys_bind(&src, &dst));
    fprintf(stderr, "  VPSS OK\n");
    return 0;
}

static int h265_venc_setup(void)
{
    ot_venc_chn_attr attr;
    ot_venc_h265_vui vui;
    ot_venc_start_param start;
    ot_mpp_chn src, dst;
    uint32_t gop = 60;   /* 2 s @ 30fps */

    memset(&attr, 0, sizeof(attr));
    attr.venc_attr.type = OT_PT_H265;
    attr.venc_attr.max_pic_width = IMG_WIDTH;
    attr.venc_attr.max_pic_height = IMG_HEIGHT;
    attr.venc_attr.buf_size = ((IMG_WIDTH * IMG_HEIGHT * 3 / 4) + 63) & ~63u;
    attr.venc_attr.profile = 0;
    attr.venc_attr.is_by_frame = TD_TRUE;
    attr.venc_attr.pic_width = IMG_WIDTH;
    attr.venc_attr.pic_height = IMG_HEIGHT;
    attr.venc_attr.h265_attr.rcn_ref_share_buf_en = TD_TRUE;
    attr.venc_attr.h265_attr.frame_buf_ratio = 75;
    attr.rc_attr.rc_mode = OT_VENC_RC_MODE_H265_CBR;
    attr.rc_attr.h265_cbr.gop = gop;
    attr.rc_attr.h265_cbr.stats_time = 1;
    attr.rc_attr.h265_cbr.src_frame_rate = 30;
    attr.rc_attr.h265_cbr.dst_frame_rate = 30;
    attr.rc_attr.h265_cbr.bit_rate = g_bitrate_kbps;
    attr.gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
    attr.gop_attr.normal_p.ip_qp_delta = 0;

    CHECK(ss_mpi_venc_create_chn(VENC_CHN, &attr));

    memset(&vui, 0, sizeof(vui));
    CHECK(ss_mpi_venc_get_h265_vui(VENC_CHN, &vui));
    vui.vui_time_info.timing_info_present_flag = 1;
    vui.vui_time_info.num_units_in_tick = 1000;
    vui.vui_time_info.time_scale = 30000;
    vui.vui_time_info.num_ticks_poc_diff_one_minus1 = 0;
    vui.vui_video_signal.video_signal_type_present_flag = 1;
    vui.vui_video_signal.video_format = 5;
    vui.vui_video_signal.video_full_range_flag = 1;
    vui.vui_video_signal.colour_description_present_flag = 1;
    vui.vui_video_signal.colour_primaries = 1;
    vui.vui_video_signal.transfer_characteristics = 1;
    vui.vui_video_signal.matrix_coefficients = 1;
    CHECK(ss_mpi_venc_set_h265_vui(VENC_CHN, &vui));

    memset(&start, 0, sizeof(start));
    start.recv_pic_num = -1;
    CHECK(ss_mpi_venc_start_chn(VENC_CHN, &start));

    src.mod_id = OT_ID_VPSS;
    src.dev_id = VPSS_GRP;
    src.chn_id = VPSS_CHN;
    dst.mod_id = OT_ID_VENC;
    dst.dev_id = 0;
    dst.chn_id = VENC_CHN;
    CHECK(ss_mpi_sys_bind(&src, &dst));
    fprintf(stderr, "  VENC H.265 OK\n");
    return 0;
}

static void h265_teardown(void)
{
    ot_mpp_chn src = { OT_ID_VI, VI_PIPE_ID, VI_CHN_ID };
    ot_mpp_chn dst = { OT_ID_VPSS, VPSS_GRP, 0 };
    ot_mpp_chn vsrc = { OT_ID_VPSS, VPSS_GRP, VPSS_CHN };
    ot_mpp_chn vdst = { OT_ID_VENC, 0, VENC_CHN };

    (void)ss_mpi_sys_unbind(&vsrc, &vdst);
    (void)ss_mpi_sys_unbind(&src, &dst);
    (void)ss_mpi_vpss_stop_grp(VPSS_GRP);
    (void)ss_mpi_vpss_disable_chn(VPSS_GRP, VPSS_CHN);
    (void)ss_mpi_vpss_destroy_grp(VPSS_GRP);
    (void)ss_mpi_venc_stop_chn(VENC_CHN);
    (void)ss_mpi_venc_destroy_chn(VENC_CHN);
    ss_mpi_isp_exit(VI_PIPE_ID);
    if (g_isp_thread_ok) {
        pthread_join(g_isp_thread, NULL);
        g_isp_thread_ok = 0;
    }
    if (g_sns_obj != NULL && g_sns_obj->pfn_un_register_callback != NULL)
        g_sns_obj->pfn_un_register_callback(VI_PIPE_ID, &g_ae_lib, &g_awb_lib);
    ss_mpi_awb_unregister(VI_PIPE_ID, &g_awb_lib);
    ss_mpi_ae_unregister(VI_PIPE_ID, &g_ae_lib);
    if (g_sns_handle != NULL) {
        dlclose(g_sns_handle);
        g_sns_handle = NULL;
        g_sns_obj = NULL;
    }
}

/* Teardown VI (común a ambos modos) */
static void vi_teardown(void)
{
    ss_mpi_vi_disable_chn(VI_PIPE_ID, VI_CHN_ID);
    ss_mpi_vi_stop_pipe(VI_PIPE_ID);
    ss_mpi_vi_destroy_pipe(VI_PIPE_ID);
    ss_mpi_vi_unbind(VI_DEV_ID, VI_PIPE_ID);
    ss_mpi_vi_disable_dev(VI_DEV_ID);
    usleep(200000);
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

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
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
    if (fd < 0) return -1;
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

/* Loop H.265: lee VENC fd, concatena packs, envía header WB + AnnexB. */
static void h265_run_loop(int sock, int bench_seconds)
{
    int venc_fd = ss_mpi_venc_get_fd(VENC_CHN);
    uint64_t t0 = now_us();
    int client = -1;
    int bench = (bench_seconds > 0);

    if (venc_fd < 0) {
        fprintf(stderr, "  venc_get_fd FAIL\n");
        return;
    }
    if (!bench)
        fprintf(stderr, "  H.265 TCP server on port, waiting client...\n");

    while (g_running) {
        ot_venc_chn_status status;
        ot_venc_stream stream;
        fd_set readfds;
        struct timeval timeout = { 1, 0 };
        uint8_t hdr[WB_HDR_LEN];
        td_s32 ret;
        int ready, i, is_idr;

        if (client < 0 && sock >= 0) {
            fd_set rfds;
            struct timeval tv = { 0, 0 };
            int r;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            r = select(sock + 1, &rfds, NULL, NULL, &tv);
            if (r > 0) {
                client = accept(sock, NULL, NULL);
                if (client >= 0)
                    fprintf(stderr, "  H265 client connected\n");
            }
        }
        if (bench && (now_us() - t0) > (uint64_t)bench_seconds * 1000000)
            break;

        FD_ZERO(&readfds);
        FD_SET(venc_fd, &readfds);
        ready = select(venc_fd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (ready == 0) {
            if (bench && (now_us() - t0) > (uint64_t)bench_seconds * 1000000)
                break;
            continue;
        }

        memset(&status, 0, sizeof(status));
        ret = ss_mpi_venc_query_status(VENC_CHN, &status);
        if (ret != TD_SUCCESS || status.cur_packs == 0) continue;

        memset(&stream, 0, sizeof(stream));
        stream.pack = calloc(status.cur_packs, sizeof(*stream.pack));
        if (!stream.pack) break;
        stream.pack_cnt = status.cur_packs;
        ret = ss_mpi_venc_get_stream(VENC_CHN, &stream, 1000);
        if (ret != TD_SUCCESS) {
            free(stream.pack);
            continue;
        }

        size_t total = 0;
        for (i = 0; i < (int)stream.pack_cnt; i++)
            total += stream.pack[i].len;
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

        is_idr = frame_is_idr(frame, total);
        g_h265_frames++;
        g_h265_bytes += total;
        if (is_idr) g_h265_idr++;

        if (client >= 0) {
            hdr[0] = WB_MAGIC0;
            hdr[1] = WB_MAGIC1;
            hdr[2] = (uint8_t)(is_idr ? 1 : 0);
            hdr[3] = 0;
            hdr[4] = (uint8_t)(total & 0xFF);
            hdr[5] = (uint8_t)((total >> 8) & 0xFF);
            hdr[6] = (uint8_t)((total >> 16) & 0xFF);
            hdr[7] = (uint8_t)((total >> 24) & 0xFF);
            uint64_t pts = stream.pack_cnt ? stream.pack[0].pts : 0;
            hdr[8] = (uint8_t)(pts & 0xFF);
            hdr[9] = (uint8_t)((pts >> 8) & 0xFF);
            hdr[10] = (uint8_t)((pts >> 16) & 0xFF);
            hdr[11] = (uint8_t)((pts >> 24) & 0xFF);
            if (write_all(client, hdr, WB_HDR_LEN) != 0 ||
                write_all(client, frame, total) != 0) {
                fprintf(stderr, "  H265 TCP write fail — client disconnected\n");
                close(client);
                client = -1;
            }
        }

        uint64_t now = now_us();
        if (bench && g_h265_frames % 30 == 0)
            fprintf(stderr, "> H265 frames=%llu bytes=%llu idr=%llu fps=%.2f\n",
                    (unsigned long long)g_h265_frames,
                    (unsigned long long)g_h265_bytes,
                    (unsigned long long)g_h265_idr,
                    (double)g_h265_frames * 1000000.0 / (double)(now - t0));

        ret = ss_mpi_venc_release_stream(VENC_CHN, &stream);
        free(stream.pack);
        free(frame);
        if (ret != TD_SUCCESS) break;
    }
    if (client >= 0) close(client);
}

/* --------------------------------------------------------------------
 * Loop de datos unificado: sirve frames en el modo actual (RAW o H.265)
 * y se adapta en runtime si el canal de control hace MODE raw|h265.
 * -------------------------------------------------------------------- */
static void data_loop(int server_fd)
{
    int mem_fd = -1;
    int mem_mmaped = 0;
    void *mem_map = NULL;
    td_u64 mem_map_len = 0;
    td_u64 mem_map_phys = 0;

    int cur_mode = -1;          /* fuerza re-init de recursos al primer frame */
    int venc_fd = -1;
    td_u32 frame_index = 0;
    td_s32 ret;

    while (g_running) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = -1;
        /* accept select-based: SIGTERM (musl SA_RESTART) no interrumpe
           accept() bloqueante -> usar select con timeout para salir limpio */
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
                /* Sin cliente: drenar VENC hasta vaciarlo para no acumular backlog
                   (los frames se descartan). */
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
                if (!g_running) break;
                continue;
            }
            client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
            break;
        }
        if (client_fd < 0) {
            if (g_running) continue;
            break;
        }

        fprintf(stderr, "  Client: %s (mode %s)\n", inet_ntoa(cli_addr.sin_addr),
                g_mode == MODE_H265 ? "H265" : "RAW");
        fflush(stderr);

        /* Cliente nuevo en H.265: no enviar frames hasta el primer
           IDR (VPS/SPS/PPS/IDR) para que el decoder arranque limpio. */
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

        while (g_running) {
            /* --- si cambió el modo, refrescar recursos específicos --- */
            if (cur_mode != g_mode) {
                if (cur_mode == MODE_H265) {
                    venc_fd = -1;
                }
                if (g_mode == MODE_H265) {
                    venc_fd = ss_mpi_venc_get_fd(VENC_CHN);
                    fprintf(stderr, "  [data] venc_fd=%d (switched to H265)\n", venc_fd);
                    h265_wait_idr = 1;
                    (void)ss_mpi_venc_request_idr(VENC_CHN, TD_TRUE);
                } else {
                    if (mem_fd < 0) mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
                    fprintf(stderr, "  [data] mem_fd=%d (switched to RAW)\n", mem_fd);
                }
                cur_mode = g_mode;
                consecutive_fail = 0;
                fflush(stderr);
            }

            if (g_mode == MODE_H265) {
                /* ---------- H.265: un frame VENC + header WB ---------- */
                if (g_switching) { usleep(50000); continue; }
                if (venc_fd < 0) { usleep(50000); continue; }
                fd_set rfds;
                struct timeval tv = { 1, 0 };
                FD_ZERO(&rfds);
                FD_SET(venc_fd, &rfds);
                if (select(venc_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
                    if (g_running) continue;
                    break;
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
                g_h265_frames++;
                g_h265_bytes += total;
                if (is_idr) g_h265_idr++;

                /* Esperar el primer IDR antes de enviar (arranque limpio). */
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

            /* ---------- RAW: comandos + frame bayer 48B header ---------- */
            fd_set rfds;
            struct timeval tv = {0, 10000};
            FD_ZERO(&rfds);
            FD_SET(client_fd, &rfds);

            if (select(client_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
                char tmp[256];
                ssize_t n = recv(client_fd, tmp, sizeof(tmp) - 1, 0);
                if (n == 0) {
                    break;  /* peer closed */
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                } else if (n > 0) {
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

            /* Compute timeout: at least 2x frame period + margin */
            td_u32 frame_period_us = (td_u32)lines_to_us(g_vmax);
            td_u32 timeout_ms = frame_period_us / 1000 * 2 + 2000;
            if (timeout_ms < 3000) timeout_ms = 3000;

            ot_video_frame_info frame_info;
            memset(&frame_info, 0, sizeof(frame_info));
            ret = ot_mpi_vi_get_chn_frame(VI_PIPE_ID, VI_CHN_ID, &frame_info, timeout_ms);
            if (ret != TD_SUCCESS) {
                /* Durante un switch_mode() el VI se destruye/reconstruye:
                   los fallos son transitorios y NO deben cerrar el cliente. */
                if (g_switching) {
                    usleep(50000);
                    continue;
                }
                consecutive_fail++;
                fprintf(stderr, "  [DBG] get_chn_frame fail 0x%x (fail=%d)\n", ret, consecutive_fail);
                fflush(stderr);
                if (consecutive_fail > 3) break;
                continue;
            }
            consecutive_fail = 0;

            const ot_video_frame *vf = &frame_info.video_frame;
            if ((frame_index % 15) == 0) {
                fprintf(stderr, "  [DBG] got frame idx=%u w=%u h=%u stride=%u phys=0x%llx\n",
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
                    ot_mpi_vi_release_chn_frame(VI_PIPE_ID, VI_CHN_ID, &frame_info);
                    break;
                }
                mem_map_phys = page_phys;
                mem_map_len = map_len;
                mem_mmaped = 1;
            }

            td_u8 *src = (td_u8 *)mem_map + page_off;

            if (g_save_next) {
                write_fits(g_save_filename, src, vf->width, height, stride,
                           g_exposure_lines, g_again, g_dgain);
                g_save_next = 0;
            }

            td_u8 cap_flag = (g_burst_remaining > 0) ? 1 : 0;
            if (g_burst_remaining > 0) g_burst_remaining--;

            if (send_frame_header(client_fd, (td_u32)vf->width, height, stride,
                                  (td_u32)map_size, frame_index++, now_us(),
                                  (td_u32)lines_to_us(g_exposure_lines),
                                  g_again, g_dgain, g_vmax,
                                  get_temp_cached(), cap_flag) < 0) {
                ot_mpi_vi_release_chn_frame(VI_PIPE_ID, VI_CHN_ID, &frame_info);
                break;
            }
            if (send_all(client_fd, src, (size_t)map_size) < 0) {
                ot_mpi_vi_release_chn_frame(VI_PIPE_ID, VI_CHN_ID, &frame_info);
                break;
            }

            ot_mpi_vi_release_chn_frame(VI_PIPE_ID, VI_CHN_ID, &frame_info);
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
}

/* AE manual vía ISP (modo H.265) */
static void ae_apply_isp(void)
{
    ot_isp_exposure_attr attr;

    if (ss_mpi_isp_get_exposure_attr(VI_PIPE_ID, &attr) != TD_SUCCESS) {
        fprintf(stderr, "[ctrl] get_exposure_attr fail\n");
        return;
    }
    if (g_manual_ae) {
        attr.op_type = OT_OP_MODE_MANUAL;
        attr.bypass = TD_FALSE;
        attr.manual_attr.exp_time_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.exp_time = g_man_exp_us;
        attr.manual_attr.a_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.a_gain = g_man_again;
        attr.manual_attr.d_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.d_gain = g_man_dgain;
        attr.manual_attr.ispd_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.isp_d_gain = 1024;
    } else {
        attr.op_type = OT_OP_MODE_AUTO;
        attr.bypass = TD_FALSE;
    }
    td_s32 r = ss_mpi_isp_set_exposure_attr(VI_PIPE_ID, &attr);
    fprintf(stderr, "[ctrl] AE %s exp=%uus again=%u dgain=%u ret=0x%x\n",
            g_manual_ae ? "MANUAL" : "AUTO", g_man_exp_us, g_man_again,
            g_man_dgain, r);
}

/* --------------------------------------------------------------------
 * Cambio de modo raw <-> h265 en runtime.  Tear-down del pipeline actual
 * y rebuild en el otro modo.  El sensor y el MCLK NO se tocan.
 * -------------------------------------------------------------------- */
static int switch_mode(int new_mode)
{
    if (new_mode == g_mode) {
        fprintf(stderr, "[ctrl] MODE ya es %s\n",
                g_mode == MODE_H265 ? "H265" : "RAW");
        return 0;
    }

    fprintf(stderr, "[ctrl] switch a modo %s...\n",
            new_mode == MODE_H265 ? "H265" : "RAW");
    fflush(stderr);

    /* data_loop no debe contar fallos de get_chn_frame durante el
       teardown/rebuild (cerraría el cliente del viewer). */
    g_switching = 1;

    if (g_mode == MODE_H265)
        h265_teardown();
    vi_teardown();

    if (new_mode == MODE_H265) {
        if (vi_setup_h265() != 0 ||
            h265_sensor_setup() != 0 ||
            h265_isp_setup() != 0 ||
            h265_vpss_setup() != 0) {
            fprintf(stderr, "[ctrl] switch a H265 FAIL — estado inconsistente\n");
            g_switching = 0;
            return -1;
        }
        if (h265_venc_setup() != 0) {
            fprintf(stderr, "[ctrl] VENC FAIL en switch\n");
            g_switching = 0;
            return -1;
        }
        g_isp_thread_ok = 0;
        if (pthread_create(&g_isp_thread, NULL, isp_thread_fn, NULL) != 0) {
            fprintf(stderr, "FAIL pthread_create ISP\n");
            g_switching = 0;
            return -1;
        }
        g_isp_thread_ok = 1;
        /* CCM del sensor (igual que en el init principal) */
        {
            ot_isp_color_matrix_attr ccm;
            memset(&ccm, 0, sizeof(ccm));
            if (ss_mpi_isp_get_ccm_attr(VI_PIPE_ID, &ccm) == TD_SUCCESS &&
                ccm.auto_attr.ccm_tab_num >= 3) {
                ccm.op_type = OT_OP_MODE_AUTO;
                ccm.auto_attr.iso_act_en = TD_FALSE;
                ccm.auto_attr.temp_act_en = TD_FALSE;
                (void)ss_mpi_isp_set_ccm_attr(VI_PIPE_ID, &ccm);
            }
        }
    } else {
        if (vi_setup_raw() != 0) {
            fprintf(stderr, "[ctrl] switch a RAW FAIL\n");
            g_switching = 0;
            return -1;
        }
    }
    g_mode = new_mode;
    g_switching = 0;
    fprintf(stderr, "[ctrl] modo activo: %s\n",
            g_mode == MODE_H265 ? "H265" : "RAW");
    return 0;
}

/* --------------------------------------------------------------------
 * Canal de control TCP (puerto CTRL_PORT).  Comandos de texto para ambos
 * modos: MODE raw|h265, E/A/D/T (AE), X <reg>, R, ?.  Separado del socket
 * de datos para no corromper el stream binario.
 * -------------------------------------------------------------------- */
static void ctrl_handle_client(int cfd)
{
    char buf[256];
    size_t used = 0;

    fprintf(stderr, "[ctrl] MODE raw|h265 | E <us> | A <again> | D <dgain> | "
            "T auto | R status | X <reghex> | ? ayuda\n");
    while (g_running) {
        ssize_t n = read(cfd, buf + used, sizeof(buf) - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        char *p;
        while (used > 0 && (p = memchr(buf, '\n', used)) != NULL) {
            size_t len = (size_t)(p - buf);
            if (len > 0 && buf[len - 1] == '\r') len--;
            if (len > 0) {
                buf[len] = 0;
                char *line = buf;
                if (g_mode == MODE_H265 &&
                    (line[0] == 'E' || line[0] == 'A' || line[0] == 'D' ||
                     line[0] == 'T')) {
                    if (line[0] == 'E' && line[1] == ' ') {
                        g_manual_ae = 1;
                        g_man_exp_us = (td_u32)atoi(line + 2);
                        if (g_man_exp_us < 100) g_man_exp_us = 100;
                        if (g_man_exp_us > g_max_exp_us) g_man_exp_us = g_max_exp_us;
                        ae_apply_isp();
                    } else if (line[0] == 'A' && line[1] == ' ') {
                        g_manual_ae = 1;
                        g_man_again = (td_u32)atoi(line + 2);
                        if (g_man_again < 1024) g_man_again = 1024;
                        if (g_man_again > 32768) g_man_again = 32768;
                        ae_apply_isp();
                    } else if (line[0] == 'D' && line[1] == ' ') {
                        g_manual_ae = 1;
                        g_man_dgain = (td_u32)atoi(line + 2);
                        if (g_man_dgain < 1024) g_man_dgain = 1024;
                        if (g_man_dgain > 16384) g_man_dgain = 16384;
                        ae_apply_isp();
                    } else if (line[0] == 'T' && strncmp(line, "T auto", 6) == 0) {
                        g_manual_ae = 0;
                        ae_apply_isp();
                    } else if (line[0] == 'T' && line[1] == ' ') {
                        /* "T <us>": exposición manual en microsegundos */
                        g_manual_ae = 1;
                        g_man_exp_us = (td_u32)atoi(line + 2);
                        if (g_man_exp_us < 100) g_man_exp_us = 100;
                        if (g_man_exp_us > g_max_exp_us) g_man_exp_us = g_max_exp_us;
                        ae_apply_isp();
                    }
                } else if (strncmp(line, "MODE ", 5) == 0) {
                    int want = (!strncmp(line + 5, "h265", 4)) ? MODE_H265 : MODE_RAW;
                    if (switch_mode(want) != 0)
                        fprintf(stderr, "[ctrl] MODE switch falló\n");
                } else if (line[0] == 'R') {
                    if (g_mode == MODE_H265) {
                        ot_isp_exp_info info;
                        memset(&info, 0, sizeof(info));
                        if (ss_mpi_isp_query_exposure_info(VI_PIPE_ID, &info) == TD_SUCCESS)
                            fprintf(stderr, "[ctrl] H265 exp=%uus again=%u(%.2fx) "
                                    "dgain=%u(%.2fx) fps=%u iso=%u\n",
                                    info.exp_time, info.a_gain, info.a_gain / 1024.0,
                                    info.d_gain, info.d_gain / 1024.0, info.fps, info.iso);
                        else
                            fprintf(stderr, "[ctrl] query_exposure_info fail\n");
                    } else {
                        fprintf(stderr, "[ctrl] RAW E=%u VMAX=%u A=%u D=%u TEMP=%.2fC\n",
                                g_exposure_lines, g_vmax, g_again, g_dgain,
                                (double)get_temp_cached() / 100.0);
                    }
                } else if (line[0] == 'X' && line[1] == ' ') {
                    unsigned long r = strtoul(line + 2, NULL, 16);
                    if (r <= 0xFFFF) {
                        td_u8 b0 = i2c_read_reg((td_u16)r);
                        td_u8 b1 = i2c_read_reg((td_u16)(r + 1));
                        td_u8 b2 = i2c_read_reg((td_u16)(r + 2));
                        fprintf(stderr, "[ctrl] reg 0x%04lX = %02X %02X %02X\n",
                                r, b0, b1, b2);
                    }
                } else if (line[0] == '?') {
                    fprintf(stderr, "[ctrl] MODE raw|h265 | E <us> | A <again> 22.10 | "
                            "D <dgain> 22.10 | T auto | R status | X <reghex>\n");
                } else {
                    /* modo RAW: usar el mismo process_command del socket de datos */
                    process_command(line);
                }
            }
            used -= len + 1;
            memmove(buf, p + 1, used);
        }
        if (used == sizeof(buf) - 1) used = 0;
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
    while (g_running) {
        fd_set rfds;
        struct timeval tv = { 1, 0 };
        int r;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        r = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR) continue;
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

int main(int argc, char *argv[])
{
    td_s32 ret;
    int port = TCP_PORT;
    ot_vb_pool pool = OT_VB_INVALID_POOL_ID;

    int bench_seconds = 10;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0) g_bench = 1;
        else if (strncmp(argv[i], "--bench=", 8) == 0) { g_bench = 1; bench_seconds = atoi(argv[i] + 8); }
        else if (strcmp(argv[i], "--sweep") == 0) g_sweep = 1;
        else if (strncmp(argv[i], "--incksel=", 10) == 0) g_incksel = (int)strtol(argv[i] + 10, NULL, 0);
        else if (strncmp(argv[i], "--datarate=", 11) == 0) g_datarate = (int)strtol(argv[i] + 11, NULL, 0);
        else if (strncmp(argv[i], "--vc=", 5) == 0) g_vc_num = (int)strtol(argv[i] + 5, NULL, 0);
        else if (strncmp(argv[i], "--shrconv=", 10) == 0) g_shrconv = (int)strtol(argv[i] + 10, NULL, 0);
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            g_mode = (strncmp(argv[i + 1], "h265", 4) == 0) ? MODE_H265 : MODE_RAW;
            i++;
        } else if (strncmp(argv[i], "--mode=", 7) == 0) {
            g_mode = (strncmp(argv[i] + 7, "h265", 4) == 0) ? MODE_H265 : MODE_RAW;
        } else if (strcmp(argv[i], "--ctrl-port") == 0 && i + 1 < argc) {
            g_ctrl_port = atoi(argv[i + 1]);
            i++;
        } else if (strncmp(argv[i], "--ctrl-port=", 12) == 0) {
            g_ctrl_port = atoi(argv[i] + 12);
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            g_bitrate_kbps = atoi(argv[i + 1]);
            i++;
        } else if (strncmp(argv[i], "--bitrate=", 10) == 0) {
            g_bitrate_kbps = atoi(argv[i] + 10);
        }
    }
    if (argc > 1 && argv[1][0] != '-') port = atoi(argv[1]);
    if (port <= 0) port = TCP_PORT;

    /* sweep es un debug solo-RAW (usa el chn bayer): forzar modo RAW */
    if (g_sweep) g_mode = MODE_RAW;

    fprintf(stderr, "=== ASTRO Streamer (port %d, mode %s, ctrl %d) ===\n",
            port, g_mode == MODE_H265 ? "H265" : "RAW", g_ctrl_port);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    fprintf(stderr, "[0] MPI init...\n");
    /* pre-clean parity waybeam: sys_exit() + vb_exit() ANTES de re-setear
       el VB, y luego sys_init() DESPUÉS del vb_init (waybeam validated
       con esta secuencia entrega imagen real; sin el pre-clean el VI
       quedaba sin señal MIPI, frame blanco f0 0f ff tras reboot). */
    ret = ot_mpi_sys_exit();
    fprintf(stderr, "  sys_exit: 0x%x\n", ret);
    usleep(50000);
    ret = ot_mpi_vb_exit();
    fprintf(stderr, "  vb_exit: 0x%x\n", ret);
    usleep(50000);
    ot_vb_cfg vb_cfg;
    memset(&vb_cfg, 0, sizeof(vb_cfg));
    /* Dos pools comunes: raw (bayer 12bpp, w*h*2 como waybeam) para VI,
       yuv (YVU420) para VI-chn/VPSS/VENC en modo H.265.  Con el MMZ de
       64MB caben. */
    vb_cfg.max_pool_cnt = 2;
    vb_cfg.common_pool[0].blk_size = (IMG_WIDTH * IMG_HEIGHT * 2) + 0x4000;
    vb_cfg.common_pool[0].blk_cnt = 6;
    vb_cfg.common_pool[0].remap_mode = OT_VB_REMAP_MODE_NONE;
    strncpy(vb_cfg.common_pool[0].mmz_name, "anonymous", sizeof(vb_cfg.common_pool[0].mmz_name) - 1);
    vb_cfg.common_pool[1].blk_size = (IMG_WIDTH * IMG_HEIGHT * 3 / 2) + 0x4000;
    vb_cfg.common_pool[1].blk_cnt = 4;
    vb_cfg.common_pool[1].remap_mode = OT_VB_REMAP_MODE_NONE;
    strncpy(vb_cfg.common_pool[1].mmz_name, "anonymous", sizeof(vb_cfg.common_pool[1].mmz_name) - 1);
    ret = ot_mpi_vb_set_cfg(&vb_cfg);
    fprintf(stderr, "  set_cfg: 0x%x\n", ret);
    ret = ot_mpi_vb_init();
    fprintf(stderr, "  init: 0x%x\n", ret);
    ret = ot_mpi_sys_init();
    fprintf(stderr, "  sys_init: 0x%x\n", ret);

    fprintf(stderr, "[3] VB pool...\n");
    /* Use the common pool created by vb_init for the VI module (no user pool). */
    ot_vb_common_pools_id pools_id;
    memset(&pools_id, 0, sizeof(pools_id));
    ret = ot_mpi_vb_get_common_pool_id(&pools_id);
    fprintf(stderr, "  get_common_pool_id: 0x%x cnt=%u id[0]=%u\n",
            ret, pools_id.pool_cnt,
            pools_id.pool_cnt > 0 ? pools_id.pool[0] : (ot_vb_pool)-1);
    if (pools_id.pool_cnt == 0) {
        ot_vb_pool_cfg pool_cfg;
        memset(&pool_cfg, 0, sizeof(pool_cfg));
        pool_cfg.blk_size = RAW12_BUF_SIZE;
        pool_cfg.blk_cnt = 6;
        pool_cfg.remap_mode = OT_VB_REMAP_MODE_NONE;
        strncpy(pool_cfg.mmz_name, "anonymous", sizeof(pool_cfg.mmz_name) - 1);
        pool = ot_mpi_vb_create_pool(&pool_cfg);
        if (pool == OT_VB_INVALID_POOL_ID) { fprintf(stderr, "  create_pool FAILED\n"); goto cleanup; }
        fprintf(stderr, "  create_pool=%u\n", pool);
    }
    ret = ot_mpi_vb_init_mod_common_pool(OT_VB_UID_VI);
    fprintf(stderr, "  init_mod_common_pool(VI): 0x%x\n", ret);
    ret = ot_mpi_vb_init_mod_common_pool(OT_VB_UID_VPSS);
    fprintf(stderr, "  init_mod_common_pool(VPSS): 0x%x\n", ret);
    ret = ot_mpi_vb_init_mod_common_pool(OT_VB_UID_VENC);
    fprintf(stderr, "  init_mod_common_pool(VENC): 0x%x\n", ret);

    /* [4] NO se llama ss_mpi_sys_set_vi_vpss_mode (parity waybeam offline:
       el branch offline solo hace sys_init(); el modo VI-offline ya es el
       atributo estático del MPP).  Llamarlo aquí dejaba el VI sin señal
       MIPI -> frame blanco. */

    fprintf(stderr, "[5] MCLK + sensor + MIPI RX...\n");
    enable_mclk_and_reset_sensor();

    fprintf(stderr, "[6] VI pipeline (%s)...\n",
            g_mode == MODE_H265 ? "H265" : "RAW");
    if (g_mode == MODE_H265) {
        /* orden parity waybeam: MIPI -> VI -> I2C/lib -> ISP -> VPSS -> VENC */
        if (vi_setup_h265() != 0) {
            fprintf(stderr, "  H265 pipeline FAIL (vi)\n");
            goto cleanup;
        }
        /* I2C al sensor (parity waybeam: se abre DESPUÉS del VI setup). */
        g_i2c_fd = open("/dev/i2c-0", O_RDWR);
        if (g_i2c_fd >= 0) {
            ret = ioctl(g_i2c_fd, OT_I2C_SLAVE_FORCE, (I2C_DEV_ADDR >> 1));
            if (ret < 0) { fprintf(stderr, "  I2C fail\n"); close(g_i2c_fd); g_i2c_fd = -1; }
            else {
                unsigned char rid_reg[2] = {0x30, 0xDC};
                unsigned char rid_val = 0;
                write(g_i2c_fd, rid_reg, 2);
                read(g_i2c_fd, &rid_val, 1);
                fprintf(stderr, "  Chip ID=0x%02X\n", rid_val);
            }
        }
        if (h265_sensor_setup() != 0 ||
            h265_isp_setup() != 0 ||
            h265_vpss_setup() != 0 ||
            h265_venc_setup() != 0) {
            fprintf(stderr, "  H265 pipeline FAIL\n");
            goto cleanup;
        }
        if (pthread_create(&g_isp_thread, NULL, isp_thread_fn, NULL) != 0) {
            fprintf(stderr, "  FAIL pthread_create ISP\n");
            goto cleanup;
        }
        g_isp_thread_ok = 1;
        /* CCM del sensor (igual que waybeam) */
        {
            ot_isp_color_matrix_attr ccm;
            memset(&ccm, 0, sizeof(ccm));
            if (ss_mpi_isp_get_ccm_attr(VI_PIPE_ID, &ccm) == TD_SUCCESS &&
                ccm.auto_attr.ccm_tab_num >= 3) {
                ccm.op_type = OT_OP_MODE_AUTO;
                ccm.auto_attr.iso_act_en = TD_FALSE;
                ccm.auto_attr.temp_act_en = TD_FALSE;
                (void)ss_mpi_isp_set_ccm_attr(VI_PIPE_ID, &ccm);
            }
        }
        g_max_exp_us = 33333;
        fprintf(stderr, "  H265 pipeline OK\n");
    } else {
        if (vi_setup_raw() != 0) {
            fprintf(stderr, "  RAW pipeline FAIL\n");
            goto cleanup;
        }
        g_i2c_fd = open("/dev/i2c-0", O_RDWR);
        if (g_i2c_fd >= 0) {
            ret = ioctl(g_i2c_fd, OT_I2C_SLAVE_FORCE, (I2C_DEV_ADDR >> 1));
            if (ret < 0) { fprintf(stderr, "  I2C fail\n"); close(g_i2c_fd); g_i2c_fd = -1; }
            else {
                unsigned char rid_reg[2] = {0x30, 0xDC};
                unsigned char rid_val = 0;
                write(g_i2c_fd, rid_reg, 2);
                read(g_i2c_fd, &rid_val, 1);
                fprintf(stderr, "  Chip ID=0x%02X\n", rid_val);
            }
        }
        if (init_sensor_full() == 0) fprintf(stderr, "  Sensor OK\n");
        g_max_exp_us = 33333;
    }
    usleep(200000);

    if (g_bench) {
        fprintf(stderr, "[10] BENCH mode (%s): measuring for %ds...\n",
                g_mode == MODE_H265 ? "H265" : "RAW", bench_seconds);
        fflush(stderr);

        if (g_mode == MODE_H265) {
            h265_run_loop(-1, bench_seconds);
            fprintf(stderr, "BENCH done: frames=%llu bytes=%llu idr=%llu\n",
                    (unsigned long long)g_h265_frames,
                    (unsigned long long)g_h265_bytes,
                    (unsigned long long)g_h265_idr);
        } else {
            struct timeval t_start, t_now;
            gettimeofday(&t_start, NULL);
            int frames = 0, errors = 0;

            while (g_running) {
                ot_video_frame_info frame_info;
                memset(&frame_info, 0, sizeof(frame_info));
                ret = ot_mpi_vi_get_chn_frame(VI_PIPE_ID, VI_CHN_ID, &frame_info, 3000);
                if (ret != TD_SUCCESS) { errors++; if (errors > 30) break; continue; }
                ot_mpi_vi_release_chn_frame(VI_PIPE_ID, VI_CHN_ID, &frame_info);
                frames++;

                gettimeofday(&t_now, NULL);
                double elapsed = (t_now.tv_sec - t_start.tv_sec) +
                                 (t_now.tv_usec - t_start.tv_usec) / 1e6;
                if (elapsed >= bench_seconds) break;
                if (elapsed >= 2.0) {
                    fprintf(stderr, "  frames=%d in %.1fs = %.2f fps (err=%d)\n",
                            frames, elapsed, frames / elapsed, errors);
                    fflush(stderr);
                    frames = 0; errors = 0;
                    gettimeofday(&t_start, NULL);
                }
            }
            fprintf(stderr, "BENCH done.\n");
        }
        goto cleanup;
    }

    if (g_sweep) {
        fprintf(stderr, "[10] SWEEP mode: relocking PLL for each INCK_SEL...\n");
        fflush(stderr);

        static const struct { int inc; int dr; } sweep_table[] = {
            {0x01, 0x02}, {0x01, 0x05}, {0x02, 0x05}, {0x03, 0x05},
            {0x04, 0x05}, {0x00, 0x05}, {0x05, 0x05}, {0x06, 0x05},
            {0x07, 0x05}, {0x00, 0x02}, {0x02, 0x02}, {0x03, 0x02},
            {0x04, 0x02}, {0x05, 0x02},
        };

        for (unsigned s = 0; s < sizeof(sweep_table)/sizeof(sweep_table[0]) && g_running; s++) {
            int inc = sweep_table[s].inc;
            int dr = sweep_table[s].dr;

            i2c_write_reg(0x3000, 0x01);
            usleep(50000);
            i2c_write_reg(0x3014, (td_u8)inc);
            i2c_write_reg(0x3015, (td_u8)dr);
            i2c_write_reg(0x3000, 0x00);
            usleep(200000);

            struct timeval t_start, t_now;
            gettimeofday(&t_start, NULL);
            int frames = 0, errors = 0;
            while (g_running) {
                ot_video_frame_info frame_info;
                memset(&frame_info, 0, sizeof(frame_info));
                ret = ot_mpi_vi_get_chn_frame(VI_PIPE_ID, VI_CHN_ID, &frame_info, 2000);
                if (ret != TD_SUCCESS) { errors++; continue; }
                ot_mpi_vi_release_chn_frame(VI_PIPE_ID, VI_CHN_ID, &frame_info);
                frames++;
                gettimeofday(&t_now, NULL);
                double elapsed = (t_now.tv_sec - t_start.tv_sec) +
                                 (t_now.tv_usec - t_start.tv_usec) / 1e6;
                if (elapsed >= 3.0) break;
            }
            fprintf(stderr, "SWEEP INCK=0x%02X DR=0x%02X: %d frames in ~3s = %.2f fps (err=%d)\n",
                    inc, dr, frames, frames / 3.0, errors);
            fflush(stderr);
        }
        fprintf(stderr, "SWEEP done.\n");
        goto cleanup;
    }

    fprintf(stderr, "[10] TCP server on port %d...\n", port);
    fflush(stderr);

    /* Arranca el canal de control (MODE raw|h265 + AE) en puerto separado */
    if (g_ctrl_port > 0 && !g_bench && !g_sweep) {
        g_ctrl_thread_arg = g_ctrl_port;
        if (pthread_create(&g_ctrl_thread, NULL, ctrl_thread_fn,
                           &g_ctrl_thread_arg) != 0)
            fprintf(stderr, "  FAIL pthread_create CTRL\n");
    }

    int server_fd = tcp_listen(port);
    if (server_fd < 0) goto cleanup;
    fprintf(stderr, "  Waiting for client on port %d...\n", port);
    fflush(stderr);

    data_loop(server_fd);
    close(server_fd);

    if (g_ctrl_port > 0 && !g_bench && !g_sweep)
        pthread_join(g_ctrl_thread, NULL);

cleanup:
    fprintf(stderr, "Cleanup...\n");
    fflush(stderr);
    if (g_i2c_fd >= 0) close(g_i2c_fd);
    if (g_mode == MODE_H265)
        h265_teardown();
    vi_teardown();
    usleep(200000);
    fprintf(stderr, "Done.\n");
    fflush(stderr);
    _exit(0);
}
