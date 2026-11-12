# GUI 冒烟测试 v3：SendInput 真实鼠标双击
import ctypes, ctypes.wintypes as wt, time, sys

u32 = ctypes.windll.user32
SM = u32.SendMessageW
SM.restype = ctypes.c_ssize_t
SM.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, ctypes.c_void_p]

def enum_windows(title_prefix):
    out = []
    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    @EnumWindowsProc
    def cb(hwnd, lparam):
        sb = ctypes.create_unicode_buffer(256)
        u32.GetWindowTextW(hwnd, sb, 256)
        if sb.value.startswith(title_prefix):
            out.append((hwnd, sb.value))
        return True
    u32.EnumWindows(cb, 0)
    return out

def find_controls(parent):
    tree = listv = 0
    EnumChildProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    @EnumChildProc
    def cb(c, l):
        nonlocal tree, listv
        buf = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(c, buf, 64)
        if buf.value == "SysTreeView32" and not tree:
            tree = c
        if buf.value == "SysListView32" and not listv:
            listv = c
        return True
    u32.EnumChildWindows(parent, cb, 0)
    return tree, listv

class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.DWORD), ("dy", wt.DWORD), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]
class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("mi", MOUSEINPUT)]

def send_click(sx, sy, double=False):
    """真实鼠标左键点击（屏幕绝对坐标）"""
    MOUSEEVENTF_MOVE = 0x0001
    MOUSEEVENTF_LEFTDOWN = 0x0002
    MOUSEEVENTF_LEFTUP = 0x0004
    MOUSEEVENTF_ABSOLUTE = 0x8000
    def mk(flags):
        inp = INPUT()
        inp.type = 0  # INPUT_MOUSE
        inp.mi.dx = int(sx * 65535 / 1920)
        inp.mi.dy = int(sy * 65535 / 1080)
        inp.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE
        return inp
    clicks = [mk(MOUSEEVENTF_MOVE), mk(MOUSEEVENTF_LEFTDOWN), mk(MOUSEEVENTF_LEFTUP)]
    if double:
        clicks += [mk(MOUSEEVENTF_LEFTDOWN), mk(MOUSEEVENTF_LEFTUP)]
    arr = (INPUT * len(clicks))(*clicks)
    u32.SendInput(len(clicks), arr, ctypes.sizeof(INPUT))

def client_to_screen(hwnd, x, y):
    pt = POINT(x, y)
    u32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y

# ---- 定位 ----
win = None
for _ in range(20):
    ws = enum_windows("fakedisk-windows")
    if ws:
        win = ws[0][0]; break
    time.sleep(0.5)
if not win:
    print("ERR no window"); sys.exit(1)
tv, lv = find_controls(win)
print("win", win, "tv", tv, "lv", lv)

LVM_GETITEMCOUNT = 0x1004
def rows():
    return SM(lv, LVM_GETITEMCOUNT, 0, 0)

print("root rows:", rows())

# 屏幕尺寸
sw = u32.GetSystemMetrics(0); sh = u32.GetSystemMetrics(1)
print("screen:", sw, "x", sh)

def dbl_listview(cx, cy, tag):
    sx, sy = client_to_screen(lv, cx, cy)
    print(tag, "listview client", (cx, cy), "-> screen", (sx, sy))
    send_click(sx, sy, double=True)
    time.sleep(1.0)
    print(tag, "rows ->", rows())

# 第 0 行（docs）：x=60 第一列文本区，y=33 列头下第一行
dbl_listview(60, 33, "dbl row0 (docs)")
r = rows()
print("in docs rows:", r)

# 若进入 docs（1 项），双击打开 pic.png 验证图片查看器
if r == 1:
    dbl_listview(60, 33, "dbl pic.png")
    imgs = enum_windows("pic.png")
    print("image viewer:", imgs)
    if imgs:
        SM(imgs[0][0], 0x0010, 0, 0)  # WM_CLOSE
        time.sleep(0.4)
        print("viewer closed, alive:", bool(enum_windows("fakedisk-windows")))
else:
    # 尝试第 1 行 tools
    dbl_listview(60, 55, "dbl row1 (tools)")
    print("in tools rows:", rows())

time.sleep(0.5)
print("alive:", bool(enum_windows("fakedisk-windows")))
