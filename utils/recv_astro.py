#!/usr/bin/env python3
"""Receiver OpenCV para astro_streamer (raw12 + H.265).

Conecta al socket de datos (puerto 5000) y al canal de control (5998).
El formato de datos depende del modo actual del streamer:

  - RAW : cabecera "AS" de 48 bytes + raw12 packed (3 B / 2 px, LSB-first)
  - H265: cabecera "WB" de 12 bytes + AnnexB H.265

Botón CONFIG (o tecla 'm') conmuta raw <-> h265 en runtime via el canal de
control (MODE raw|h265).  El decode H.265 es EN PROCESO (cv2.VideoCapture sobre
FIFO, libav interno) — sin subproceso ffmpeg.

Controles:
  q/ESC salir | m conmutar raw<->h265 | b color | g gamma | a burst DARK
  l burst LIGHT | d set dark | x cancelar | space captura simple
  +/− brillo | 1-9 presets de exposición | s guardar stream h265
"""
import socket, struct, sys, time, os, threading, tempfile, queue
import numpy as np

HDR = struct.Struct("<2sHHHIIQIIIIiB3x")  # 48 bytes matching C header "AS"
WB_HDR = struct.Struct("<2sBBIIIIII")      # 28 bytes "WB" + idr/0 + len + pts + exp_us + again + dgain + vmax

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
MODE = "raw"         # raw | h265  (modo deseado; refleja el del streamer)

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
                        ("GAIN-AUTO", "AUTO"),
                        ("CONFIG", "CONFIG")):
        wpx = 74 if name not in ("GAIN-AUTO", "CONFIG") else 56
        b[name] = ((x, y, wpx, 22), label)
        x += wpx + 4
    return b

BUTTONS = make_buttons()
BUTTON_STATE = {"again": 1024, "dgain": 1024, "exp_us": 0}

# Control socket (canal separado, no corrompe el stream binario)
CTRL = None
HOST = "192.168.1.16"
PORT = 5000
CTRL_PORT = 5998

def ctrl_send(cmd):
    """Enviar comando por el canal de control (MODE + AE)."""
    global CTRL
    if CTRL is None:
        try:
            CTRL = socket.create_connection((HOST, CTRL_PORT), timeout=5)
            CTRL.settimeout(None)
            print(f"  (ctrl reconectado {HOST}:{CTRL_PORT})")
        except OSError:
            return ""
    try:
        CTRL.sendall((cmd + "\n").encode())
    except OSError:
        try:
            CTRL.close()
        except OSError:
            pass
        CTRL = None
        return ""
    return ""

def button_action(name):
    st = BUTTON_STATE
    if name == "A-":
        st["again"] = max(1, int(round(st["again"] / STEP)))
        ctrl_send(f"A {st['again']}")
    elif name == "A+":
        st["again"] = min(32768, int(round(st["again"] * STEP)))
        ctrl_send(f"A {st['again']}")
    elif name == "D-":
        st["dgain"] = max(1, int(round(st["dgain"] / STEP)))
        ctrl_send(f"D {st['dgain']}")
    elif name == "D+":
        st["dgain"] = min(16384, int(round(st["dgain"] * STEP)))
        ctrl_send(f"D {st['dgain']}")
    elif name == "T-":
        st["exp_us"] = max(100, int(round(st["exp_us"] / STEP)))
        ctrl_send(f"T {st['exp_us']}")
    elif name == "T+":
        st["exp_us"] = min(30000000, int(round(st["exp_us"] * STEP)))
        ctrl_send(f"T {st['exp_us']}")
    elif name == "GAIN-AUTO":
        if MODE == "h265":
            ctrl_send("T auto")
            print("auto exposure (H265)")
        else:
            set_iso(100)
            set_exposure_us(1000)
            print("ISO 100 + exp 1000us (RAW)")
    elif name == "CONFIG":
        toggle_mode()
    print(f"  btn {name}: A={st['again']} D={st['dgain']} T={st['exp_us']}us")

def mouse_cb(event, x, y, flags, param):
    if event != cv2.EVENT_LBUTTONDOWN:
        return
    for name, (rect, label) in BUTTONS.items():
        rx, ry, rw, rh = rect
        if rx <= x < rx + rw and ry <= y < ry + rh:
            button_action(name)
            return

# ------------------------------------------------------------- mode switch --
def toggle_mode():
    cur = READER.get("kind", "raw")
    new = "h265" if cur == "raw" else "raw"
    ctrl_send(f"MODE {new}")
    print(f"  -> MODE {new} (desde {cur})")
    if new == "raw":
        ctrl_send("A 1024"); ctrl_send("D 1024"); ctrl_send("T 1000")

# ------------------------------------------------------------ command fns --
def set_exposure_us(us):
    ctrl_send(f"T {int(us)}")

def set_exposure_ms(ms):
    ctrl_send(f"M {int(ms)}")

def set_exposure_lines(lines):
    ctrl_send(f"E {int(lines)}")

def set_iso(iso):
    ctrl_send(f"I {int(iso)}")

def set_gain(again=None, dgain=None):
    if again is not None: ctrl_send(f"A {int(again)}")
    if dgain is not None: ctrl_send(f"D {int(dgain)}")

def set_vmax(vmax):
    ctrl_send(f"V {int(vmax)}")

def preview_mode():
    ctrl_send("P")

def set_frame_type(t):
    ctrl_send(f"F {t}")

def set_object(name):
    ctrl_send(f"C {name}")

# ------------------------------------------------------------------- I/O --
def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data

def read_frame(sock):
    """Lee un frame del socket de datos.  Detecta formato por magic:
    'AS' -> raw12, 'WB' -> H.265.  Devuelve (kind, meta|None, payload) o None."""
    head = recv_exact(sock, 2)
    if head is None:
        return None
    if head == b"AS":
        rest = recv_exact(sock, HDR.size - 2)
        if rest is None:
            return None
        hdr_data = head + rest
        magic, w, h, stride, size, idx, ts, exp_us, again, dgain, vmax, temp, cap = HDR.unpack(hdr_data)
        payload = recv_exact(sock, size)
        if payload is None:
            return None
        meta = dict(w=w, h=h, stride=stride, size=size, idx=idx, ts_us=ts,
                    exp_us=exp_us, again=again, dgain=dgain, vmax=vmax,
                    temp=temp, cap=cap)
        state["exp"], state["vmax"] = exp_us, vmax
        state["again"], state["dgain"] = again, dgain
        state["temp"] = temp
        return "raw", meta, payload
    elif head == b"WB":
        rest = recv_exact(sock, WB_HDR.size - 2)
        if rest is None:
            return None
        hdr_data = head + rest
        magic, is_idr, zero, length, pts, exp_us, again, dgain, vmax = WB_HDR.unpack(hdr_data)
        payload = recv_exact(sock, length)
        if payload is None:
            return None
        meta = dict(idx=0, is_idr=is_idr, pts=pts, length=length, exp_us=exp_us,
                    again=again, dgain=dgain, vmax=vmax, temp=0.0, cap=0)
        return "h265", meta, payload
    return None

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

# ----------------------------------------------- H.265 decode EN PROCESO ----
# Un único thread lee el socket de datos: los frames raw van a una cola, los
# H.265 se escriben a un FIFO que cv2.VideoCapture decodifica con libav dentro
# del mismo proceso (sin subproceso ffmpeg externo).
FIFO = "/tmp/astro_h265.pipe"
DEC = {"frame": None, "ok": False, "t": 0.0}
RAW_Q = queue.Queue(maxsize=8)
READER = {"kind": "raw", "running": True, "rec_file": None}
H265_META = {"exp_us": 0, "again": 1024, "dgain": 1024, "vmax": 1250}
LIVE = {"exp_us": 0, "again": 1024, "dgain": 1024}   # valor REAL actual del sensor
SEEDED = False   # semilla de BUTTON_STATE con el valor real la 1ra vez

def _reader_thread():
    """Dueño exclusivo del socket: despacha raw->cola, h265->FIFO.
    Reconecta automaticamente si el streamer cierra la conexion (p.ej. por un
    switch de modo H265<->RAW que desincronizaria el parseo de frames)."""
    while READER["running"]:
        try:
            s = socket.create_connection((HOST, PORT), timeout=10)
            s.settimeout(None)
        except OSError:
            time.sleep(0.5)
            continue
        try:
            while READER["running"]:
                got = read_frame(s)
                if got is None:
                    break
                kind, meta, payload = got
                READER["kind"] = kind
                if kind == "raw":
                    try:
                        RAW_Q.put((meta, payload), timeout=0.1)
                    except queue.Full:
                        pass
                else:
                    try:
                        os.write(DEC["wfd"], payload)
                    except OSError:
                        break
                    H265_META.clear(); H265_META.update(meta)
                    rf = READER["rec_file"]
                    if rf is not None:
                        try:
                            rf.write(payload)
                        except OSError:
                            pass
        except OSError:
            pass
        finally:
            try:
                s.close()
            except OSError:
                pass
            time.sleep(0.2)

def _decoder_thread():
    """cv2.VideoCapture sobre el FIFO: guarda el ultimo frame decodificado.
    Solo decodifica en modo H265; en RAW libera el cap para no quedar bloqueado
    en el FIFO vacio y reabre limpio al volver a H265 (evita que DEC['frame']
    se quede en None y la pantalla no conmute)."""
    cap = None
    while True:
        if READER["kind"] != "h265":
            if cap is not None:
                try:
                    cap.release()
                except OSError:
                    pass
                cap = None
            time.sleep(0.2)
            continue
        if cap is None or not cap.isOpened():
            try:
                cap = cv2.VideoCapture(FIFO, cv2.CAP_FFMPEG)
            except Exception:
                time.sleep(0.3)
                continue
            if not cap.isOpened():
                time.sleep(0.3)
                continue
        ok, frame = cap.read()
        if not ok:
            try:
                cap.release()
            except OSError:
                pass
            cap = None
            time.sleep(0.2)
            continue
        DEC["frame"] = frame
        DEC["ok"] = True
        DEC["t"] = time.time()

def start_io_threads():
    """Abre el FIFO (O_RDWR para no bloquear) y lanza reader+decoder."""
    if os.path.exists(FIFO):
        os.unlink(FIFO)
    try:
        os.mkfifo(FIFO)
    except OSError:
        pass
    # fd O_RDWR de keep-alive: evita deadlock en los opens del FIFO.
    DEC["wfd"] = os.open(FIFO, os.O_RDWR)
    t1 = threading.Thread(target=_reader_thread, daemon=True)
    t2 = threading.Thread(target=_decoder_thread, daemon=True)
    t1.start()
    t2.start()
    return t1, t2

# ------------------------------------------------------------------- main --
def main():
    global SEEDED
    global HOST, PORT, CTRL_PORT
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.16"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    ctrl_port = int(sys.argv[3]) if len(sys.argv) > 3 else 5998
    HOST, PORT, CTRL_PORT = host, port, ctrl_port
    print(f"Data host {host}:{port} (reader reconecta solo, ctrl {ctrl_port})")

    global CTRL
    try:
        CTRL = socket.create_connection((host, ctrl_port), timeout=10)
        CTRL.settimeout(None)
        print(f"Connected ctrl  {host}:{ctrl_port}")
    except OSError as e:
        print(f"  (sin canal de control: {e})")

    global DARK, GAMMA, COLOR, BRIGHTNESS, OBJECT, BURST, SAVE_BURST, MODE
    last_t = time.time()
    frames = 0

    # I/O: un único thread lee el socket (raw->cola, h265->FIFO->decoder).
    start_io_threads()
    time.sleep(0.5)

    last_img16 = None
    last_meta = None
    while READER["running"]:
        # --- mode label sigue el modo REAL del stream (READER["kind"]) ---
        kind = READER["kind"]
        MODE = kind

        got = None
        if kind == "raw":
            try:
                got = RAW_Q.get(timeout=0.05)
            except queue.Empty:
                got = None

        meta = None
        img16 = None

        if kind == "raw" and got is not None:
            meta, payload = got
            raw = np.frombuffer(payload, dtype=np.uint8).reshape(meta["h"], meta["stride"])
            img16 = raw12_to_uint16(raw)
            last_img16 = img16
            last_meta = meta
            LIVE.update(exp_us=meta["exp_us"], again=meta["again"], dgain=meta["dgain"])
            state["exp"], state["vmax"] = meta["exp_us"], meta["vmax"]
            state["again"], state["dgain"] = meta["again"], meta["dgain"]
            if not SEEDED:
                BUTTON_STATE.update(exp_us=meta["exp_us"], again=meta["again"],
                                    dgain=meta["dgain"])
                SEEDED = True
            if meta["cap"] and (BURST > 0 or SAVE_BURST):
                if BURST > 0:
                    BURST -= 1
                    name = f"{CAPTURE_DIR}/frame_{int(time.time())}_{state['ftype']}_{meta['idx']:06d}.fits"
                    out = img16.astype(np.float32)
                    if DARK is not None and state["ftype"] == "LIGHT":
                        out = np.clip(out - DARK, 0, None)
                    fits_write(name, meta, out.astype(np.uint16))
                elif SAVE_BURST:
                    SAVE_BURST = False
                    BURST = 0
        elif kind == "h265":
            meta = H265_META
            LIVE.update(exp_us=H265_META["exp_us"], again=H265_META["again"],
                        dgain=H265_META["dgain"])
            if not SEEDED:
                BUTTON_STATE.update(exp_us=H265_META["exp_us"], again=H265_META["again"],
                                    dgain=H265_META["dgain"])
                SEEDED = True
            state["exp"], state["vmax"] = H265_META["exp_us"], H265_META["vmax"]
            state["again"], state["dgain"] = H265_META["again"], H265_META["dgain"]

        now = time.time()
        frames += 1
        if now - last_t >= 2.0:
            state["fps"] = frames / (now - last_t)
            frames = 0
            last_t = now

        fm = 0.0
        if img16 is not None:
            fm = focus_metric(img16)

        if not HAVE_CV:
            if kind == "raw" and img16 is not None:
                print(f"E={meta['exp_us']}us VMAX={meta['vmax']} A={meta['again']} "
                      f"TEMP={meta['temp']:.1f}C fps={state['fps']:.1f} "
                      f"focus={fm:.0f} cap={meta['cap']}")
            elif kind == "h265":
                print(f"h265 ... fps={state['fps']:.1f}")
            continue

        if kind == "raw" and last_img16 is not None:
            disp = last_img16
            if DARK is not None:
                disp = np.clip(last_img16.astype(np.float32) - DARK, 0, None).astype(np.uint16)
            view = stretch(disp, GAMMA)
            if COLOR:
                view = cv2.cvtColor(view, cv2.COLOR_BAYER_RG2BGR)
            else:
                view = np.stack([view] * 3, axis=-1)
            view = cv2.resize(view, (960, 540), interpolation=cv2.INTER_NEAREST)
            m = last_meta
            line1 = f"RAW E={m['exp_us']}us VMAX={m['vmax']} A={m['again']} "
            line1 += f"D={m['dgain']} TEMP={m['temp']:.1f}C fps={state['fps']:.1f} focus={fm:.0f}"
        elif kind == "h265":
            d = DEC.get("frame")
            if d is not None:
                view = cv2.resize(d, (960, 540))
                line1 = (f"H265 E={H265_META['exp_us']}us A={H265_META['again']} "
                     f"D={H265_META['dgain']} fps={state['fps']:.1f}")
            else:
                view = np.zeros((540, 960, 3), dtype=np.uint8)
                line1 = "H265 (decodificando...)"
        else:
            view = np.zeros((540, 960, 3), dtype=np.uint8)
            line1 = "RAW (esperando frames...)"

        cv2.putText(view, line1, (10, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)
        cv2.putText(view, f"type={state['ftype']} obj={state['obj']} burst={BURST} "
                     f"dark={'Y' if DARK is not None else 'N'} mode={MODE}",
                     (10, 40), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)
        # clickable buttons (gain / exposure / config)
        for name, (rect, label) in BUTTONS.items():
            rx, ry, rw, rh = rect
            cv2.rectangle(view, (rx, ry), (rx + rw, ry + rh), (60, 60, 60), -1)
            cv2.rectangle(view, (rx, ry), (rx + rw, ry + rh), (0, 200, 0), 1)
            cv2.putText(view, label, (rx + 4, ry + 16),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.4, (255, 255, 255), 1)
        cv2.imshow("astro preview", view)
        cv2.setMouseCallback("astro preview", mouse_cb)

        k = cv2.waitKey(1) & 0xFF
        if k == ord('q') or k == 27:
            break
        elif k == ord('m'):
            toggle_mode()
        elif k == ord('b'):
            COLOR = not COLOR
        elif k == ord('g'):
            GAMMA = not GAMMA
        elif k == ord('a'):
            set_frame_type("DARK")
            BURST = 10
            print("capturing 10 darks...")
        elif k == ord('l'):
            set_frame_type("LIGHT")
            BURST = 10
            print("capturing 10 lights...")
        elif k == ord('d'):
            if img16 is not None:
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
        elif k == ord('+') or k == ord('='):
            BRIGHTNESS *= 1.2
        elif k == ord('-'):
            BRIGHTNESS /= 1.2
        elif k == ord('p'):
            preview_mode()
            print("preview mode (30fps)")
        elif k in PRESETS:
            set_exposure_us(PRESETS[k])
            print(f"exposure -> {PRESETS[k]}us")
        elif k == ord('s'):
            if READER["rec_file"] is None:
                h265_file_name = f"{CAPTURE_DIR}/astro_{int(time.time())}.h265"
                READER["rec_file"] = open(h265_file_name, "wb")
                print(f"recording h265 -> {h265_file_name}")
            else:
                READER["rec_file"].close()
                READER["rec_file"] = None
                print(f"saved {h265_file_name}")
        elif k == ord('h'):
            print("keys: q=quit m=raw<->h265 b=color g=gamma a=darkburst l=lightburst "
                  "d=setdark x=cancel space=single s=record_h265 +/−=brightness "
                  "1-9=exp presets")

    READER["running"] = False
    if READER["rec_file"] is not None:
        READER["rec_file"].close()
        READER["rec_file"] = None
    if CTRL is not None:
        CTRL.close()
    if HAVE_CV:
        cv2.destroyAllWindows()
    print("done")

if __name__ == "__main__":
    main()