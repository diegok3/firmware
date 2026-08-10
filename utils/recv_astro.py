#!/usr/bin/env python3
"""
recv_astro.py - PC client for astro_streamer on Hi3516CV610 + IMX662.

Features:
  - Live preview (raw12 -> grayscale/bayer), auto-stretch for dim astro frames
  - Exposure control: microseconds (T), milliseconds (M), lines (E), preview (P)
  - Gain control: analog (A), digital (D), ISO (I)
  - VMAX control (V) and frame-period query
  - FITS capture (B<burst> command flags frames; saved here with metadata)
  - Dark frame subtraction (capture dark, then subtract from lights)
  - Histogram + focus aid (max gradient metric)
  - Sensor temperature display

Usage:
  python3 recv_astro.py [host] [port]
Keys:
  q            quit
  s            save next captured burst frames (see below)
  d            mark current view as dark (average into master dark)
  b            toggle bayer color / grayscale
  g            toggle gamma stretch
  +/-          brightness
  a            capture dark burst
  l            capture light burst
  x            cancel burst
  o <name>     set object name
  1..9         100us..9s quick exposure presets
Space          start/stop burst capture (uses current exposure)
"""
import socket, struct, sys, time, os, threading, re
import numpy as np

HDR = struct.Struct("<2sHHHIIQIIIIiB3x")  # 48 bytes matching C header

try:
    import cv2
    HAVE_CV = True
except ImportError:
    HAVE_CV = False
    print("OpenCV not available; preview disabled (FITS save still works)")

DARK = None          # master dark (float32, 16-bit scale)
GAMMA = True
COLOR = False
BRIGHTNESS = 1.0
OBJECT = "UNKNOWN"
BURST = 0            # number of burst frames left to save
SAVE_BURST = False
CAPTURE_DIR = "astro_captures"
os.makedirs(CAPTURE_DIR, exist_ok=True)

state = {"exp": 0, "vmax": 0, "again": 1024, "dgain": 1024,
         "temp": 0.0, "ftype": "LIGHT", "obj": OBJECT, "fps": 0.0}

PRESETS = {  # key: exposure time
    '1': 100, '2': 1000, '3': 10000, '4': 50000,
    '5': 100000, '6': 300000, '7': 1000000,
    '8': 5000000, '9': 10000000,
}

# ---------------------------------------------------------------- buttons --
# Clickable on-screen buttons.  Each button mutates a value and sends the
# matching command to the streamer.  Steps are multiplicative (1.5x) so gain
# can be dialed down fast when the frame is saturated.
STEP = 1.5

def gain_step(v, up):
    v = int(round(v * STEP if up else v / STEP))
    return max(1, v)

def exp_step(v, up):
    v = int(round(v * STEP if up else v / STEP))
    return max(1, v)

def make_buttons():
    """Return dict: name -> (rect, label).  Drawn in order; y fixed ~30px."""
    b = {}
    x = 5
    y = 24
    for name, label in (("A-", "AGAIN -"), ("A+", "AGAIN +"),
                        ("D-", "DGAIN -"), ("D+", "DGAIN +"),
                        ("T-", "EXP  -"), ("T+", "EXP  +"),
                        ("GAIN-AUTO", "AUTO")):
        wpx = 74 if name != "GAIN-AUTO" else 56
        b[name] = ((x, y, wpx, 22), label)
        x += wpx + 4
    return b

BUTTONS = make_buttons()
BUTTON_STATE = {"again": 1024, "dgain": 1024, "exp_us": 0}

def button_action(name, sock):
    st = BUTTON_STATE
    if name == "A-":
        st["again"] = max(1, int(round(st["again"] / STEP)))
        send_cmd(sock, f"A {st['again']}")
    elif name == "A+":
        st["again"] = min(32768, int(round(st["again"] * STEP)))
        send_cmd(sock, f"A {st['again']}")
    elif name == "D-":
        st["dgain"] = max(1, int(round(st["dgain"] / STEP)))
        send_cmd(sock, f"D {st['dgain']}")
    elif name == "D+":
        st["dgain"] = min(16384, int(round(st["dgain"] * STEP)))
        send_cmd(sock, f"D {st['dgain']}")
    elif name == "T-":
        st["exp_us"] = max(1, int(round(st["exp_us"] / STEP)))
        send_cmd(sock, f"T {st['exp_us']}")
    elif name == "T+":
        st["exp_us"] = min(30000000, int(round(st["exp_us"] * STEP)))
        send_cmd(sock, f"T {st['exp_us']}")
    elif name == "GAIN-AUTO":
        set_iso(sock, 100)      # ISO 100 -> again/dgain back near nominal
        set_exposure_us(sock, 1000)
    print(f"  btn {name}: A={st['again']} D={st['dgain']} T={st['exp_us']}us")

def mouse_cb(event, x, y, flags, param):
    sock = param
    if event != cv2.EVENT_LBUTTONDOWN:
        return
    for name, (rect, label) in BUTTONS.items():
        rx, ry, rw, rh = rect
        if rx <= x < rx + rw and ry <= y < ry + rh:
            button_action(name, sock)
            return

def send_cmd(sock, cmd):
    """Fire-and-forget command. The data socket carries ONLY binary frames;
    any read here would desync the stream. Sensor status is read from the
    48-byte frame header (see read_frame), not from a text response."""
    sock.sendall((cmd + "\n").encode())
    return ""

def set_exposure_us(sock, us):
    send_cmd(sock, f"T {int(us)}")

def set_exposure_ms(sock, ms):
    send_cmd(sock, f"M {int(ms)}")

def set_exposure_lines(sock, lines):
    send_cmd(sock, f"E {int(lines)}")

def set_iso(sock, iso):
    send_cmd(sock, f"I {int(iso)}")

def set_gain(sock, again=None, dgain=None):
    if again is not None: send_cmd(sock, f"A {int(again)}")
    if dgain is not None: send_cmd(sock, f"D {int(dgain)}")

def set_vmax(sock, vmax):
    send_cmd(sock, f"V {int(vmax)}")

def preview_mode(sock):
    send_cmd(sock, "P")

def set_frame_type(sock, t):
    send_cmd(sock, f"F {t}")

def set_object(sock, name):
    send_cmd(sock, f"C {name}")

def read_frame(sock):
    """Read one 48-byte header + raw12 payload. Returns (hdr_dict, raw_np) or None."""
    hdr_data = b""
    while len(hdr_data) < HDR.size:
        chunk = sock.recv(HDR.size - len(hdr_data))
        if not chunk: return None
        hdr_data += chunk
    magic, w, h, stride, size, idx, ts, exp_us, again, dgain, vmax, temp, cap = HDR.unpack(hdr_data)
    if magic != b'AS':
        return None
    payload = b""
    while len(payload) < size:
        chunk = sock.recv(size - len(payload))
        if not chunk: return None
        payload += chunk
    raw = np.frombuffer(payload, dtype=np.uint8).reshape(h, stride)
    meta = dict(w=w, h=h, stride=stride, size=size, idx=idx, ts_us=ts,
                exp_us=exp_us, again=again, dgain=dgain, vmax=vmax,
                temp=temp, cap=cap)
    state["exp"], state["vmax"] = exp_us, vmax
    state["again"], state["dgain"] = again, dgain
    state["temp"] = temp
    return meta, raw

def raw12_to_uint16(raw):
    """Decode packed raw12 (3 bytes / 2 px) to uint16 0..4095.

    Verified LSB-first packing (2026-08-10): b0 low nibble always 0 and
    b1 < 16 prove the data is 8-bit values left-shifted <<4 inside a
    12-bit LSB-first container.  The previous MSB-first decode was wrong.
    """
    w = raw.shape[1] * 2 // 3  # 1920 for stride 2880
    h = raw.shape[0]
    out = np.zeros((h, w), dtype=np.uint16)
    b = raw[:, :w // 2 * 3]
    r = b.reshape(h, -1, 3).astype(np.uint32)
    p0 = r[:, :, 0] | ((r[:, :, 1] & 0x0F) << 8)   # LSB-first
    p1 = (r[:, :, 1] >> 4) | (r[:, :, 2] << 4)      # LSB-first
    out[:, 0::2] = p0
    out[:, 1::2] = p1
    return out

def stretch(img16, gamma=True):
    """Auto-stretch 16-bit to 8-bit, robust to dim astro frames."""
    lo = np.percentile(img16, 0.5)
    hi = np.percentile(img16, 99.5)
    if hi - lo < 1:
        # Constant frame: map 0->0 and max->255 so a saturated
        # (all-4080) frame shows as white instead of black.
        hi = float(img16.max())
        lo = 0.0
        if hi - lo < 1: hi = lo + 1
    im = (img16.astype(np.float32) - lo) / (hi - lo)
    np.clip(im, 0, 1, out=im)
    if gamma:
        im = np.power(im, 0.45)
    im = (im * 255).astype(np.uint8)
    return im

def focus_metric(img16):
    """Simple focus aid: mean of |gradient| on downsampled frame."""
    s = img16[::4, ::4].astype(np.float32)
    gx = np.abs(np.diff(s, axis=1)).mean()
    gy = np.abs(np.diff(s, axis=0)).mean()
    return gx + gy

def fits_write(path, meta, img16):
    """Write 16-bit FITS with astro metadata (0..4095 raw)."""
    import datetime
    hdr = []
    def card(k, v):
        if isinstance(v, str):
            if len(v) > 68: v = v[:68]
            hdr.append(f"{k:<8}= '{v}'")
        else:
            hdr.append(f"{k:<8}= {v}")
    card("SIMPLE", True)
    card("BITPIX", 16)
    card("NAXIS", 2)
    card("NAXIS1", img16.shape[1])
    card("NAXIS2", img16.shape[0])
    card("BZERO", 0)
    card("BSCALE", 1)
    exp_s = meta["exp_us"] / 1e6
    card("EXPTIME", f"{exp_s:.6f}")
    gain_lin = meta["again"] * meta["dgain"] / 1024.0 / 1024.0
    card("GAIN", f"{gain_lin:.4f}")
    card("AGAIN", meta["again"])
    card("DGAIN", meta["dgain"])
    card("BAYERPAT", "RGGB")
    card("INSTRUME", "IMX662 Hi3516CV610")
    card("FRAME", state["ftype"])
    card("OBJECT", state["obj"])
    card("CCD-TEMP", f"{meta['temp']:.2f}")
    card("DATE-OBS", datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S"))
    card("END", "")
    header = np.zeros((2880,), dtype=np.uint8) + ord(' ')
    off = 0
    for c in hdr:
        b = c.encode()
        header[off:off+80].fill(ord(' '))
        header[off:off+len(b)] = np.frombuffer(b, dtype=np.uint8)
        off += 80
        if off >= 2880: break
    with open(path, "wb") as f:
        f.write(header.tobytes())
        # big-endian 16-bit
        f.write(img16.astype(">u2").tobytes())
        pad = (2880 - (img16.size * 2) % 2880) % 2880
        if pad: f.write(b"\x00" * pad)
    print(f"  saved {path} ({exp_s:.3f}s gain={gain_lin:.1f}x)")

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.16"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(None)
    print(f"Connected to {host}:{port}")

    global DARK, GAMMA, COLOR, BRIGHTNESS, OBJECT, BURST, SAVE_BURST
    last_t = time.time()
    frames = 0
    last_capture_name = None

    while True:
        got = read_frame(sock)
        if got is None:
            print("connection closed")
            break
        meta, raw = got
        img16 = raw12_to_uint16(raw)
        BUTTON_STATE["again"], BUTTON_STATE["dgain"] = meta["again"], meta["dgain"]
        BUTTON_STATE["exp_us"] = meta["exp_us"]

        now = time.time()
        frames += 1
        if now - last_t >= 2.0:
            state["fps"] = frames / (now - last_t)
            frames = 0
            last_t = now

        # focus aid
        fm = focus_metric(img16)

        if meta["cap"] and (BURST > 0 or SAVE_BURST):
            if BURST > 0:
                BURST -= 1
                name = f"{CAPTURE_DIR}/frame_{int(time.time())}_{state['ftype']}_{meta['idx']:06d}.fits"
                out = img16.astype(np.float32)
                if DARK is not None and state["ftype"] == "LIGHT":
                    out = np.clip(out - DARK, 0, None)
                fits_write(name, meta, out.astype(np.uint16))
                last_capture_name = name
            elif SAVE_BURST:
                SAVE_BURST = False
                BURST = 0

        if not HAVE_CV:
            print(f"E={meta['exp_us']}us VMAX={meta['vmax']} A={meta['again']} "
                  f"TEMP={meta['temp']:.1f}C fps={state['fps']:.1f} "
                  f"focus={fm:.0f} cap={meta['cap']}")
            continue

        disp = img16
        if DARK is not None:
            disp = np.clip(img16.astype(np.float32) - DARK, 0, None).astype(np.uint16)
        view = stretch(disp, GAMMA)
        if COLOR:
            bayer = cv2.cvtColor(view, cv2.COLOR_BAYER_RG2BGR)
        else:
            bayer = view
        bayer = cv2.resize(bayer, (960, 540), interpolation=cv2.INTER_NEAREST)
        cv2.putText(bayer, f"E={meta['exp_us']}us VMAX={meta['vmax']} A={meta['again']} "
                     f"TEMP={meta['temp']:.1f}C fps={state['fps']:.1f} focus={fm:.0f}",
                     (10, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)
        cv2.putText(bayer, f"type={state['ftype']} obj={state['obj']} burst={BURST} dark={'Y' if DARK is not None else 'N'}",
                     (10, 40), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)
        # clickable buttons (gain / exposure control)
        for name, (rect, label) in BUTTONS.items():
            rx, ry, rw, rh = rect
            cv2.rectangle(bayer, (rx, ry), (rx + rw, ry + rh), (60, 60, 60), -1)
            cv2.rectangle(bayer, (rx, ry), (rx + rw, ry + rh), (0, 200, 0), 1)
            cv2.putText(bayer, label, (rx + 4, ry + 16),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)
        cv2.imshow("astro preview", bayer)
        cv2.setMouseCallback("astro preview", mouse_cb, sock)

        k = cv2.waitKey(1) & 0xFF
        if k == ord('q') or k == 27:
            break
        elif k == ord('b'):
            COLOR = not COLOR
        elif k == ord('g'):
            GAMMA = not GAMMA
        elif k == ord('a'):
            set_frame_type(sock, "DARK")
            BURST = 10
            print("capturing 10 darks...")
        elif k == ord('l'):
            set_frame_type(sock, "LIGHT")
            BURST = 10
            print("capturing 10 lights...")
        elif k == ord('d'):
            if meta is not None and 'exp_us' in meta:
                DARK = img16.astype(np.float32) * 0.2 + (DARK if DARK is not None else 0) * 0.8
                print(f"dark updated (current exp={meta['exp_us']}us)")
        elif k == ord('x'):
            BURST = 0
            print("burst cancelled")
        elif k == ord(' '):
            if BURST == 0:
                BURST = 1
                print("single-frame capture")
            else:
                BURST = 0
        elif k == ord('o'):
            print("enter object name:")
            # non-blocking note; set via next key? use simple path
            pass
        elif k == ord('+') or k == ord('='):
            BRIGHTNESS *= 1.2
        elif k == ord('-'):
            BRIGHTNESS /= 1.2
        elif k == ord('p'):
            preview_mode(sock)
            print("preview mode (30fps)")
        elif k in PRESETS:
            set_exposure_us(sock, PRESETS[k])
            print(f"exposure -> {PRESETS[k]}us")
        elif k == ord('i'):
            print("enter ISO then press key (use: ISO <value> via cmd)")
        elif k == ord('h'):
            print("keys: q=quit b=color g=gamma a=darkburst l=lightburst d=setdark "
                  "x=cancel space=single p=preview 1-9=exp presets")

    sock.close()
    if HAVE_CV:
        cv2.destroyAllWindows()
    print("done")

if __name__ == "__main__":
    main()
