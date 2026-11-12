# 验证只读版菜单点击提示“你无权进行此操作”，manager 版正常
import ctypes, ctypes.wintypes as wt, time, sys, subprocess

u32 = ctypes.windll.user32

def enum_windows(prefix, exact=False):
    out = []
    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    @EnumWindowsProc
    def cb(hwnd, lparam):
        sb = ctypes.create_unicode_buffer(256)
        u32.GetWindowTextW(hwnd, sb, 256)
        if (sb.value == prefix) if exact else sb.value.startswith(prefix):
            out.append((hwnd, sb.value))
        return True
    u32.EnumWindows(cb, 0)
    return out

class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]
class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.DWORD), ("dy", wt.DWORD), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]
class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]
class INPUT(ctypes.Structure):
    class U(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]
    _fields_ = [("type", wt.DWORD), ("u", U)]
sw = u32.GetSystemMetrics(0); sh = u32.GetSystemMetrics(1)

def click(sx, sy):
    MOVE, LD, LU, ABS = 0x0001, 0x0002, 0x0004, 0x8000
    def mk(f):
        i = INPUT(); i.type = 0
        i.u.mi.dx = int(sx * 65535 / sw); i.u.mi.dy = int(sy * 65535 / sh)
        i.u.mi.dwFlags = f | ABS
        return i
    arr = (INPUT * 3)(mk(MOVE), mk(LD), mk(LU))
    u32.SendInput(3, arr, ctypes.sizeof(INPUT))

def key(vk, down):
    i = INPUT(); i.type = 1
    i.u.ki.wVk = vk
    i.u.ki.dwFlags = 0 if down else 2
    u32.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))

def wait_win(prefix, timeout=15):
    for _ in range(timeout * 2):
        ws = enum_windows(prefix)
        if ws: return ws[0]
        time.sleep(0.5)
    return None

BIN = r"C:\Users\ncyc\Desktop\fakedisk\bin"
DISK = r"C:\Users\ncyc\Desktop\fakedisk\test.fdk"

def check(exe, prefix, expect):
    subprocess.Popen([BIN + "\\" + exe, DISK])
    w = wait_win(prefix)
    if not w:
        print(exe, ": ERR no window"); return
    time.sleep(1.2)
    pt = POINT(500, 200); u32.ClientToScreen(w[0], ctypes.byref(pt)); click(pt.x, pt.y)
    time.sleep(0.4)
    key(0x12, True); time.sleep(0.1)
    key(0x46, True); key(0x46, False)
    key(0x12, False)
    time.sleep(0.6)
    key(0x4E, True); key(0x4E, False)   # N = 新建磁盘
    time.sleep(1.0)
    # 找弹窗：标题精确为“fakedisk”（MessageBox）或“新建 fakedisk 磁盘”（新建对话框）
    msgs = enum_windows("fakedisk", exact=True)
    dlg = enum_windows("新建 fakedisk 磁盘")
    got = "MSG:" + (msgs[0][1] if msgs else "none") + " | DLG:" + ("yes" if dlg else "no")
    print(exe, "->", got)
    # 关闭弹窗（Esc / WM_CLOSE）
    for h, _ in msgs + dlg:
        u32.SendMessageW(h, 0x0010, 0, 0)
    time.sleep(0.8)
    u32.SendMessageW(w[0], 0x0010, 0, 0)
    time.sleep(0.8)

check("fakedisk-windows.exe", "fakedisk-windows", "no-permission")
check("fakedisk-windows-manager.exe", "fakedisk-windows-manager", "new-disk")
print("DONE")
