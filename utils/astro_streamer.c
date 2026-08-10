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
#include "ot_mipi_rx.h"
#include "ot_i2c.h"
#include "ot_mpi_vi.h"
#include "ot_mpi_vb.h"
#include "ot_mpi_sys.h"

#define VI_DEV_ID       0
#define VI_PIPE_ID      0
#define VI_CHN_ID       0
#define IMG_WIDTH       1920
#define IMG_HEIGHT      1080
#define I2C_DEV_ADDR    0x34
#define IMG_W_STRIDE    ((IMG_WIDTH * 12 + 127) / 128 * 128 / 8)
#define RAW12_BUF_SIZE  (IMG_W_STRIDE * IMG_HEIGHT)
#define TCP_PORT        5000
#define HEADER_SIZE     48

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
static int g_incksel = 0x03;
static int g_datarate = 0x05;
static int g_sweep = 0;
static int g_bench = 0;
static int g_vc_num = 0;
static int g_shrconv = 1;   /* Sony convention: SHR = VMAX - exposure (V4L2 ref) */

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

static td_s32 enable_mclk_and_reset_sensor(void)
{
    td_s32 fd;
    lane_divide_mode_t lane_mode = LANE_DIVIDE_MODE_0;
    sns_clk_source_t clk_source = 0;
    sns_rst_source_t rst_source = 0;

    fd = open("/dev/ot_mipi_rx", O_RDWR);
    if (fd < 0) return -1;

    ioctl(fd, OT_MIPI_SET_HS_MODE, &lane_mode);
    ioctl(fd, OT_MIPI_ENABLE_SENSOR_CLOCK, &clk_source);
    ioctl(fd, OT_MIPI_RESET_SENSOR, &rst_source);
    usleep(10000);
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
        fprintf(stderr, "  E=%u VMAX=%u A=%u D=%u TYPE=%s OBJ=%s\n",
                g_exposure_lines, g_vmax, g_again, g_dgain, g_frame_type, g_object);
    }
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
    }
    if (argc > 1 && argv[1][0] != '-') port = atoi(argv[1]);
    if (port <= 0) port = TCP_PORT;

    fprintf(stderr, "=== ASTRO Streamer (port %d) ===\n", port);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    fprintf(stderr, "[0] MPI init...\n");
    ret = ot_mpi_sys_init();
    if (ret != TD_SUCCESS) {
        fprintf(stderr, "  sys_init 0x%x, cleaning...\n", ret);
        ot_mpi_vi_disable_chn(VI_PIPE_ID, VI_CHN_ID);
        ot_mpi_vi_stop_pipe(VI_PIPE_ID);
        ot_mpi_vi_destroy_pipe(VI_PIPE_ID);
        ot_mpi_vi_disable_dev(VI_DEV_ID);
        ot_mpi_sys_exit();
        usleep(500000);
        ret = ot_mpi_sys_init();
        if (ret != TD_SUCCESS) {
            fprintf(stderr, "  sys_init FAILED: 0x%x\n", ret);
            return 1;
        }
    }
    fprintf(stderr, "  OK\n");

    fprintf(stderr, "[2] VB init...\n");
    ret = ot_mpi_vb_exit();
    fprintf(stderr, "  vb_exit: 0x%x\n", ret);
    usleep(50000);
    ot_vb_cfg vb_cfg;
    memset(&vb_cfg, 0, sizeof(vb_cfg));
    vb_cfg.max_pool_cnt = 1;
    vb_cfg.common_pool[0].blk_size = RAW12_BUF_SIZE;
    vb_cfg.common_pool[0].blk_cnt = 6;
    vb_cfg.common_pool[0].remap_mode = OT_VB_REMAP_MODE_NONE;
    strncpy(vb_cfg.common_pool[0].mmz_name, "anonymous", sizeof(vb_cfg.common_pool[0].mmz_name) - 1);
    ret = ot_mpi_vb_set_cfg(&vb_cfg);
    fprintf(stderr, "  set_cfg: 0x%x\n", ret);
    ret = ot_mpi_vb_init();
    fprintf(stderr, "  init: 0x%x\n", ret);

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

    fprintf(stderr, "[4] VI-VPSS mode...\n");
    ot_vi_vpss_mode vpss_mode;
    memset(&vpss_mode, 0, sizeof(vpss_mode));
    for (int i = 0; i < 4; i++) vpss_mode.mode[i] = OT_VI_OFFLINE_VPSS_OFFLINE;
    ot_mpi_sys_set_vi_vpss_mode(&vpss_mode);

    fprintf(stderr, "[5] MCLK + sensor...\n");
    enable_mclk_and_reset_sensor();

    fprintf(stderr, "[5b] MIPI RX...\n");
    int mipi_fd = open("/dev/ot_mipi_rx", O_RDWR);
    if (mipi_fd >= 0) {
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
        ioctl(mipi_fd, OT_MIPI_SET_DEV_ATTR, &dev_attr);
        combo_dev_t devno = 0;
        ioctl(mipi_fd, OT_MIPI_UNRESET_MIPI, &devno);
        usleep(10000);
        close(mipi_fd);
    }

    fprintf(stderr, "[6] Sensor I2C...\n");
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
            if (init_sensor_full() == 0) fprintf(stderr, "  Sensor OK\n");
        }
    }
    usleep(200000);

    fprintf(stderr, "[7-9] VI pipeline...\n");
    ot_mpi_vi_disable_chn(VI_PIPE_ID, VI_CHN_ID);
    ot_mpi_vi_stop_pipe(VI_PIPE_ID);
    ot_mpi_vi_unbind(VI_DEV_ID, VI_PIPE_ID);
    ot_mpi_vi_disable_dev(VI_DEV_ID);
    usleep(50000);

    ot_vi_dev_attr dev_attr;
    memset(&dev_attr, 0, sizeof(dev_attr));
    dev_attr.intf_mode = OT_VI_INTF_MODE_MIPI;
    dev_attr.work_mode = OT_VI_WORK_MODE_MULTIPLEX_1;
    dev_attr.scan_mode = OT_VI_SCAN_PROGRESSIVE;
    dev_attr.data_type = OT_VI_DATA_TYPE_RAW;
    dev_attr.in_size.width = IMG_WIDTH;
    dev_attr.in_size.height = IMG_HEIGHT;
    dev_attr.data_rate = OT_DATA_RATE_X1;
    dev_attr.component_mask[0] = 0xFFF0000;
    ret = ot_mpi_vi_set_dev_attr(VI_DEV_ID, &dev_attr);
    if (ret != TD_SUCCESS) { fprintf(stderr, "  set_dev_attr: 0x%x\n", ret); goto cleanup; }
    ret = ot_mpi_vi_enable_dev(VI_DEV_ID);
    if (ret != TD_SUCCESS) { fprintf(stderr, "  enable_dev: 0x%x\n", ret); goto cleanup; }
    ret = ot_mpi_vi_bind(VI_DEV_ID, VI_PIPE_ID);
    if (ret != TD_SUCCESS) { fprintf(stderr, "  bind: 0x%x\n", ret); goto cleanup; }

    ot_vi_pipe_attr pipe_attr;
    memset(&pipe_attr, 0, sizeof(pipe_attr));
    pipe_attr.isp_bypass = TD_TRUE;
    pipe_attr.size.width = IMG_WIDTH;
    pipe_attr.size.height = IMG_HEIGHT;
    pipe_attr.pixel_format = OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
    pipe_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    pipe_attr.frame_rate_ctrl.src_frame_rate = -1;
    pipe_attr.frame_rate_ctrl.dst_frame_rate = -1;
    ot_mpi_vi_stop_pipe(VI_PIPE_ID);
    ot_mpi_vi_create_pipe(VI_PIPE_ID, &pipe_attr);
    ot_mpi_vi_set_pipe_attr(VI_PIPE_ID, &pipe_attr);
    {
        td_s32 vcret = ot_mpi_vi_set_pipe_vc_number(VI_PIPE_ID, g_vc_num);
        if (vcret != TD_SUCCESS)
            fprintf(stderr, "  set_pipe_vc_number(%d): 0x%x\n", g_vc_num, vcret);
        else
            fprintf(stderr, "  vc_num set to %d\n", g_vc_num);
    }
    ot_mpi_vi_start_pipe(VI_PIPE_ID);

    ot_vi_chn_attr chn_attr;
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
    ot_mpi_vi_disable_chn(VI_PIPE_ID, VI_CHN_ID);
    ot_mpi_vi_set_chn_attr(VI_PIPE_ID, VI_CHN_ID, &chn_attr);
    ret = ot_mpi_vi_enable_chn(VI_PIPE_ID, VI_CHN_ID);
    if (ret != TD_SUCCESS) { fprintf(stderr, "  enable_chn: 0x%x\n", ret); goto cleanup; }
    fprintf(stderr, "  VI OK\n");

    if (g_bench) {
        fprintf(stderr, "[10] BENCH mode: measuring frame rate for %ds...\n", bench_seconds);
        fflush(stderr);

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

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) goto cleanup;
    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(server_fd); goto cleanup; }
    if (listen(server_fd, 1) < 0) { close(server_fd); goto cleanup; }
    fprintf(stderr, "  Waiting for client on port %d...\n", port);
    fflush(stderr);

    int mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (mem_fd < 0) goto cleanup;

    int mem_mmaped = 0;
    void *mem_map = NULL;
    td_u64 mem_map_len = 0;
    td_u64 mem_map_phys = 0;

    char cmd_buf[512];
    int cmd_len = 0;
    td_u32 frame_index = 0;

    while (g_running) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) continue;

        fprintf(stderr, "  Client: %s\n", inet_ntoa(cli_addr.sin_addr));
        fflush(stderr);

        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        int consecutive_fail = 0;
        cmd_len = 0;
        g_burst_remaining = 0;

        while (g_running) {
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
                                /* NOTE: no text response on the data socket.
                                   Any bytes sent here would corrupt the binary
                                   frame stream. Status is in the frame header. */
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
    close(mem_fd);
    close(server_fd);

cleanup:
    fprintf(stderr, "Cleanup...\n");
    fflush(stderr);
    if (g_i2c_fd >= 0) close(g_i2c_fd);
    ot_mpi_vi_disable_chn(VI_PIPE_ID, VI_CHN_ID);
    ot_mpi_vi_stop_pipe(VI_PIPE_ID);
    ot_mpi_vi_destroy_pipe(VI_PIPE_ID);
    ot_mpi_vi_unbind(VI_DEV_ID, VI_PIPE_ID);
    ot_mpi_vi_disable_dev(VI_DEV_ID);
    usleep(200000);
    fprintf(stderr, "Done.\n");
    fflush(stderr);
    _exit(0);
}
