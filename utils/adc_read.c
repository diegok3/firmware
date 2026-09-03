/* adc_read.c — una conversión single-step del LSADC en el canal dado.
 * Uso: adc_read [dev] [chn]   (defaults: /dev/ot_lsadc 0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/ioctl.h>

#define LSADC_IOCTL_BASE 'A'
#define LSADC_IOC_MODEL_SEL  _IOWR(LSADC_IOCTL_BASE, 0, int)
#define LSADC_IOC_CHN_ENABLE _IOW(LSADC_IOCTL_BASE, 1, int)
#define LSADC_IOC_CHN_DISABLE _IOW(LSADC_IOCTL_BASE, 2, int)
#define LSADC_IOC_START      _IO(LSADC_IOCTL_BASE, 3)
#define LSADC_IOC_STOP       _IO(LSADC_IOCTL_BASE, 4)
#define LSADC_IOC_GET_CHNVAL _IOWR(LSADC_IOCTL_BASE, 5, int)

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/ot_lsadc";
    int chn = argc > 2 ? atoi(argv[2]) : 0;
    int mode = 0, fd, v, i;
    setbuf(stdout, NULL);
    fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (ioctl(fd, LSADC_IOC_MODEL_SEL, &mode) < 0) { perror("mode"); return 1; }
    if (ioctl(fd, LSADC_IOC_CHN_ENABLE, &chn) < 0) { perror("enable"); return 1; }
    for (i = 0; i < 5; i++) {
        if (ioctl(fd, LSADC_IOC_START) < 0) { perror("start"); return 1; }
        usleep(50000);
        v = ioctl(fd, LSADC_IOC_GET_CHNVAL, &chn);
        printf("chn%d sample%d = %d\n", chn, i, v);
        ioctl(fd, LSADC_IOC_STOP);
        usleep(200000);
    }
    ioctl(fd, LSADC_IOC_CHN_DISABLE, &chn);
    close(fd);
    return 0;
}
