#!/bin/bash
# Compile indi_mini.c (mini servidor INDI, path RAW ISP-bypass).
# Sin ISP/VPSS/VENC: link mínimo para ahorrar flash/RAM.
# Run from utils/ with the firmware root as parent.
set -e
SDK=../output/host/opt/ext-toolchain/sdk
CC=../output/host/bin/arm-openipc-linux-musleabi-gcc
KO=../output/build/hisilicon-opensdk-ff20187b/kernel/include/hi3516cv6xx

$CC -O2 -Wall -o indi_mini indi_mini.c \
  -I$SDK/include -I$KO -I$KO/isp_ext_inc \
  -L$SDK/lib -Wl,-rpath,$SDK/lib \
  -lss_mpi -lss_mpi_sysmem -lss_mpi_sysbind -lot_osal -lsecurec \
  -lpthread -ldl -lm

echo "OK: utils/indi_mini"
ls -lh indi_mini
