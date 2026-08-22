#!/bin/bash
# Compile astro_streamer.c (raw12 + H.265, canal de control 5998).
# Run from utils/ with the firmware root as parent.
set -e
SDK=../output/host/opt/ext-toolchain/sdk
CC=../output/host/bin/arm-openipc-linux-musleabi-gcc
KO=../output/build/hisilicon-opensdk-ff20187b/kernel/include/hi3516cv6xx
ISP=../output/build/hisilicon-opensdk-ff20187b/libraries/isp/include/hi3516cv6xx

$CC -O2 -Wall -o astro_streamer astro_streamer.c \
  -I$SDK/include -I$KO -I$KO/isp_ext_inc -I$KO/exp_inc \
  -I$ISP -I$ISP/ext_inc -I$ISP/3a \
  -L$SDK/lib -Wl,-rpath,$SDK/lib -Wl,--allow-shlib-undefined \
  -Wl,--start-group \
  -lss_mpi -lss_mpi_isp -lss_mpi_ae -lss_mpi_awb -lot_mpi_isp \
  -lbnr -lcalcflicker -ldrc -ldehaze -lacs -lir_auto -lldci -lextend_stats \
  -lss_mpi_sysmem -lss_mpi_sysbind -lot_osal -lsecurec \
  -Wl,--end-group -lpthread -ldl -lm

echo "OK: utils/astro_streamer"