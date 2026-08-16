#include <stdio.h>
#include <stdlib.h>
#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"

int main(int argc, char **argv)
{
    int ret;
    printf("[test-init] ss_mpi_sys_init...\n");
    ret = ss_mpi_sys_init();
    printf("[test-init] sys_init 0x%08x\n", ret);
    if (ret != 0) {
        printf("[test-init] SYS init FAIL (0x%08x)\n", ret);
    } else {
        printf("[test-init] SYS init OK\n");
        ss_mpi_sys_exit();
    }
    return 0;
}