// ============================================================================
// fakedisk-windows 对话框模块（新建磁盘 / 属性 / 输入框）
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include "fakedisk.h"
#include "resource.h"
#include "dialogs.h"
#include "lang.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")

using namespace fdisk;

// ---------------------------------------------------------------------------
// 编码工具（与主文件一致）
// ---------------------------------------------------------------------------
static std::wstring u8w(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string wu8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring fmt_size_w(uint64_t n) {
    wchar_t buf[40];
    if (n >= 1024ULL * 1024 * 1024) swprintf(buf, 40, L"%.2f GB", double(n) / (1024.0 * 1024 * 1024));
    else if (n >= 1024ULL * 1024) swprintf(buf, 40, L"%.2f MB", double(n) / (1024.0 * 1024));
    else if (n >= 1024) swprintf(buf, 40, L"%.2f KB", double(n) / 1024.0);
    else swprintf(buf, 40, L"%llu B", (unsigned long long)n);
    return buf;
}
static std::wstring fmt_time_w2(int64_t t) {
    if (t <= 0) return L(L"未知", L"unknown");
    time_t tt = (time_t)t;
    struct tm tmv; localtime_s(&tmv, &tt);
    wchar_t buf[40];
    swprintf(buf, 40, L"%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

// ---------------------------------------------------------------------------
// 输入框对话框（新建文件夹 / 重命名共用）
// ---------------------------------------------------------------------------
struct InputCtx { std::wstring prompt; wchar_t* buf; int maxlen; };

static INT_PTR CALLBACK input_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INITDIALOG) {
        InputCtx* ctx = (InputCtx*)l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)ctx);
        SetDlgItemTextW(h, IDC_INPUT_PROMPT, ctx->prompt.c_str());
        SetDlgItemTextW(h, IDC_INPUT_EDIT, ctx->buf);
        SetDlgItemTextW(h, IDOK, L(L"确定", L"OK"));
        SetDlgItemTextW(h, IDCANCEL, L(L"取消", L"Cancel"));
        SendDlgItemMessageW(h, IDC_INPUT_EDIT, EM_SETSEL, 0, -1);
        return TRUE;
    }
    if (m == WM_COMMAND) {
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            InputCtx* ctx = (InputCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (LOWORD(w) == IDOK && ctx)
                GetDlgItemTextW(h, IDC_INPUT_EDIT, ctx->buf, ctx->maxlen);
            EndDialog(h, LOWORD(w) == IDOK);
            return TRUE;
        }
    }
    return FALSE;
}

bool input_dialog(const std::wstring& title, const std::wstring& prompt,
                  wchar_t* buf, int maxlen) {
    InputCtx ctx = { prompt, buf, maxlen };
    return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_INPUT),
                           g_hWnd, input_proc, (LPARAM)&ctx) == TRUE;
}

// ---------------------------------------------------------------------------
// 新建磁盘对话框
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK new_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INITDIALOG) {
        SetWindowTextW(h, L(L"新建 fakedisk 磁盘", L"New fakedisk Disk"));
        SetDlgItemTextW(h, IDC_NEW_FILE_LABEL, L(L"磁盘文件:", L"Disk file:"));
        SetDlgItemTextW(h, IDC_NEW_BROWSE, L(L"浏览...", L"Browse..."));
        SetDlgItemTextW(h, IDC_NEW_CAP_LABEL, L(L"容量:", L"Capacity:"));
        SetDlgItemTextW(h, IDC_NEW_LABEL_LABEL, L(L"卷标:", L"Label:"));
        SetDlgItemTextW(h, IDOK, L(L"确定", L"OK"));
        SetDlgItemTextW(h, IDCANCEL, L(L"取消", L"Cancel"));
        const wchar_t* caps[] = { L"1M", L"4M", L"16M", L"32M", L"64M", L"128M", L"256M", L"512M", L"1G", L"4G", L"16G" };
        for (auto* c : caps) SendDlgItemMessageW(h, IDC_NEW_CAP, CB_ADDSTRING, 0, (LPARAM)c);
        SendDlgItemMessageW(h, IDC_NEW_CAP, CB_SETCURSEL, 4, 0); // 默认 64M
        return TRUE;
    }
    if (m == WM_COMMAND) {
        if (LOWORD(w) == IDC_NEW_BROWSE) {
            wchar_t file[MAX_PATH] = L"disk.fdk";
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = h;
            ofn.lpstrFilter = L(L"fakedisk 磁盘 (*.fdk)\0*.fdk\0所有文件 (*.*)\0*.*\0",
                                L"fakedisk Disk (*.fdk)\0*.fdk\0All Files (*.*)\0*.*\0");
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L(L"选择新磁盘文件位置", L"Choose location for new disk");
            ofn.Flags = OFN_OVERWRITEPROMPT;
            if (GetSaveFileNameW(&ofn)) SetDlgItemTextW(h, IDC_NEW_PATH, file);
            return TRUE;
        }
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            if (LOWORD(w) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
            wchar_t path[MAX_PATH] = {}, cap[64] = {}, label[64] = {};
            GetDlgItemTextW(h, IDC_NEW_PATH, path, MAX_PATH);
            GetDlgItemTextW(h, IDC_NEW_CAP, cap, 63);
            GetDlgItemTextW(h, IDC_NEW_LABEL, label, 63);
            if (!path[0]) { MessageBoxW(h, L(L"请选择磁盘文件路径", L"Please choose a disk file path"), L"fakedisk", MB_ICONINFORMATION); return TRUE; }
            std::string err;
            if (!Disk::create(path, wu8(cap), wu8(label), err)) {
                MessageBoxW(h, (L(L"创建失败：", L"Create failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
                return TRUE;
            }
            EndDialog(h, 1);
            open_disk(path);
            return TRUE;
        }
    }
    return FALSE;
}

void new_disk_dialog() {
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_NEW),
                    g_hWnd, new_proc, 0);
}

// ---------------------------------------------------------------------------
// 属性对话框
// ---------------------------------------------------------------------------
struct PropCtx { Entry* entry; bool is_root; };

// 由 Entry 反推内部路径（模态对话框期间 Entry* 有效）
static std::string find_entry_path(Entry* target) {
    std::string result;
    std::function<bool(Entry&, const std::string&)> find =
        [&](Entry& dir, const std::string& prefix) -> bool {
        for (auto& c : dir.children) {
            std::string cur = prefix.empty() ? ("/" + c.name) : (prefix + "/" + c.name);
            if (&c == target) { result = cur; return true; }
            if (c.is_dir() && find(c, cur)) return true;
        }
        return false;
    };
    if (find(g_disk.root(), "")) return result;
    return "";
}

static INT_PTR CALLBACK prop_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INITDIALOG) {
        PropCtx* ctx = (PropCtx*)l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)ctx);
        Entry* e = ctx->entry;
        SetWindowTextW(h, L(L"属性", L"Properties"));
        SetDlgItemTextW(h, IDC_PROP_NAME_LABEL, L(L"名称:", L"Name:"));
        SetDlgItemTextW(h, IDC_PROP_TYPE_LABEL, L(L"类型:", L"Type:"));
        SetDlgItemTextW(h, IDC_PROP_SIZE_LABEL, L(L"大小:", L"Size:"));
        SetDlgItemTextW(h, IDC_PROP_ATTR_GROUP, L(L"属性", L"Attributes"));
        SetDlgItemTextW(h, IDC_PROP_RO, L(L"只读(&R)", L"Read-only(&R)"));
        SetDlgItemTextW(h, IDC_PROP_H, L(L"隐藏(&H)", L"Hidden(&H)"));
        SetDlgItemTextW(h, IDC_PROP_S, L(L"系统(&S)", L"System(&S)"));
        SetDlgItemTextW(h, IDC_PROP_A, L(L"存档(&A)", L"Archive(&A)"));
        SetDlgItemTextW(h, IDC_PROP_LABEL_LABEL, L(L"卷标:", L"Label:"));
        SetDlgItemTextW(h, IDOK, L(L"确定", L"OK"));
        SetDlgItemTextW(h, IDCANCEL, L(L"取消", L"Cancel"));
        std::wstring type = e->is_dir() ? L(L"文件夹", L"Folder") : L(L"文件", L"File");
        size_t dot = e->name.rfind('.');
        if (!e->is_dir() && dot != std::string::npos) type = u8w(e->name.substr(dot + 1)) + L(L" 文件", L" file");
        SetDlgItemTextW(h, IDC_PROP_NAME, u8w(e->name).c_str());
        SetDlgItemTextW(h, IDC_PROP_TYPE, type.c_str());
        SetDlgItemTextW(h, IDC_PROP_SIZE,
                        e->is_dir() ? L(L"（目录）", L"(folder)") : fmt_size_w(e->size).c_str());
        std::wstring tm = L(L"创建: ", L"Created: ") + fmt_time_w2(e->created) +
                          L(L"\r\n修改: ", L"\r\nModified: ") + fmt_time_w2(e->modified);
        SetDlgItemTextW(h, IDC_PROP_TIME, tm.c_str());
        CheckDlgButton(h, IDC_PROP_RO, (e->attr & ATTR_READONLY) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_PROP_H, (e->attr & ATTR_HIDDEN) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_PROP_S, (e->attr & ATTR_SYSTEM) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(h, IDC_PROP_A, (e->attr & ATTR_ARCHIVE) ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(h, IDC_PROP_LABEL, u8w(g_disk.label()).c_str());
        return TRUE;
    }
    if (m == WM_COMMAND) {
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            if (LOWORD(w) == IDOK) {
                PropCtx* ctx = (PropCtx*)GetWindowLongPtrW(h, GWLP_USERDATA);
                if (ctx) {
                    Entry* e = ctx->entry;
                    uint8_t add = 0, del = 0;
                    auto sync = [&](int id, uint8_t bit) {
                        if (IsDlgButtonChecked(h, id) == BST_CHECKED) add |= bit;
                        else del |= bit;
                    };
                    sync(IDC_PROP_RO, ATTR_READONLY);
                    sync(IDC_PROP_H, ATTR_HIDDEN);
                    sync(IDC_PROP_S, ATTR_SYSTEM);
                    sync(IDC_PROP_A, ATTR_ARCHIVE);
                    std::string err;
                    if (!ctx->is_root && (add || del)) {
                        std::string p = find_entry_path(e);
                        if (p.empty() || !g_disk.set_attr(p, add, del, err))
                            MessageBoxW(h, (L(L"修改属性失败：", L"Failed to change attributes: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
                    }
                    wchar_t label[64] = {};
                    GetDlgItemTextW(h, IDC_PROP_LABEL, label, 63);
                    g_disk.set_label(wu8(label));
                    if (!g_disk.save(err)) {
                        MessageBoxW(h, (L(L"保存失败：", L"Save failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
                    }
                }
            }
            EndDialog(h, LOWORD(w) == IDOK);
            return TRUE;
        }
    }
    return FALSE;
}

void prop_dialog() {
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0) { MessageBoxW(g_hWnd, L(L"请先选择一个条目", L"Please select an item first"), L"fakedisk", MB_ICONINFORMATION); return; }
    LVITEMW lvi = {};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = sel;
    ListView_GetItem(g_hList, &lvi);
    Entry* e = (Entry*)lvi.lParam;
    if (!e) return;
    PropCtx ctx = { e, false };
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PROP),
                    g_hWnd, prop_proc, (LPARAM)&ctx);
}

void register_dialogs() {
    // 对话框资源由 .rc 编译，无需运行时注册
}
