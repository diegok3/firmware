/* i2c_peek: lee registros 16-bit del IMX662 por /dev/i2c-0 con repeated-start.
 * Uso: i2c_peek 3000 3001 3028 3029 302A 3050 30DC
 * Imprime REG=VV o REG=ERR. Solo lectura (no toca el sensor en uso).
 * Compilar: ../output/host/bin/arm-openipc-linux-musleabi-gcc -O2 -o i2c_peek i2c_peek.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>

#define I2C_RDWR 0x0707
#define I2C_M_RD 0x0001
#define SENSOR_ADDR 0x1a /* 7-bit (0x34 en 8-bit) */

struct i2c_msg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    uint8_t *buf;
};
struct i2c_rdwr_ioctl_data {
    struct i2c_msg *msgs;
    uint32_t nmsgs;
};

static int g_fd = -1;

static int read_reg(unsigned reg, unsigned *val)
{
    unsigned char rbuf[2] = { (reg >> 8) & 0xFF, reg & 0xFF };
    unsigned char v = 0;
    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data rdwr;
    msgs[0].addr = SENSOR_ADDR; msgs[0].flags = 0;
    msgs[0].len = 2; msgs[0].buf = rbuf;
    msgs[1].addr = SENSOR_ADDR; msgs[1].flags = I2C_M_RD;
    msgs[1].len = 1; msgs[1].buf = &v;
    rdwr.msgs = msgs; rdwr.nmsgs = 2;
    if (ioctl(g_fd, I2C_RDWR, &rdwr) < 0)
        return -1;
    *val = v;
    return 0;
}

int main(int argc, char **argv)
{
    int i, rc = 0;
    if (argc < 2) {
        fprintf(stderr, "uso: %s REGHEX [REGHEX...]\n", argv[0]);
        return 1;
    }
    g_fd = open("/dev/i2c-0", O_RDWR);
    if (g_fd < 0) { perror("open /dev/i2c-0"); return 1; }
    for (i = 1; i < argc; i++) {
        unsigned reg = (unsigned)strtoul(argv[i], NULL, 16);
        unsigned v = 0;
        if (read_reg(reg, &v) == 0)
            printf("%04X=%02X\n", reg, v);
        else {
            printf("%04X=ERR\n", reg);
            rc = 1;
        }
    }
    close(g_fd);
    return rc;
}
