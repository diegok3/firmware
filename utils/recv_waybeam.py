#!/usr/bin/env python3
"""recv_waybeam.py — ver y controlar en el PC el stream H.265 de waybeam_test.

El SoC envia por TCP:
  - video  (default 5999): access units H.265 AnnexB con header 12B:
        hdr[0:2] "WB", hdr[2] is_idr, hdr[3] 0,
        hdr[4:8] tamano frame (uint32 LE), hdr[8:12] pts (uint32 LE)
  - control (default 5998): comandos ASCII lineales
        E <us>   exposicion manual en microsegundos (modo MANUAL)
        A <again> ganancia analogica 22.10 (1024 = 1x)
        D <dgain> ganancia digital 22.10 (1024 = 1x)
        T        volver a AUTO

Uso:
    python3 utils/recv_waybeam.py 192.168.1.16 5999
    python3 utils/recv_waybeam.py 192.168.1.16 5999 --ctrl-port 5998
    python3 utils/recv_waybeam.py -o salida.h265 192.168.1.16 5999   # solo guardar

Requiere: ffmpeg + opencv-python + numpy.
Controles: botones en ventana o teclas:
    a/A d/D  -> ganancia analogica/digital +/- (1.5x por paso)
    e/E      -> exposicion us +/- (1.5x por paso)
    t        -> AUTO
    q        -> salir
"""
import socket
import struct
import subprocess
import sys
import time
import threading
import os

import numpy as np
import cv2

HDR_LEN = 12
STEP = 1.5
VIDEO_W = 1920
VIDEO_H = 1080
FRAME_BYTES = VIDEO_W * VIDEO_H * 3


def parse_args(argv):
    host = "192.168.1.16"
    port = 5999
    ctrl_port = 5998
    out = None
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "-o":
            out = args.pop(0)
        elif a == "--ctrl-port":
            ctrl_port = int(args.pop(0))
        elif a in ("-h", "--help"):
            print(__doc__)
            sys.exit(0)
        elif a.isdigit():
            port = int(a)
        else:
            host = a
    return host, port, ctrl_port, out


def read_exact(f, n):
    """Lee n bytes exactos desde socket (.recv) o archivo (subprocess .read)."""
    buf = b""
    while len(buf) < n:
        c = f.recv(n - len(buf)) if hasattr(f, "recv") else f.read(n - len(buf))
        if not c:
            raise ConnectionError("stream cerrado")
        buf += c
    return buf


def send_cmd(ctrl, cmd):
    try:
        ctrl.sendall((cmd + "\n").encode())
        print(f"  >> {cmd}", flush=True)
    except OSError as e:
        print(f"  !! control: {e}", flush=True)


# --------------------------------------------------------------- buttons --
def make_buttons():
    b = {}
    x, y = 5, 24
    for name, label in (("A-", "AGAIN -"), ("A+", "AGAIN +"),
                        ("D-", "DGAIN -"), ("D+", "DGAIN +"),
                        ("E-", "EXP  -"), ("E+", "EXP  +"),
                        ("AUTO", "AUTO")):
        wpx = 74 if name != "AUTO" else 56
        b[name] = ((x, y, wpx, 22), label)
        x += wpx + 4
    return b


BUTTONS = make_buttons()
STATE = {"again": 1024, "dgain": 1024, "exp_us": 10000}

def gain_step(v, up):
    v = int(round(v * STEP if up else v / STEP))
    return max(1, v)

def exp_step(v, up):
    v = int(round(v * STEP if up else v / STEP))
    return max(1, v)

def button_action(name, ctrl):
    st = STATE
    if name == "A-":
        st["again"] = gain_step(st["again"], False)
        send_cmd(ctrl, f"A {st['again']}")
    elif name == "A+":
        st["again"] = min(32768, gain_step(st["again"], True))
        send_cmd(ctrl, f"A {st['again']}")
    elif name == "D-":
        st["dgain"] = gain_step(st["dgain"], False)
        send_cmd(ctrl, f"D {st['dgain']}")
    elif name == "D+":
        st["dgain"] = min(16384, gain_step(st["dgain"], True))
        send_cmd(ctrl, f"D {st['dgain']}")
    elif name == "E-":
        st["exp_us"] = exp_step(st["exp_us"], False)
        send_cmd(ctrl, f"E {st['exp_us']}")
    elif name == "E+":
        st["exp_us"] = min(33333, exp_step(st["exp_us"], True))
        send_cmd(ctrl, f"E {st['exp_us']}")
    elif name == "AUTO":
        send_cmd(ctrl, "T")
    print(f"  btn {name}: A={st['again']} D={st['dgain']} E={st['exp_us']}us")

def mouse_cb(event, x, y, flags, param):
    ctrl = param
    if event != cv2.EVENT_LBUTTONDOWN:
        return
    for name, (rect, label) in BUTTONS.items():
        rx, ry, rw, rh = rect
        if rx <= x < rx + rw and ry <= y < ry + rh:
            button_action(name, ctrl)
            return


# -------------------------------------------------------- video reader -----
class VideoReader(threading.Thread):
    """Lee AUs H265 del SoC, los decodifica con ffmpeg y deja el ultimo frame."""

    def __init__(self, host, port, out=None):
        super().__init__(daemon=True)
        self.host = host
        self.port = port
        self.out = out
        self.latest = None
        self.lock = threading.Lock()
        self.frames = 0
        self.idr = 0
        self.stop = False

    def run(self):
        if self.out:
            dst = open(self.out, "wb")
            ff = None
        else:
            dst = None
            ff = subprocess.Popen(
                ["ffmpeg", "-loglevel", "error", "-fflags", "nobuffer",
                 "-probesize", "2M", "-analyzeduration", "500000",
                 "-f", "hevc", "-i", "pipe:0", "-f", "rawvideo",
                 "-pix_fmt", "bgr24", "-s", f"{VIDEO_W}x{VIDEO_H}", "pipe:1"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                bufsize=0)
        try:
            while not self.stop:
                try:
                    sock = socket.create_connection((self.host, self.port),
                                                    timeout=8)
                except OSError:
                    time.sleep(2)
                    continue
                print(f"[video] conectado a {self.host}:{self.port}", flush=True)
                try:
                    while not self.stop:
                        hdr = read_exact(sock, HDR_LEN)
                        if hdr[:2] != b"WB":
                            print(f"[video] header invalido {hdr[:2]!r}",
                                  flush=True)
                            sock.close()
                            break
                        (flen,) = struct.unpack("<I", hdr[4:8])
                        frame = read_exact(sock, flen)
                        if hdr[2]:
                            self.idr += 1
                        self.frames += 1
                        if ff:
                            if not hdr[2] and self.idr == 0:
                                continue  # esperar IDR (SPS/PPS) para ffmpeg
                            ff.stdin.write(frame)
                            ff.stdin.flush()
                            raw = read_exact(ff.stdout, FRAME_BYTES)
                            arr = np.frombuffer(raw, dtype=np.uint8)
                            arr = arr.reshape(VIDEO_H, VIDEO_W, 3)
                            with self.lock:
                                self.latest = arr
                        else:
                            dst.write(frame)
                            dst.flush()
                except ConnectionError:
                    print(f"[video] desconectado (frames={self.frames})",
                          flush=True)
                    sock.close()
                    time.sleep(2)
        except KeyboardInterrupt:
            pass
        finally:
            if ff:
                ff.stdin.close()
                try:
                    ff.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    ff.kill()
            if dst:
                dst.close()

    def get(self):
        with self.lock:
            return self.latest


def draw_ui(frame, fps):
    frame = frame.copy()
    for name, (rect, label) in BUTTONS.items():
        rx, ry, rw, rh = rect
        col = (40, 120, 200)
        cv2.rectangle(frame, (rx, ry), (rx + rw, ry + rh), col, -1)
        cv2.putText(frame, label, (rx + 3, ry + 15),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)
    st = STATE
    status = (f"A={st['again']} D={st['dgain']} E={st['exp_us']}us "
              f"| recv fps={fps:.1f}")
    cv2.putText(frame, status, (5, 16), cv2.FONT_HERSHEY_SIMPLEX, 0.45,
                (0, 255, 0), 1)
    return frame


def main():
    host, port, ctrl_port, out = parse_args(sys.argv[1:])
    reader = VideoReader(host, port, out)
    reader.start()

    if out:
        print(f"[recv] guardando H265 en {out} (Ctrl+C para salir)")
        try:
            while reader.is_alive():
                time.sleep(1)
        except KeyboardInterrupt:
            reader.stop = True
        print(f"[recv] total frames={reader.frames} idr={reader.idr}")
        return

    ctrl = None
    while True:
        try:
            ctrl = socket.create_connection((host, ctrl_port), timeout=5)
            print(f"[ctrl] control conectado a {host}:{ctrl_port}", flush=True)
            break
        except OSError:
            print(f"[ctrl] esperando control {host}:{ctrl_port}...", flush=True)
            time.sleep(2)

    cv2.namedWindow("waybeam preview")
    cv2.setMouseCallback("waybeam preview", mouse_cb, ctrl)
    print("[recv] controles: botones | a/A d/D ganancia, e/E exposicion, "
          "t auto, q salir", flush=True)

    t0 = time.time()
    shown = 0
    try:
        while True:
            frame = reader.get()
            if frame is None:
                time.sleep(0.02)
                continue
            now = time.time()
            dt = now - t0
            fps = shown / dt if dt > 0 else 0
            if dt >= 2.0:
                fps = shown / dt
                shown = 0
                t0 = now
            shown += 1
            cv2.imshow("waybeam preview", draw_ui(frame, fps))
            key = cv2.waitKey(30) & 0xFF
            if key in (ord("q"), 27):
                break
            elif key in (ord("a"), ord("A")):
                button_action("A-" if key == ord("a") else "A+", ctrl)
            elif key in (ord("d"), ord("D")):
                button_action("D-" if key == ord("d") else "D+", ctrl)
            elif key in (ord("e"), ord("E")):
                button_action("E-" if key == ord("e") else "E+", ctrl)
            elif key == ord("t"):
                button_action("AUTO", ctrl)
    except KeyboardInterrupt:
        pass
    finally:
        reader.stop = True
        cv2.destroyAllWindows()
        print(f"[recv] frames={reader.frames} idr={reader.idr}")


if __name__ == "__main__":
    main()