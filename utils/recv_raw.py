#!/usr/bin/env python3
"""
Receiver for IMX662 raw12 stream over TCP.
Usage: python3 recv_raw.py <soc_ip> [port]

Controls:
  ESC      = quit
  1/2      = exposure  -/+50 lines
  3/4      = analog gain  -/+256
  5/6      = digital gain  -/+256
  d/g/b    = demosaic / grayscale / bayer
  +/-      = display brightness
  s        = screenshot
  r        = reset exposure/gain to defaults
"""

import sys
import struct
import socket
import numpy as np

try:
    import cv2
except ImportError:
    print("Need opencv-python: pip3 install opencv-python")
    sys.exit(1)

WIDTH = 1920
HEIGHT = 1080


def recv_all(sock, n):
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def send_cmd(sock, cmd):
    """Send command and read OK response."""
    sock.sendall((cmd + "\n").encode())
    resp = b''
    sock.settimeout(2.0)
    try:
        while True:
            ch = sock.recv(1)
            if not ch:
                break
            resp += ch
            if ch == b'\n':
                break
    except socket.timeout:
        pass
    sock.settimeout(None)
    return resp.decode().strip()


def unpack_raw12(buf, w, h):
    """Decode packed raw12 (3 bytes / 2 px) to uint16 0..4095.

    LSB-first packing (verified 2026-08-10): byte structure shows b0 low
    nibble always 0 and b1 < 16, i.e. 8-bit data left-shifted <<4 inside a
    12-bit LSB-first container.  Previous MSB-first decode was wrong.
    """
    raw = np.frombuffer(buf, dtype=np.uint8)
    n_pixels = (len(raw) // 3) * 2
    triplets = raw[:n_pixels * 3 // 2].reshape(-1, 3)
    b0 = triplets[:, 0].astype(np.uint16)
    b1 = triplets[:, 1].astype(np.uint16)
    b2 = triplets[:, 2].astype(np.uint16)
    p0 = b0 | ((b1 & 0x0F) << 8)   # LSB-first
    p1 = (b1 >> 4) | (b2 << 4)      # LSB-first
    pixels = np.empty(n_pixels, dtype=np.uint16)
    pixels[0::2] = p0
    pixels[1::2] = p1
    return pixels.reshape(h, w)


def to_demosaic(bayer_16):
    bayer_8 = (bayer_16 >> 4).astype(np.uint8)
    return cv2.cvtColor(bayer_8, cv2.COLOR_BayerRG2BGR)


def to_bayer_display(bayer_16, w, h):
    bayer_8 = (bayer_16 >> 4).astype(np.uint8)
    rgb = np.zeros((h, w, 3), dtype=np.uint8)
    rgb[0::2, 0::2, 2] = bayer_8[0::2, 0::2]
    rgb[0::2, 1::2, 1] = bayer_8[0::2, 1::2]
    rgb[1::2, 0::2, 1] = bayer_8[1::2, 0::2]
    rgb[1::2, 1::2, 0] = bayer_8[1::2, 1::2]
    return rgb


def to_gray(bayer_16):
    return (bayer_16 >> 4).astype(np.uint8)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <soc_ip> [port]")
        sys.exit(1)

    ip = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5000

    print(f"Connecting to {ip}:{port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.connect((ip, port))
    print("Connected!")
    print("Controls:")
    print("  ESC=quit  s=screenshot  r=reset")
    print("  1/2=exposure -/+  3/4=analog gain -/+  5/6=digital gain -/+")
    print("  d/g/b=demosaic/grayscale/bayer  +/-=brightness")

    mode = 'd'
    brightness = 0
    screenshot_count = 0
    frame_count = 0
    exposure = 0
    again = 1024
    dgain = 1024

    while True:
        hdr = recv_all(sock, 12)
        if hdr is None:
            print("Connection closed")
            break

        w, h, stride, _, size = struct.unpack('<HHHHI', hdr)
        if w == 0 or h == 0 or size > 10_000_000:
            print(f"Bad header: w={w} h={h} size={size}")
            break

        data = recv_all(sock, size)
        if data is None:
            print("Connection closed during frame")
            break

        if len(data) != size:
            print(f"Incomplete frame: got {len(data)} expected {size}")
            break

        try:
            bayer_16 = unpack_raw12(data, w, h)

            if mode == 'd':
                img = to_demosaic(bayer_16)
            elif mode == 'b':
                img = to_bayer_display(bayer_16, w, h)
            else:
                gray = to_gray(bayer_16)
                img = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)

            if brightness != 0:
                img = np.clip(img.astype(np.int16) + brightness, 0, 255).astype(np.uint8)

            info = f"F:{frame_count} E:{exposure} A:{again} D:{dgain}"
            cv2.putText(img, info, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
            cv2.imshow('IMX662 Stream', img)
        except Exception as e:
            print(f"Display error: {e}")
            continue

        frame_count += 1
        key = cv2.waitKey(1) & 0xFF

        if key == 27:  # ESC
            break
        elif key == ord('s'):
            fname = f"screenshot_{screenshot_count:04d}.png"
            cv2.imwrite(fname, img)
            print(f"Saved {fname}")
            screenshot_count += 1
        elif key == ord('d'):
            mode = 'd'
            print("Mode: demosaic")
        elif key == ord('g'):
            mode = 'g'
            print("Mode: grayscale")
        elif key == ord('b'):
            mode = 'b'
            print("Mode: bayer")
        elif key == ord('+') or key == ord('='):
            brightness = min(brightness + 10, 200)
            print(f"Brightness: {brightness}")
        elif key == ord('-'):
            brightness = max(brightness - 10, -200)
            print(f"Brightness: {brightness}")
        elif key == ord('r'):
            exposure, again, dgain = 0, 1024, 1024
            send_cmd(sock, f"E {exposure}")
            send_cmd(sock, f"A {again}")
            send_cmd(sock, f"D {dgain}")
            print("Reset: E=0 A=1024 D=1024")
        elif key == ord('1'):
            exposure = min(exposure + 50, 1249)
            print(f"Exposure: {exposure} → {send_cmd(sock, f'E {exposure}')}")
        elif key == ord('2'):
            exposure = max(exposure - 50, 0)
            print(f"Exposure: {exposure} → {send_cmd(sock, f'E {exposure}')}")
        elif key == ord('3'):
            again = max(again - 256, 1024)
            print(f"Analog: {again} → {send_cmd(sock, f'A {again}')}")
        elif key == ord('4'):
            again = min(again + 256, 32768)
            print(f"Analog: {again} → {send_cmd(sock, f'A {again}')}")
        elif key == ord('5'):
            dgain = max(dgain - 256, 1024)
            print(f"Digital: {dgain} → {send_cmd(sock, f'D {dgain}')}")
        elif key == ord('6'):
            dgain = min(dgain + 256, 16384)
            print(f"Digital: {dgain} → {send_cmd(sock, f'D {dgain}')}")

    sock.close()
    cv2.destroyAllWindows()


if __name__ == '__main__':
    main()
