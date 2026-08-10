#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

#define I2C_ADDR 0x1A
#ifndef OT_I2C_SLAVE_FORCE
#define OT_I2C_SLAVE_FORCE 0x0706
#endif
#define OT_MIPI_ENABLE_SENSOR_CLOCK _IOW('m', 0x10, int)

int i2c_fd = -1;

int i2c_read(unsigned short reg, unsigned char *val) {
    unsigned char buf[2] = { (reg >> 8) & 0xFF, reg & 0xFF };
    if (write(i2c_fd, buf, 2) != 2) return -1;
    if (read(i2c_fd, val, 1) != 1) return -1;
    return 0;
}

void enable_mclk_no_reset(void) {
    int fd = open("/dev/ot_mipi_rx", O_RDWR);
    if (fd < 0) { fprintf(stderr, "Cannot open mipi_rx\n"); return; }
    int clk = 0;
    ioctl(fd, OT_MIPI_ENABLE_SENSOR_CLOCK, &clk);
    usleep(50000);
    close(fd);
    fprintf(stderr, "MCLK enabled (no reset)\n");
}

int main(int argc, char *argv[]) {
    enable_mclk_no_reset();

    i2c_fd = open("/dev/i2c-0", O_RDWR);
    if (i2c_fd < 0) { perror("open i2c-0"); return 1; }
    if (ioctl(i2c_fd, OT_I2C_SLAVE_FORCE, I2C_ADDR) < 0) { perror("ioctl"); return 1; }

    unsigned char chip_id = 0;
    i2c_read(0x30DC, &chip_id);
    printf("Chip ID: 0x%02X\n", chip_id);

    if (argc > 1) {
        unsigned short reg = (unsigned short)strtol(argv[1], NULL, 0);
        if (argc > 2) {
            unsigned char val = (unsigned char)strtol(argv[2], NULL, 0);
            unsigned char buf[3] = { (reg >> 8) & 0xFF, reg & 0xFF, val };
            write(i2c_fd, buf, 3);
            printf("Wrote [0x%04X] = 0x%02X\n", reg, val);
            usleep(100000);
        }
        unsigned char v;
        if (i2c_read(reg, &v) == 0)
            printf("[0x%04X] = 0x%02X\n", reg, v);
        else
            printf("[0x%04X] = <ERR>\n", reg);
    } else {
        printf("\n=== Mode regs (0x30xx) ===\n");
        for (unsigned short reg = 0x3000; reg <= 0x3052; reg++) {
            unsigned char v;
            if (i2c_read(reg, &v) == 0)
                printf("  [0x%04X] = 0x%02X\n", reg, v);
            else
                printf("  [0x%04X] = <ERR>\n", reg);
        }
        printf("\n=== PLL (0x3440-0x349A) ===\n");
        for (unsigned short reg = 0x3440; reg <= 0x349A; reg++) {
            unsigned char v;
            if (i2c_read(reg, &v) == 0)
                printf("  [0x%04X] = 0x%02X\n", reg, v);
            else
                printf("  [0x%04X] = <ERR>\n", reg);
        }
    }

    close(i2c_fd);
    return 0;
}
