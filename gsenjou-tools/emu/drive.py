#!/usr/bin/env python3
"""Headless libretro frontend for the Azahar 3DS core: runs a .3dsx, presses
scripted buttons and saves screenshots. OpenGL comes from a surfaceless EGL
context rendering into an off-screen framebuffer.

usage: drive.py CORE.so GAME.3dsx WORKDIR SCRIPT
SCRIPT is a text file, one command per line:
    wait N               run N frames with no buttons
    press A [N]          hold button(s) (e.g. A or A+R) for N frames (default 6), then release 6
    hold R N             hold for N frames, no release frames
    shot NAME            save WORKDIR/shots/NAME.png
    every N K NAME       K screenshots NAME_00.. one every N frames while holding nothing
    holdshots R N K NAME hold R, screenshot every N frames, K times
Buttons: A B X Y L R START SELECT UP DOWN LEFT RIGHT
"""
import ctypes as C, os, sys, struct, zlib

core_path, game_path, work, script_path = sys.argv[1:5]
os.makedirs(os.path.join(work, "shots"), exist_ok=True)

# ---------------- EGL / GL ----------------
egl = C.CDLL("libEGL.so.1")
gl = C.CDLL("libOpenGL.so.0")
egl.eglGetProcAddress.restype = C.c_void_p
egl.eglGetProcAddress.argtypes = [C.c_char_p]
egl.eglGetDisplay.restype = C.c_void_p
egl.eglGetDisplay.argtypes = [C.c_void_p]
egl.eglInitialize.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
egl.eglBindAPI.argtypes = [C.c_uint]
egl.eglChooseConfig.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_int, C.c_void_p]
egl.eglCreateContext.restype = C.c_void_p
egl.eglCreateContext.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
egl.eglCreatePbufferSurface.restype = C.c_void_p
egl.eglCreatePbufferSurface.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
egl.eglMakeCurrent.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
egl.eglGetError.restype = C.c_int

EGL_NONE = 0x3038
dpy = egl.eglGetDisplay(None)
assert egl.eglInitialize(dpy, None, None), hex(egl.eglGetError())
assert egl.eglBindAPI(0x30A2)  # EGL_OPENGL_API
attrs = (C.c_int * 13)(0x3033, 0x1, 0x3040, 0x8, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3025, 24, EGL_NONE)
config = C.c_void_p()
num = C.c_int()
assert egl.eglChooseConfig(dpy, attrs, C.byref(config), 1, C.byref(num)) and num.value, "no EGL config"
surface = egl.eglCreatePbufferSurface(dpy, config, (C.c_int * 5)(0x3057, 16, 0x3056, 16, EGL_NONE))
ctx = egl.eglCreateContext(dpy, config, None, (C.c_int * 7)(0x3098, 4, 0x30FB, 3, 0x30FD, 1, EGL_NONE))
assert ctx, "no GL 4.3 core context: " + hex(egl.eglGetError())
assert egl.eglMakeCurrent(dpy, surface, surface, ctx)


def glfn(name, restype, *argtypes):
    f = getattr(gl, name)
    f.restype = restype
    f.argtypes = list(argtypes)
    return f


glGenFramebuffers = glfn("glGenFramebuffers", None, C.c_int, C.c_void_p)
glBindFramebuffer = glfn("glBindFramebuffer", None, C.c_uint, C.c_uint)
glGenTextures = glfn("glGenTextures", None, C.c_int, C.c_void_p)
glBindTexture = glfn("glBindTexture", None, C.c_uint, C.c_uint)
glTexImage2D = glfn("glTexImage2D", None, C.c_uint, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_void_p)
glFramebufferTexture2D = glfn("glFramebufferTexture2D", None, C.c_uint, C.c_uint, C.c_uint, C.c_uint, C.c_int)
glGenRenderbuffers = glfn("glGenRenderbuffers", None, C.c_int, C.c_void_p)
glBindRenderbuffer = glfn("glBindRenderbuffer", None, C.c_uint, C.c_uint)
glRenderbufferStorage = glfn("glRenderbufferStorage", None, C.c_uint, C.c_uint, C.c_int, C.c_int)
glFramebufferRenderbuffer = glfn("glFramebufferRenderbuffer", None, C.c_uint, C.c_uint, C.c_uint, C.c_uint)
glReadPixels = glfn("glReadPixels", None, C.c_int, C.c_int, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_void_p)
glPixelStorei = glfn("glPixelStorei", None, C.c_uint, C.c_int)

FB_W, FB_H = 2048, 2048
fbo, tex, rbo = C.c_uint(), C.c_uint(), C.c_uint()
glGenFramebuffers(1, C.byref(fbo))
glBindFramebuffer(0x8D40, fbo.value)
glGenTextures(1, C.byref(tex))
glBindTexture(0x0DE1, tex.value)
glTexImage2D(0x0DE1, 0, 0x8058, FB_W, FB_H, 0, 0x1908, 0x1401, None)  # RGBA8
glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex.value, 0)
glGenRenderbuffers(1, C.byref(rbo))
glBindRenderbuffer(0x8D41, rbo.value)
glRenderbufferStorage(0x8D41, 0x88F0, FB_W, FB_H)  # DEPTH24_STENCIL8
glFramebufferRenderbuffer(0x8D40, 0x821A, 0x8D41, rbo.value)

# ---------------- libretro ----------------
core = C.CDLL(os.path.abspath(core_path))
ENV = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIO_BATCH = C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t)
POLL = C.CFUNCTYPE(None)
STATE = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)
RESET = C.CFUNCTYPE(None)
GETFB = C.CFUNCTYPE(C.c_size_t)
GETPROC = C.CFUNCTYPE(C.c_void_p, C.c_char_p)


class HwRender(C.Structure):
    _fields_ = [("context_type", C.c_uint), ("context_reset", RESET), ("get_current_framebuffer", GETFB),
                ("get_proc_address", GETPROC), ("depth", C.c_bool), ("stencil", C.c_bool),
                ("bottom_left_origin", C.c_bool), ("version_major", C.c_uint), ("version_minor", C.c_uint),
                ("cache_context", C.c_bool), ("context_destroy", RESET), ("debug_context", C.c_bool)]


class Variable(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]


class GameInfo(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p), ("size", C.c_size_t), ("meta", C.c_char_p)]


LOGFN = C.CFUNCTYPE(None, C.c_int, C.c_char_p)  # variadic in C; we only print the format string


class LogCb(C.Structure):
    _fields_ = [("log", LOGFN)]


OPTIONS = {b"citra_use_virtual_sd": b"enabled", b"citra_use_libretro_save_path": b"LibRetro Default",
           b"citra_use_cpu_jit": b"enabled", b"citra_resolution_factor": b"1x (Native 400x240)",
           b"citra_audio_emulation": b"HLE (Fast)"}
keep = []
hw = {"cb": None}
workdir_b = os.path.abspath(work).encode()
log_lines = open(os.path.join(work, "core.log"), "w")


@LOGFN
def core_log(level, fmt):
    log_lines.write(f"[{level}] {fmt.decode(errors='replace').rstrip()}\n")


log_cb = LogCb(core_log)


@GETFB
def get_fb():
    return fbo.value


@GETPROC
def get_proc(name):
    return egl.eglGetProcAddress(name)


@ENV
def environment(cmd, data):
    cmd &= 0xFFFF  # strip EXPERIMENTAL flag
    if cmd in (9, 31, 30):  # system / save / core assets dir
        C.cast(data, C.POINTER(C.c_char_p))[0] = workdir_b
        return True
    if cmd == 10:  # SET_PIXEL_FORMAT
        return True
    if cmd == 14:  # SET_HW_RENDER
        cb = C.cast(data, C.POINTER(HwRender)).contents
        if cb.context_type not in (1, 3):
            return False
        cb.get_current_framebuffer = get_fb
        cb.get_proc_address = get_proc
        hw["cb"] = cb
        hw["bottom_left"] = cb.bottom_left_origin
        return True
    if cmd == 56:  # GET_PREFERRED_HW_RENDER
        C.cast(data, C.POINTER(C.c_uint))[0] = 3
        return True
    if cmd == 15:  # GET_VARIABLE
        var = C.cast(data, C.POINTER(Variable)).contents
        if var.key in OPTIONS:
            var.value = OPTIONS[var.key]
            return True
        return False
    if cmd == 52:  # GET_CORE_OPTIONS_VERSION -> legacy variables
        C.cast(data, C.POINTER(C.c_uint))[0] = 0
        return True
    if cmd == 27:  # GET_LOG_INTERFACE
        C.cast(data, C.POINTER(LogCb))[0] = log_cb
        return True
    if cmd == 3:  # GET_CAN_DUPE
        C.cast(data, C.POINTER(C.c_bool))[0] = True
        return True
    if cmd in (16, 11, 35, 37, 32, 8, 18, 21, 36, 44, 53, 54, 55, 67, 68, 69, 6, 60, 64, 65):
        return True
    if cmd == 17:  # GET_VARIABLE_UPDATE
        C.cast(data, C.POINTER(C.c_bool))[0] = False
        return True
    return False


frame = {"w": 0, "h": 0, "want": None}


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


@VIDEO
def video(data, w, h, pitch):
    frame["w"], frame["h"] = w, h
    if frame["want"] and data == 2**64 - 1:  # RETRO_HW_FRAME_BUFFER_VALID
        buf = (C.c_ubyte * (w * h * 4))()
        glBindFramebuffer(0x8D40, fbo.value)
        glPixelStorei(0x0D05, 1)
        glReadPixels(0, 0, w, h, 0x1908, 0x1401, buf)
        raw = bytes(buf)
        rows = [raw[y * w * 4:(y + 1) * w * 4] for y in range(h)]
        if hw.get("bottom_left", True):
            rows.reverse()
        write_png(os.path.join(work, "shots", frame["want"] + ".png"), w, h, rows)
        print("shot", frame["want"], w, h, flush=True)
        frame["want"] = None


@AUDIO
def audio(l, r):
    pass


@AUDIO_BATCH
def audio_batch(data, frames):
    return frames


@POLL
def poll():
    pass


BUTTONS = {"B": 0, "Y": 1, "SELECT": 2, "START": 3, "UP": 4, "DOWN": 5, "LEFT": 6, "RIGHT": 7,
           "A": 8, "X": 9, "L": 10, "R": 11}
held = set()


@STATE
def state(port, device, index, ident):
    if port == 0 and device == 1:
        return 1 if ident in held else 0
    return 0


for name, cb in (("retro_set_environment", environment), ("retro_set_video_refresh", video),
                 ("retro_set_audio_sample", audio), ("retro_set_audio_sample_batch", audio_batch),
                 ("retro_set_input_poll", poll), ("retro_set_input_state", state)):
    getattr(core, name)(cb)
core.retro_init()
info = GameInfo(os.path.abspath(game_path).encode(), None, 0, None)
assert core.retro_load_game(C.byref(info)), "retro_load_game failed (see core.log)"
assert hw["cb"] is not None, "core did not ask for OpenGL"
hw["cb"].context_reset()
print("game loaded", flush=True)


def run(n, buttons=()):
    held.clear()
    held.update(BUTTONS[b] for b in buttons)
    for _ in range(n):
        core.retro_run()


def snap(name):
    frame["want"] = name
    for _ in range(3):
        core.retro_run()
        if frame["want"] is None:
            return
    print("no frame for", name, flush=True)
    frame["want"] = None


for raw in open(script_path):
    parts = raw.split()
    if not parts or parts[0].startswith("#"):
        continue
    cmd, args = parts[0], parts[1:]
    if cmd == "wait":
        run(int(args[0]))
    elif cmd == "press":
        run(int(args[1]) if len(args) > 1 else 6, args[0].split("+"))
        run(6)
    elif cmd == "hold":
        run(int(args[1]), args[0].split("+"))
    elif cmd == "shot":
        held.clear()
        snap(args[0])
    elif cmd == "every":
        for k in range(int(args[1])):
            run(int(args[0]))
            snap(f"{args[2]}_{k:02d}")
    elif cmd == "ffchapter":
        # ffchapter MAXFRAMES SHOTEVERY: fast-forward (tapping A for choices) until the
        # test log shows a second SCRIPT line (next chapter), an ERROR, or MAXFRAMES.
        limit, every = int(args[0]), int(args[1])
        log_path = os.path.join(work, "Azahar", "sdmc", "3dvn-test.log")
        frames, k, status = 0, 0, "timeout"
        while frames < limit:
            run(110, ["R"])
            run(6, ["R", "A"])
            frames += 116
            if frames // every > k:
                k = frames // every
                snap(f"ch_{k:03d}")
            text = open(log_path).read() if os.path.exists(log_path) else ""
            if "\nERROR" in "\n" + text:
                status = "error"
                break
            if sum(1 for line in text.splitlines() if line.startswith("SCRIPT ")) >= 2:
                status = "next-chapter"
                break
        print(f"ffchapter: {status} after {frames} frames", flush=True)
        with open(os.path.join(work, "result.txt"), "w") as h:
            h.write(f"{status} {frames}\n")
    elif cmd == "holdshots":
        for k in range(int(args[2])):
            run(int(args[1]), args[0].split("+"))
            snap(f"{args[3]}_{k:02d}")
    print("done:", raw.strip(), flush=True)


log_lines.flush()
os._exit(0)
