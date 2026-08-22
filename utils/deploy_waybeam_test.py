#!/usr/bin/env python3
"""Deploy de waybeam_test + libsns_imx662.so completa + open_sys_config.ko
con knob sns0_clk_hz al device (192.168.1.16, root/12345).

Uso:
  python3 utils/deploy_waybeam_test.py [--ip 192.168.1.16] [--only-binary]
      --only-binary   sube solo el binario (no .ko ni .so)
"""
import base64
import hashlib
import os
import sys

import paramiko

IP = "192.168.1.16"
USER = "root"
PASS = "12345"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FILES = {
    "waybeam_test": {
        "local": os.path.join(ROOT, "utils", "waybeam_test"),
        "remote_tmp": "/tmp/waybeam_test",
        "install": "chmod +x /tmp/waybeam_test",
    },
    "libsns_imx662.so": {
        "local": os.path.join(
            ROOT,
            "output/build/hisilicon-opensdk-ff20187b/libraries/sensor/"
            "hi3516cv6xx/sony_imx662/libsns_imx662.so",
        ),
        "remote_tmp": "/tmp/libsns_imx662.so",
        "install": "cp -f /tmp/libsns_imx662.so /usr/lib/sensors/libsns_imx662.so && "
                   "md5sum /usr/lib/sensors/libsns_imx662.so",
    },
    "open_sys_config.ko": {
        "local": os.path.join(
            ROOT,
            "output/build/hisilicon-opensdk-ff20187b/kernel/open_sys_config.ko",
        ),
        "remote_tmp": "/tmp/open_sys_config.ko",
        "install": "cp -f /tmp/open_sys_config.ko "
                   "/lib/modules/5.10.221/hisilicon/open_sys_config.ko && "
                   "strings /lib/modules/5.10.221/hisilicon/open_sys_config.ko | grep -c sns0_clk_hz",
    },
}


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def upload(ssh, transport, name, cfg, only_binary):
    local = cfg["local"]
    if not os.path.isfile(local):
        print(f"!! falta {local}")
        return False
    with open(local, "rb") as f:
        data = f.read()
    encoded = base64.b64encode(data).decode()
    print(f"[*] {name}: {len(data)} bytes (md5 {md5(local)})")
    ch = transport.open_session()
    ch.exec_command(f"rm -f {cfg['remote_tmp']} /tmp/lib.b64")
    ch.recv_exit_status()
    for i in range(0, len(encoded), 4000):
        chunk = encoded[i:i + 4000]
        cmd = f"echo -n \"{chunk}\" > /tmp/lib.b64" if i == 0 else \
              f"echo -n \"{chunk}\" >> /tmp/lib.b64"
        ch = transport.open_session()
        ch.exec_command(cmd)
        ch.recv_exit_status()
    ch = transport.open_session()
    ch.exec_command(
        f"base64 -d /tmp/lib.b64 > {cfg['remote_tmp']} && {cfg['install']} && "
        f"rm -f /tmp/lib.b64")
    out = ch.makefile().read().decode()
    ch.recv_exit_status()
    if out.strip():
        print(f"    -> {out.strip()}")
    return True


def main():
    only_binary = "--only-binary" in sys.argv
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        ssh.connect(IP, username=USER, password=PASS, timeout=10)
    except Exception as e:
        print(f"!! conexion fallo: {e}")
        return 1
    transport = ssh.get_transport()

    ok = True
    for name, cfg in FILES.items():
        if only_binary and name != "waybeam_test":
            continue
        ok = upload(ssh, transport, name, cfg, only_binary) and ok

    ssh.close()
    print()
    print("== Siguientes pasos (en el device) ==")
    print(" 1) Detener cualquier streamer (SIGTERM) y REBOOT limpio:")
    print("      reboot")
    print(" 2) Verificar el knob del kernel:")
    print("      ls /sys/module/open_sys_config/parameters/sns0_clk_hz")
    print(" 3) Config waybeam (4 lanes, 37.125 MHz):")
    print("      /tmp/waybeam_test --lanes 4 --mclk-hz 37125000 \\")
    print("          --incksel 0x01 --datarate 0x03 --lanemode 0x03 --bench 10")
    print("    luego con TCP:")
    print("      /tmp/waybeam_test --lanes 4 --port 5999")
    print(" 4) Config validada (1 lane, 27 MHz):")
    print("      /tmp/waybeam_test --preset validated --bench 10")
    print(" 5) El knodb de 37.125 MHz se puede probar a mano:")
    print("      echo 37125000 > /sys/module/open_sys_config/parameters/sns0_clk_hz")
    print("      echo 27000000 > /sys/module/open_sys_config/parameters/sns0_clk_hz")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())