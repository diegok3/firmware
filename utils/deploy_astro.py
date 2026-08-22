#!/usr/bin/env python3
"""Deploy de astro_streamer (raw12 + H.265) al device (192.168.1.16, root/12345).

Uso:
  python3 utils/deploy_astro.py [--ip 192.168.1.16]
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


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def upload(ssh, transport, name, local, remote_tmp, install):
    if not os.path.isfile(local):
        print(f"!! falta {local}")
        return False
    with open(local, "rb") as f:
        data = f.read()
    encoded = base64.b64encode(data).decode()
    print(f"[*] {name}: {len(data)} bytes (md5 {md5(local)})")
    ch = transport.open_session()
    ch.exec_command(f"rm -f {remote_tmp} /tmp/lib.b64")
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
        f"base64 -d /tmp/lib.b64 > {remote_tmp} && {install} && "
        f"rm -f /tmp/lib.b64")
    out = ch.makefile().read().decode()
    ch.recv_exit_status()
    if out.strip():
        print(f"    -> {out.strip()}")
    return True


def main():
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--ip" and i + 1 < len(args):
            global IP
            IP = args[i + 1]

    local = os.path.join(ROOT, "utils", "astro_streamer")
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        ssh.connect(IP, username=USER, password=PASS, timeout=10)
    except Exception as e:
        print(f"!! conexion fallo: {e}")
        return 1
    transport = ssh.get_transport()

    ok = upload(ssh, transport, "astro_streamer", local,
                "/tmp/astro_streamer", "chmod +x /tmp/astro_streamer")

    ssh.close()
    print()
    print("== Siguientes pasos (en el device) ==")
    print(" 1) Detener cualquier streamer y REBOOT limpio:")
    print("      killall -TERM astro_streamer waybeam_test 2>/dev/null; reboot")
    print(" 2) Tras el reboot, correr en modo RAW:")
    print("      /tmp/astro_streamer 5000 --mode raw")
    print(" 3) O en modo H.265:")
    print("      /tmp/astro_streamer 5000 --mode h265")
    print(" 4) Bench H.265 (10s):")
    print("      /tmp/astro_streamer 5000 --mode h265 --bench 10")
    print(" 5) En el PC (viewer con boton CONFIG para conmutar):")
    print("      python3 utils/recv_astro.py 192.168.1.16 5000")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())