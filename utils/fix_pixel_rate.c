#include <stdio.h>
#include <dlfcn.h>

typedef int (*set_pipe_online_clock_t)(int pipe, unsigned int pixel_rate);

static set_pipe_online_clock_t real_func = NULL;

int ss_mpi_vi_set_pipe_online_clock(int pipe, unsigned int pixel_rate) {
    if (!real_func) {
        real_func = (set_pipe_online_clock_t)dlsym(RTLD_NEXT, "ss_mpi_vi_set_pipe_online_clock");
    }

    unsigned int original = pixel_rate;
    unsigned int fixed = 74250000;

    fprintf(stderr, "[fix_pixel_rate] ss_mpi_vi_set_pipe_online_clock(pipe=%d, pixel_rate=%u) -> forcing %u\n",
            pipe, original, fixed);

    return real_func(pipe, fixed);
}
