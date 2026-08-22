#!/usr/bin/env python3
"""Sube vi_online_exp al device (192.168.1.16, root/12345) y opcionalmente reboot.
Uso:
  python3 utils/deploy_vi_online_exp.py [--reboot]
"""
import base64
import os
import sys

import paramiko

IP = "192.168.1.16"
USER = "root"
PASS = "12345"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCAL = os.path.join(ROOT, "utils", "vi_online_exp")
REMOTE = "/tmp/vi_online_exp"


def main():
    reboot = "--reboot" in sys.argv

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(IP, username=USER, password=PASS, timeout=10)
    transport = ssh.get_transport()

    with open(LOCAL, "rb") as f:
        data = f.read()
    encoded = base64.b64encode(data).decode()
    chunks = [encoded[i:i + 4000] for i in range(0, len(encoded), 4000)]
    print(f"uploading {len(data)} bytes ({len(chunks)} chunks)")

    ch = transport.open_session()
    ch.exec_command(f"rm -f {REMOTE} /tmp/lib.b64")
    ch.recv_exit_status()

    for i, chunk in enumerate(chunks):
        cmd = f"echo -n \"{chunk}\" > /tmp/lib.b64" if i == 0 else \
              f"echo -n \"{chunk}\" >> /tmp/lib.b64"
        ch = transport.open_session()
        ch.exec_command(cmd)
        ch.recv_exit_status()

    ch = transport.open_session()
    ch.exec_command(
        f"base64 -d /tmp/lib.b64 > {REMOTE} && chmod +x {REMOTE} && "
        f"rm -f /tmp/lib.b64 && md5sum {REMOTE}")
    out = ch.makefile().read()
    print(out)
    ch.recv_exit_status()

    if reboot:
        print("rebooting device...")
        ch = transport.open_session()
        ch.exec_command("sync; reboot")
        ch.recv_exit_status()
        print("done (wait ~40s before SSH)")


if __name__ == "__main__":
    main()
