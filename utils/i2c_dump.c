#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

#define I2C_ADDR 0x1A  /* 7-bit */

#ifndef OT_I2C_SLAVE_FORCE
#define OT_I2C_SLAVE_FORCE 0x0706
#endif

int i2c_fd = -1;

int i2c_read(unsigned short reg, unsigned char *val) {
    unsigned char buf[2] = { (reg >> 8) & 0xFF, reg & 0xFF };
    if (write(i2c_fd, buf, 2) != 2) return -1;
    if (read(i2c_fd, val, 1) != 1) return -1;
    return 0;
}

void read_range(unsigned short start, unsigned short end) {
    unsigned char val;
    printf("Reading registers 0x%04X-0x%04X:\n", start, end);
    for (unsigned short reg = start; reg <= end; reg++) {
        if (i2c_read(reg, &val) == 0) {
            printf("  [0x%04X] = 0x%02X\n", reg, val);
        } else {
            printf("  [0x%04X] = <ERR>\n", reg);
        }
    }
}

int main(int argc, char *argv[]) {
    i2c_fd = open("/dev/i2c-0", O_RDWR);
    if (i2c_fd < 0) { perror("open i2c-0"); return 1; }
    if (ioctl(i2c_fd, OT_I2C_SLAVE_FORCE, I2C_ADDR) < 0) { perror("ioctl"); return 1; }

    unsigned char chip_id = 0;
    i2c_read(0x30DC, &chip_id);
    printf("Chip ID: 0x%02X\n", chip_id);

    if (argc > 1) {
        unsigned short reg = (unsigned short)strtol(argv[1], NULL, 0);
        if (argc > 2) {
            /* Write: i2c_dump 0x3444 0x2C */
            unsigned char val = (unsigned char)strtol(argv[2], NULL, 0);
            unsigned char buf[3] = { (reg >> 8) & 0xFF, reg & 0xFF, val };
            write(i2c_fd, buf, 3);
            printf("Wrote [0x%04X] = 0x%02X\n", reg, val);
            usleep(100000);
        }
        /* Read single register */
        unsigned char v;
        if (i2c_read(reg, &v) == 0)
            printf("[0x%04X] = 0x%02X\n", reg, v);
        else
            printf("[0x%04X] = <ERR>\n", reg);
    } else {
        /* Dump key registers */
        printf("\n=== Mode regs (0x30xx) ===\n");
        read_range(0x3000, 0x3052);
        printf("\n=== PLL (0x3440-0x349A) ===\n");
        read_range(0x3440, 0x349A);
        printf("\n=== MIPI TX (0x34xx) ===\n");
        read_range(0x3400, 0x343F);
    }

    close(i2c_fd);
    return 0;
}
