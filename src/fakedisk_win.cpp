// ============================================================================
// fakedisk-windows —— 图形工具
//   默认编译（FDISK_READONLY 未定义）= windows-manager，含全部修改功能
//   定义 FDISK_READONLY 编译        = windows，只读浏览/预览/导出/运行，不写盘
// Win32 原生 GUI：目录树 + 文件列表 + 菜单 + 拖放导入 + 双击打开
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>
#include "fakedisk.h"
#include "resource.h"
#include "dialogs.h"
#include "lang.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

using namespace fdisk;
using namespace Gdiplus;

Disk    g_disk;
static std::wstring g_disk_path;
static HINSTANCE g_hInst;
HWND    g_hWnd = nullptr, g_hTree = nullptr, g_hList = nullptr, g_hStatus = nullptr;
static std::string g_cwd;                 // 当前目录（内部路径，"" = 根）
static HIMAGELIST g_sysImg;               // 系统小图标图像列表
static HIMAGELIST g_bigImg;               // 系统大图标图像列表（桌面视图）
static bool    g_needSave = false;

// 视图模式：列表（详细信息） / 桌面（仿 Windows 桌面大图标网格）
enum ViewMode { FDK_VIEW_LIST = 0, FDK_VIEW_DESKTOP = 1 };
static ViewMode g_viewMode = FDK_VIEW_LIST;

// ---------------------------------------------------------------------------
// 语言：注册表持久化（HKCU\Software\fakedisk\Lang）
// ---------------------------------------------------------------------------
Lang g_lang = Lang::Zh;
static Lang load_lang() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\fakedisk", L"Lang", RRF_RT_REG_DWORD,
                     nullptr, &v, &sz) == ERROR_SUCCESS)
        return v ? Lang::En : Lang::Zh;
    return detect_system_lang();
}
static void save_lang() {
    DWORD v = g_lang == Lang::En ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\fakedisk", L"Lang", REG_DWORD, &v, sizeof(v));
}

// ---------------------------------------------------------------------------
// 编码工具
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
static std::wstring err_w() {
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, GetLastError(), 0, buf, 511, nullptr);
    return std::wstring(buf);
}
static std::wstring fmt_size_u(uint64_t n) {
    wchar_t buf[40];
    if (n >= 1024ULL * 1024 * 1024) swprintf(buf, 40, L"%.2f GB", double(n) / (1024.0 * 1024 * 1024));
    else if (n >= 1024ULL * 1024) swprintf(buf, 40, L"%.2f MB", double(n) / (1024.0 * 1024));
    else if (n >= 1024) swprintf(buf, 40, L"%.2f KB", double(n) / 1024.0);
    else swprintf(buf, 40, L"%llu B", (unsigned long long)n);
    return buf;
}
static std::wstring fmt_time_w(int64_t t) {
    if (t <= 0) return L"-- -- --  --:--";
    time_t tt = (time_t)t;
    struct tm tmv; localtime_s(&tmv, &tt);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02d %02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return buf;
}
static std::wstring type_of(const Entry& e) {
    if (e.is_dir()) return L(L"文件夹", L"Folder");
    size_t dot = e.name.rfind('.');
    if (dot == std::string::npos) return L(L"文件", L"File");
    std::string ext = e.name.substr(dot + 1);
    std::wstring we = u8w(ext);
    for (auto& c : we) c = (wchar_t)towlower(c);
    if (we == L"txt" || we == L"log" || we == L"md" || we == L"ini" || we == L"cfg" || we == L"csv" || we == L"json" || we == L"xml" || we == L"bat" || we == L"cmd") return L(L"文本文件", L"Text file");
    if (we == L"png" || we == L"jpg" || we == L"jpeg" || we == L"bmp" || we == L"gif" || we == L"webp" || we == L"ico") return L(L"图片", L"Image");
    if (we == L"exe" || we == L"com" || we == L"scr") return L(L"应用程序", L"Application");
    return we + L(L" 文件", L" file");
}
static std::wstring attr_w(uint8_t a) {
    std::wstring s;
    s += (a & ATTR_READONLY) ? L"R" : L"-";
    s += (a & ATTR_HIDDEN) ? L"H" : L"-";
    s += (a & ATTR_SYSTEM) ? L"S" : L"-";
    s += (a & ATTR_ARCHIVE) ? L"A" : L"-";
    return s;
}

// ---------------------------------------------------------------------------
// 系统图标
// ---------------------------------------------------------------------------
static void init_sys_images() {
    SHFILEINFOW sfi = {};
    g_sysImg = (HIMAGELIST)SHGetFileInfoW(L"", 0, &sfi, sizeof(sfi),
                                          SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    // 大图标列表（48x48，与系统图标索引顺序一致）
    static const GUID iid_il = { 0x46eb5926, 0x582e, 0x4017, 0x9f, 0x2e,
                                 0x0e, 0xcb, 0x00, 0x94, 0xbb, 0x04 };
    SHGetImageList(2 /*SHIL_EXTRALARGE*/, iid_il, (void**)&g_bigImg);
}
static int icon_of(const Entry& e) {
    SHFILEINFOW sfi = {};
    if (e.is_dir()) {
        SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                       SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    } else {
        std::wstring ext = L"*";
        size_t dot = e.name.rfind('.');
        if (dot != std::string::npos) ext = L"*" + u8w(e.name.substr(dot));
        SHGetFileInfoW(ext.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    }
    return sfi.iIcon;
}

// ---------------------------------------------------------------------------
// 刷新 UI
// ---------------------------------------------------------------------------
static std::string cur_path_str() {
    if (g_cwd.empty()) return "/";
    return g_cwd;
}

// 重建左侧目录树（简单可靠：全量重建）
static void refresh_tree() {
    TreeView_DeleteAllItems(g_hTree);
    if (g_disk_path.empty()) return;
    std::wstring rootname = g_disk.label().empty() ? L"FDISK" : u8w(g_disk.label());
    TVINSERTSTRUCTW tvi = {};
    tvi.hParent = TVI_ROOT;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT | TVIF_PARAM;
    tvi.item.pszText = const_cast<LPWSTR>(rootname.c_str());
    tvi.item.lParam = (LPARAM)&g_disk.root();
    HTREEITEM root = TreeView_InsertItem(g_hTree, &tvi);
    TreeView_SetItemState(g_hTree, root, TVIS_EXPANDED, TVIS_EXPANDED);

    std::function<void(HTREEITEM, Entry&)> add_children = [&](HTREEITEM parent, Entry& dir) {
        for (auto& c : dir.children) {
            if (!c.is_dir()) continue;
            TVINSERTSTRUCTW t2 = {};
            t2.hParent = parent;
            t2.hInsertAfter = TVI_LAST;
            t2.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
            t2.item.pszText = const_cast<LPWSTR>(u8w(c.name).c_str());
            t2.item.lParam = (LPARAM)&c;
            t2.item.iImage = t2.item.iSelectedImage = icon_of(c);
            HTREEITEM h = TreeView_InsertItem(g_hTree, &t2);
            add_children(h, c);
        }
    };
    add_children(root, g_disk.root());

    // 定位当前目录
    if (g_cwd.empty()) TreeView_SelectItem(g_hTree, root);
    else {
        HTREEITEM hit = TreeView_GetRoot(g_hTree);
        std::function<bool(HTREEITEM, const std::string&, size_t)> find =
            [&](HTREEITEM h, const std::string& path, size_t idx) -> bool {
            TVITEMW tvi = {};
            tvi.hItem = h;
            tvi.mask = TVIF_PARAM;
            if (!TreeView_GetItem(g_hTree, &tvi)) return false;
            Entry* e = (Entry*)tvi.lParam;
            if (!e) return false;
            if (idx >= path.size()) return false;
            size_t pos = path.find('/', idx);
            std::string part = path.substr(idx, pos == std::string::npos ? std::string::npos : pos - idx);
            if (e->name != part) return false;
            if (pos == std::string::npos) { TreeView_SelectItem(g_hTree, h); return true; }
            HTREEITEM child = TreeView_GetChild(g_hTree, h);
            while (child) {
                if (find(child, path, pos + 1)) return true;
                child = TreeView_GetNextSibling(g_hTree, child);
            }
            return false;
        };
        HTREEITEM child = TreeView_GetChild(g_hTree, root);
        bool found = false;
        while (child && !found) { found = find(child, g_cwd, 0); child = TreeView_GetNextSibling(g_hTree, child); }
        if (!found) { g_cwd.clear(); TreeView_SelectItem(g_hTree, root); }
    }
}

static void refresh_list() {
    ListView_DeleteAllItems(g_hList);
    if (g_disk_path.empty()) return;
    Entry* dir = g_disk.resolve(g_cwd);
    if (!dir) { g_cwd.clear(); dir = &g_disk.root(); }

    std::vector<Entry*> items;
    for (auto& c : dir->children) items.push_back(&c);
    std::stable_sort(items.begin(), items.end(), [](Entry* a, Entry* b) {
        if (a->is_dir() != b->is_dir()) return a->is_dir();
        return a->name < b->name;
    });

    if (g_viewMode == FDK_VIEW_DESKTOP) {
        // 桌面视图：大图标网格（仿 Windows 桌面），仅图标 + 名称
        int i = 0;
        for (Entry* c : items) {
            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
            lvi.iItem = i;
            lvi.iImage = icon_of(*c);
            lvi.lParam = (LPARAM)c;
            std::wstring wname = u8w(c->name);
            lvi.pszText = const_cast<LPWSTR>(wname.c_str());
            ListView_InsertItem(g_hList, &lvi);
            ++i;
        }
        return;
    }

    int i = 0;
    for (Entry* c : items) {
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        lvi.iItem = i;
        lvi.iImage = icon_of(*c);
        lvi.lParam = (LPARAM)c;
        std::wstring wname = u8w(c->name);
        lvi.pszText = const_cast<LPWSTR>(wname.c_str());
        int idx = ListView_InsertItem(g_hList, &lvi);
        if (c->is_dir()) {
            ListView_SetItemText(g_hList, idx, 1, const_cast<LPWSTR>(L""));
        } else {
            ListView_SetItemText(g_hList, idx, 1, const_cast<LPWSTR>(fmt_size_u(c->size).c_str()));
        }
        ListView_SetItemText(g_hList, idx, 2, const_cast<LPWSTR>(type_of(*c).c_str()));
        ListView_SetItemText(g_hList, idx, 3, const_cast<LPWSTR>(attr_w(c->attr).c_str()));
        ListView_SetItemText(g_hList, idx, 4, const_cast<LPWSTR>(fmt_time_w(c->modified).c_str()));
        ++i;
    }
}

static void refresh_status() {
    if (g_disk_path.empty()) {
        SetWindowTextW(g_hStatus, L(L"未打开磁盘", L"No disk opened"));
        return;
    }
    wchar_t buf[512];
    swprintf(buf, 512, L(L" %s  [%s]    %d 项    已用 %s / %s（剩余 %s）",
                         L" %s  [%s]    %d items    Used %s / %s (free %s)"),
             u8w(cur_path_str()).c_str(),
             g_viewMode == FDK_VIEW_DESKTOP ? L(L"桌面视图", L"Desktop view") : L(L"列表视图", L"List view"),
             (int)g_disk.resolve(g_cwd)->children.size(),
             fmt_size_u(g_disk.used_bytes()).c_str(),
             fmt_size_u(g_disk.capacity()).c_str(),
             fmt_size_u(g_disk.free_bytes()).c_str());
    SetWindowTextW(g_hStatus, buf);
}

// 切换列表 / 桌面视图
static void apply_view() {
    LONG_PTR style = GetWindowLongPtrW(g_hList, GWL_STYLE);
    if (g_viewMode == FDK_VIEW_DESKTOP) {
        style = (style & ~(LONG_PTR)LVS_TYPEMASK) | LVS_ICON | LVS_AUTOARRANGE | LVS_ALIGNTOP;
        SetWindowLongPtrW(g_hList, GWL_STYLE, style);
        ListView_SetImageList(g_hList, g_bigImg, LVSIL_NORMAL);
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_DOUBLEBUFFER);
    } else {
        style = (style & ~(LONG_PTR)LVS_TYPEMASK) | LVS_REPORT;
        SetWindowLongPtrW(g_hList, GWL_STYLE, style);
        ListView_SetImageList(g_hList, g_sysImg, LVSIL_SMALL);
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    }
    // 强制重排与重填
    SetWindowPos(g_hList, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    refresh_list();
    refresh_status();
}

void refresh_all() { refresh_tree(); refresh_list(); refresh_status(); }

// ---------------------------------------------------------------------------
// 保存
// ---------------------------------------------------------------------------
bool save_disk() {
    if (g_disk_path.empty()) return true;
    std::string err;
    if (!g_disk.save(err)) {
        MessageBoxW(g_hWnd, (L(L"保存失败：", L"Save failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
        return false;
    }
    g_needSave = false;
    return true;
}

// ---------------------------------------------------------------------------
// 打开 / 新建
// ---------------------------------------------------------------------------
bool open_disk(const std::wstring& file) {
    std::string err;
    if (!g_disk.open(file, err)) {
        MessageBoxW(g_hWnd, (L(L"打开失败：", L"Open failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
        return false;
    }
    g_disk_path = file;
    g_cwd.clear();
    refresh_all();
    return true;
}

static void menu_open_dialog() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = L(L"fakedisk 磁盘 (*.fdk)\0*.fdk\0所有文件 (*.*)\0*.*\0",
                        L"fakedisk Disk (*.fdk)\0*.fdk\0All Files (*.*)\0*.*\0");
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L(L"打开 fakedisk 磁盘", L"Open fakedisk Disk");
    if (GetOpenFileNameW(&ofn)) open_disk(file);
}

static void menu_export_dialog() {
    if (g_disk_path.empty()) return;
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0) { MessageBoxW(g_hWnd, L(L"请先在列表中选择一个文件", L"Please select a file in the list first"), L"fakedisk", MB_ICONINFORMATION); return; }
    LVITEMW lvi = {}; lvi.mask = LVIF_PARAM; lvi.iItem = sel;
    ListView_GetItem(g_hList, &lvi);
    Entry* e = (Entry*)lvi.lParam;
    if (!e || e->is_dir()) { MessageBoxW(g_hWnd, L(L"请选择一个文件（不支持导出目录）", L"Please select a file (exporting folders is not supported)"), L"fakedisk", MB_ICONINFORMATION); return; }
    wchar_t out[MAX_PATH] = {};
    wcscpy_s(out, u8w(e->name).c_str());
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = L(L"所有文件 (*.*)\0*.*\0", L"All Files (*.*)\0*.*\0");
    ofn.lpstrFile = out;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L(L"导出文件到…", L"Export file to...");
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    std::string intpath = g_cwd.empty() ? ("/" + e->name) : (g_cwd + "/" + e->name);
    std::string err;
    if (!g_disk.export_file(intpath, out, err))
        MessageBoxW(g_hWnd, (L(L"导出失败：", L"Export failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
}

static void menu_import_files() {
    if (g_disk_path.empty()) return;
    wchar_t files[32768] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFilter = L(L"所有文件 (*.*)\0*.*\0", L"All Files (*.*)\0*.*\0");
    ofn.lpstrFile = files;
    ofn.nMaxFile = 32768;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L(L"导入文件到当前目录", L"Import files into current folder");
    if (!GetOpenFileNameW(&ofn)) return;
    // 解析多选结果
    std::wstring dir;
    std::vector<std::wstring> names;
    const wchar_t* p = files;
    while (*p) {
        std::wstring s = p;
        if (dir.empty()) dir = s;
        else names.push_back(s);
        p += s.size() + 1;
    }
    if (names.empty()) names.push_back(dir); // 单选时第一个就是完整路径
    int ok = 0;
    for (auto& n : names) {
        std::wstring full = n.find(L'\\') == std::wstring::npos ? dir + L"\\" + n : n;
        std::string err;
        if (g_disk.import_file(g_cwd, full, err)) ++ok;
    }
    g_needSave = true;
    save_disk();
    refresh_list(); refresh_status();
    MessageBoxW(g_hWnd, (std::to_wstring(ok) + L(L" 个文件已导入", L" file(s) imported")).c_str(), L"fakedisk", MB_ICONINFORMATION);
}

static void menu_import_folder() {
    if (g_disk_path.empty()) return;
    BROWSEINFOW bi = {};
    bi.hwndOwner = g_hWnd;
    bi.lpszTitle = L(L"选择要导入的文件夹（递归导入）", L"Choose a folder to import (recursive)");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH] = {};
    if (!SHGetPathFromIDListW(pidl, path)) { CoTaskMemFree(pidl); return; }
    CoTaskMemFree(pidl);

    // 递归导入（复用核心 xcopy 逻辑：此处直接遍历）
    std::function<bool(const std::wstring&, const std::string&)> rec =
        [&](const std::wstring& ext_dir, const std::string& int_dir) -> bool {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((ext_dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool allok = true;
        do {
            std::wstring nm = fd.cFileName;
            if (nm == L"." || nm == L"..") continue;
            std::string nm8 = wu8(nm);
            std::string err;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::string sub = int_dir + "/" + nm8;
                if (!g_disk.resolve(sub)) {
                    if (!g_disk.mkdir(sub, err) && err.find("已存在") == std::string::npos) allok = false;
                }
                if (!rec(ext_dir + L"\\" + nm, sub)) allok = false;
            } else {
                if (!g_disk.import_file(int_dir + "/" + nm8, ext_dir + L"\\" + nm, err)) allok = false;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return allok;
    };
    std::string dest = g_cwd;
    rec(path, dest.empty() ? "/" : dest);
    g_needSave = true;
    save_disk();
    refresh_all();
}

// ---------------------------------------------------------------------------
// 打开条目（双击）
// ---------------------------------------------------------------------------
static void show_text_viewer(const std::string& name, const std::wstring& text);
static void show_image_viewer(const std::string& name, const std::vector<uint8_t>& data);

static void open_entry(Entry* e) {
    if (!e) return;
    if (e->is_dir()) {
        if (g_cwd.empty()) g_cwd = e->name; else g_cwd += "/" + e->name;
        refresh_tree(); refresh_list(); refresh_status();
        return;
    }
    std::string intpath = g_cwd.empty() ? ("/" + e->name) : (g_cwd + "/" + e->name);
    std::vector<uint8_t> data;
    std::string err;
    if (!g_disk.read_file(intpath, data, err)) {   // 数据在内存中读取
        MessageBoxW(g_hWnd, (L(L"读取失败：", L"Read failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
        return;
    }
    std::wstring wext = type_of(*e);
    if (wext == L(L"文本文件", L"Text file")) {
        std::wstring text = u8w(std::string((const char*)data.data(), data.size()));
        show_text_viewer(e->name, text);
    } else if (wext == L(L"图片", L"Image")) {
        show_image_viewer(e->name, data);
    } else {
        // 程序 / 其他文件：提取到临时目录后打开
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring base = std::wstring(tmp) + L"fakedisk_extract\\";
        CreateDirectoryW(base.c_str(), nullptr);
        std::wstring dir = base + (g_disk.label().empty() ? L"disk" : u8w(g_disk.label())) + L"\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring out = dir + u8w(e->name);
        std::string werr;
        if (!write_external_file(out, data.data(), data.size(), werr)) {
            MessageBoxW(g_hWnd, (L(L"提取失败：", L"Extract failed: ") + u8w(werr)).c_str(), L"fakedisk", MB_ICONERROR);
            return;
        }
        HINSTANCE r = ShellExecuteW(g_hWnd, L"open", out.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32)
            MessageBoxW(g_hWnd, L(L"无法打开（系统未关联程序或执行被拒绝）", L"Cannot open (no associated program or execution was denied)"), L"fakedisk", MB_ICONERROR);
    }
}

// 文本查看器窗口
static void show_text_viewer(const std::string& name, const std::wstring& text) {
    class TextWin {
    public:
        static ATOM reg() {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = proc;
            wc.hInstance = g_hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            wc.lpszClassName = L"FakeDiskTextViewer";
            return RegisterClassW(&wc);
        }
        static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
            if (m == WM_CREATE) {
                HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                          ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                          0, 0, 0, 0, h, nullptr, g_hInst, nullptr);
                SetWindowTextW(ed, ((CREATESTRUCTW*)l)->lpCreateParams ? (LPCWSTR)((CREATESTRUCTW*)l)->lpCreateParams : L"");
                // 等宽字体
                HFONT font = CreateFontW(-16, 0, 0, 0, 0, 0, 0, 0, GB2312_CHARSET, 0, 0, 0,
                                         FIXED_PITCH | FF_MODERN, L"Consolas");
                SendMessageW(ed, WM_SETFONT, (WPARAM)font, TRUE);
            } else if (m == WM_SIZE) {
                HWND ed = GetWindow(h, GW_CHILD);
                if (ed) MoveWindow(ed, 0, 0, LOWORD(l), HIWORD(l), TRUE);
            } else if (m == WM_CLOSE) {
                DestroyWindow(h);
            } else if (m == WM_DESTROY) {
                // 无主窗口，不退出消息循环
            }
            return DefWindowProcW(h, m, w, l);
        }
    };
    static ATOM cls = TextWin::reg();
    std::wstring title = u8w(name) + L(L" - 文本查看", L" - Text Viewer");
    HWND wnd = CreateWindowExW(0, L"FakeDiskTextViewer", title.c_str(),
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               640, 480, nullptr, nullptr, g_hInst, (void*)text.c_str());
    ShowWindow(wnd, SW_SHOW);
}

// 图片查看器窗口（GDI+）
static void show_image_viewer(const std::string& name, const std::vector<uint8_t>& data) {
    class ImgWin {
    public:
        static ATOM reg() {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = proc;
            wc.hInstance = g_hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            wc.lpszClassName = L"FakeDiskImageViewer";
            return RegisterClassW(&wc);
        }
        static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
            if (m == WM_PAINT) {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(h, &ps);
                Bitmap* bmp = (Bitmap*)GetWindowLongPtrW(h, GWLP_USERDATA);
                if (bmp) {
                    RECT rc; GetClientRect(h, &rc);
                    int w = rc.right, ht = rc.bottom;
                    int iw = bmp->GetWidth(), ih = bmp->GetHeight();
                    double scale = (std::min)(double(w) / iw, double(ht) / ih);
                    if (scale > 1.0) scale = 1.0;
                    int dw = (int)(iw * scale), dh = (int)(ih * scale);
                    int x = (w - dw) / 2, y = (ht - dh) / 2;
                    Graphics g(hdc);
                    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                    g.DrawImage(bmp, x, y, dw, dh);
                }
                EndPaint(h, &ps);
            } else if (m == WM_CLOSE) {
                Bitmap* bmp = (Bitmap*)GetWindowLongPtrW(h, GWLP_USERDATA);
                if (bmp) delete bmp;
                DestroyWindow(h);
            }
            return DefWindowProcW(h, m, w, l);
        }
    };
    // 内存流 → Bitmap
    IStream* st = nullptr;
    ULONG written = 0;
    if (!SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st)) ||
        !SUCCEEDED(st->Write(data.data(), (ULONG)data.size(), &written))) {
        if (st) st->Release();
        MessageBoxW(g_hWnd, L(L"图片解码失败", L"Failed to decode image"), L"fakedisk", MB_ICONERROR);
        return;
    }
    Bitmap* bmp = Bitmap::FromStream(st);
    st->Release();
    if (!bmp || bmp->GetLastStatus() != Ok) {
        if (bmp) delete bmp;
        MessageBoxW(g_hWnd, L(L"不支持的图片格式", L"Unsupported image format"), L"fakedisk", MB_ICONERROR);
        return;
    }
    static ATOM cls = ImgWin::reg();
    std::wstring title = u8w(name) + L(L" - 图片查看", L" - Image Viewer");
    HWND wnd = CreateWindowExW(0, L"FakeDiskImageViewer", title.c_str(),
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               (int)(bmp->GetWidth()) + 40, (int)(bmp->GetHeight()) + 60,
                               nullptr, nullptr, g_hInst, nullptr);
    SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)bmp);
    ShowWindow(wnd, SW_SHOW);
}

// ---------------------------------------------------------------------------
// 删除 / 重命名 / 新建文件夹
// ---------------------------------------------------------------------------
static void delete_selected() {
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0) return;
    LVITEMW lvi = {}; lvi.mask = LVIF_PARAM; lvi.iItem = sel;
    ListView_GetItem(g_hList, &lvi);
    Entry* e = (Entry*)lvi.lParam;
    if (!e) return;
    std::wstring q = L(L"确定删除 ", L"Delete ") + u8w(e->name) +
                     (e->is_dir() ? L(L" 及其中所有内容？", L" and everything inside it?") : L(L" ？", L"?"));
    if (MessageBoxW(g_hWnd, q.c_str(), L"fakedisk", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    std::string intpath = g_cwd.empty() ? ("/" + e->name) : (g_cwd + "/" + e->name);
    std::string err;
    if (!g_disk.remove(intpath, err)) {
        MessageBoxW(g_hWnd, (L(L"删除失败：", L"Delete failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
        return;
    }
    g_needSave = true;
    save_disk();
    refresh_all();
}

static void rename_selected() {
    int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (sel < 0) return;
    LVITEMW lvi = {}; lvi.mask = LVIF_PARAM; lvi.iItem = sel;
    ListView_GetItem(g_hList, &lvi);
    Entry* e = (Entry*)lvi.lParam;
    if (!e) return;
    wchar_t buf[512];
    wcscpy_s(buf, u8w(e->name).c_str());
    if (input_dialog(L(L"重命名", L"Rename"), L(L"新名称：", L"New name:"), buf, 511)) {
        std::string intpath = g_cwd.empty() ? ("/" + e->name) : (g_cwd + "/" + e->name);
        std::string err;
        if (!g_disk.rename(intpath, wu8(buf), err)) {
            MessageBoxW(g_hWnd, (L(L"重命名失败：", L"Rename failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
            return;
        }
        g_needSave = true;
        save_disk();
        refresh_all();
    }
}

static void new_folder() {
    wchar_t buf[512] = {};
    if (input_dialog(L(L"新建文件夹", L"New Folder"), L(L"文件夹名称：", L"Folder name:"), buf, 511)) {
        std::string path = g_cwd.empty() ? ("/" + wu8(buf)) : (g_cwd + "/" + wu8(buf));
        std::string err;
        if (!g_disk.mkdir(path, err)) {
            MessageBoxW(g_hWnd, (L(L"新建失败：", L"Create failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
            return;
        }
        g_needSave = true;
        save_disk();
        refresh_all();
    }
}

// ---------------------------------------------------------------------------
// 对话框（定义在 dialogs.cpp 之外的内部函数）
// ---------------------------------------------------------------------------
bool input_dialog(const std::wstring& title, const std::wstring& prompt, wchar_t* buf, int maxlen);

// ---------------------------------------------------------------------------
// 多语言：菜单文本动态化 + 列头重建 + 界面整体重设
// ---------------------------------------------------------------------------
static void set_menu_text(HMENU menu, UINT id, const wchar_t* text) {
    MENUITEMINFOW mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = const_cast<LPWSTR>(text);
    SetMenuItemInfoW(menu, id, FALSE, &mii);
}

static void apply_menu_language(HMENU bar) {
    // 注意：不可用 static 缓存（首次调用会固化语言，切换后不更新）
    const wchar_t* top[6] = {
        L(L"文件(&F)", L"&File"), L(L"编辑(&E)", L"&Edit"), L(L"工具(&T)", L"&Tools"),
        L(L"视图(&V)", L"&View"), L(L"语言(&L)", L"&Language"), L(L"帮助(&H)", L"&Help")
    };
    for (int i = 0; i < 6; ++i) {
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING;
        mii.dwTypeData = const_cast<LPWSTR>(top[i]);
        SetMenuItemInfoW(bar, i, TRUE, &mii);
    }
    HMENU file = GetSubMenu(bar, 0), edit = GetSubMenu(bar, 1), tools = GetSubMenu(bar, 2),
          view = GetSubMenu(bar, 3), lang = GetSubMenu(bar, 4), help = GetSubMenu(bar, 5);
    set_menu_text(file, IDM_OPEN,          L(L"打开磁盘(&O)...", L"&Open Disk..."));
    set_menu_text(file, IDM_NEW,           L(L"新建磁盘(&N)...", L"&New Disk..."));
    set_menu_text(file, IDM_IMPORT_FILES,  L(L"导入文件(&I)...", L"&Import Files..."));
    set_menu_text(file, IDM_IMPORT_FOLDER, L(L"导入文件夹(&D)...", L"Import &Folder..."));
    set_menu_text(file, IDM_EXPORT,        L(L"导出(&E)...", L"&Export..."));
    set_menu_text(file, IDM_CLOSE,         L(L"关闭磁盘(&C)", L"&Close Disk"));
    set_menu_text(file, IDM_EXIT,          L(L"退出(&X)", L"E&xit"));
    set_menu_text(edit, IDM_NEWFOLDER,     L(L"新建文件夹(&W)", L"&New Folder"));
    set_menu_text(edit, IDM_RENAME,        L(L"重命名(&R)\tF2", L"&Rename\tF2"));
    set_menu_text(edit, IDM_DELETE,        L(L"删除(&D)\tDel", L"&Delete\tDel"));
    set_menu_text(edit, IDM_PROP,          L(L"属性(&P)...", L"&Properties..."));
    set_menu_text(tools, IDM_FSCK,         L(L"校验完整性(&K)", L"&Verify Integrity"));
    set_menu_text(tools, IDM_DEFRAG,       L(L"整理数据区(&G)", L"&Defragment"));
    set_menu_text(tools, IDM_RUN,          L(L"打开/运行选中项(&U)", L"&Open/Run Selected"));
    set_menu_text(view, IDM_VIEW_DESKTOP,  L(L"桌面视图(&D)", L"&Desktop View"));
    set_menu_text(view, IDM_VIEW_LIST,     L(L"列表视图(&L)", L"&List View"));
    set_menu_text(help, IDM_ABOUT,         L(L"关于(&A)", L"&About..."));
    // 语言菜单本身不翻译（语言名），仅勾选当前
    CheckMenuRadioItem(lang, IDM_LANG_ZH, IDM_LANG_EN,
                       g_lang == Lang::En ? IDM_LANG_EN : IDM_LANG_ZH, MF_BYCOMMAND);
}

static void insert_columns() {
    const wchar_t* cols[] = { L(L"名称", L"Name"), L(L"大小", L"Size"), L(L"类型", L"Type"),
                              L(L"属性", L"Attr"), L(L"修改时间", L"Modified") };
    int widths[] = { 200, 90, 90, 70, 130 };
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (int i = 0; i < 5; ++i) {
        col.pszText = const_cast<LPWSTR>(cols[i]);
        col.cx = widths[i];
        col.iSubItem = i;
        ListView_InsertColumn(g_hList, i, &col);
    }
}

static const wchar_t* main_title() {
#ifdef FDISK_READONLY
    return L(L"fakedisk-windows — 单文件虚拟磁盘（只读）",
             L"fakedisk-windows — Single-file Virtual Disk (Read-only)");
#else
    return L(L"fakedisk-windows-manager — 单文件虚拟磁盘（读写）",
             L"fakedisk-windows-manager — Single-file Virtual Disk (Read/Write)");
#endif
}

// 语言切换后整体重设界面
static void apply_ui_language() {
    HMENU bar = GetMenu(g_hWnd);
    if (bar) { apply_menu_language(bar); DrawMenuBar(g_hWnd); }
    SetWindowTextW(g_hWnd, main_title());
    for (int i = 4; i >= 0; --i) ListView_DeleteColumn(g_hList, i);
    insert_columns();
    refresh_all();
}

// ---------------------------------------------------------------------------
// 菜单命令
// ---------------------------------------------------------------------------
static void on_command(WORD id) {
    switch (id) {
    case IDM_OPEN: menu_open_dialog(); break;
#ifdef FDISK_READONLY
    // 只读版：修改类按钮保持可点击，但提示无权操作
    case IDM_NEW:
    case IDM_IMPORT_FILES:
    case IDM_IMPORT_FOLDER:
    case IDM_NEWFOLDER:
    case IDM_RENAME:
    case IDM_DELETE:
    case IDM_PROP:
    case IDM_DEFRAG:
        MessageBoxW(g_hWnd,
                    L(L"你无权进行此操作\n\n当前为只读工具，修改请使用 fakedisk-windows-manager.exe。",
                      L"You are not allowed to perform this operation.\n\nThis is a read-only tool. "
                      L"Please use fakedisk-windows-manager.exe for modifications."),
                    L"fakedisk", MB_ICONWARNING);
        break;
#else
    case IDM_NEW: new_disk_dialog(); break;
#endif
    case IDM_CLOSE:
        if (!g_disk_path.empty()) {
#ifndef FDISK_READONLY
            save_disk();
#endif
            g_disk_path.clear(); g_cwd.clear();
            refresh_all();
        }
        break;
#ifndef FDISK_READONLY
    case IDM_IMPORT_FILES: menu_import_files(); break;
    case IDM_IMPORT_FOLDER: menu_import_folder(); break;
    case IDM_NEWFOLDER: new_folder(); break;
    case IDM_DELETE: delete_selected(); break;
    case IDM_RENAME: rename_selected(); break;
    case IDM_PROP: prop_dialog(); break;
#endif
    case IDM_FSCK: {
        if (g_disk_path.empty()) break;
        std::string err; std::vector<std::string> probs;
        bool ok = g_disk.verify(err, &probs);
        if (ok) MessageBoxW(g_hWnd, L(L"fsck 校验通过：目录表与全部文件内容完整。",
                                      L"fsck passed: TOC and all file contents are intact."),
                            L"fakedisk", MB_ICONINFORMATION);
        else {
            std::wstring msg = L(L"发现 ", L"Found ") + std::to_wstring(probs.size()) + L(L" 个问题：\n", L" problem(s):\n");
            for (auto& p : probs) { msg += L" • "; msg += u8w(p); msg += L"\n"; }
            MessageBoxW(g_hWnd, msg.c_str(), L"fakedisk", MB_ICONWARNING);
        }
        break;
    }
#ifndef FDISK_READONLY
    case IDM_DEFRAG: {
        if (g_disk_path.empty()) break;
        std::string err;
        if (g_disk.defrag(err)) { g_needSave = true; save_disk(); refresh_status(); }
        else MessageBoxW(g_hWnd, (L(L"整理失败：", L"Defragment failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
        break;
    }
#endif
    case IDM_RUN: {
        int sel = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
        if (sel >= 0) {
            LVITEMW lvi = {}; lvi.mask = LVIF_PARAM; lvi.iItem = sel;
            ListView_GetItem(g_hList, &lvi);
            open_entry((Entry*)lvi.lParam);
        }
        break;
    }
    case IDM_VIEW_DESKTOP:
        g_viewMode = FDK_VIEW_DESKTOP;
        apply_view();
        break;
    case IDM_VIEW_LIST:
        g_viewMode = FDK_VIEW_LIST;
        apply_view();
        break;
    case IDM_LANG_ZH:
    case IDM_LANG_EN:
        g_lang = (id == IDM_LANG_EN) ? Lang::En : Lang::Zh;
        save_lang();
        apply_ui_language();
        break;
    case IDM_ABOUT:
#ifdef FDISK_READONLY
        MessageBoxW(g_hWnd, L(L"fakedisk-windows v1.0（只读）\n单文件虚拟磁盘图形工具\n\n"
                   L"支持：打开磁盘、浏览结构、导出文件、fsck 校验、\n"
                   L"文本与图片内存预览、程序提取运行。\n"
                   L"修改操作（导入/删除/新建/属性等）请使用\nfakedisk-windows-manager.exe。",
                   L"fakedisk-windows v1.0 (read-only)\nSingle-file virtual disk GUI tool\n\n"
                   L"Features: open disk, browse structure, export files, fsck verify,\n"
                   L"in-memory text & image preview, extract & run programs.\n"
                   L"Modifying operations (import/delete/new/properties etc.) require\nfakedisk-windows-manager.exe."),
                   L(L"关于 fakedisk", L"About fakedisk"), MB_ICONINFORMATION);
#else
        MessageBoxW(g_hWnd, L(L"fakedisk-windows-manager v1.0\n单文件虚拟磁盘图形工具（读写）\n\n"
                   L"支持：打开/新建磁盘、导入/导出文件与文件夹、\n"
                   L"新建/删除/重命名/属性修改、fsck 校验、整理、\n"
                   L"文本与图片内存预览、程序提取运行、拖放导入。",
                   L"fakedisk-windows-manager v1.0\nSingle-file virtual disk GUI tool (read/write)\n\n"
                   L"Features: open/new disk, import/export files & folders,\n"
                   L"new/delete/rename/properties, fsck verify, defragment,\n"
                   L"in-memory text & image preview, extract & run programs, drag & drop import."),
                   L(L"关于 fakedisk", L"About fakedisk"), MB_ICONINFORMATION);
#endif
        break;
    }
}

// ---------------------------------------------------------------------------
// 主窗口过程
// ---------------------------------------------------------------------------
static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hWnd = hwnd;
        // 树
        g_hTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_LINESATROOT |
                                  TVS_HASBUTTONS | TVS_SHOWSELALWAYS | WS_VSCROLL,
                                  0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);
        TreeView_SetImageList(g_hTree, g_sysImg, TVSIL_NORMAL);
        // 列表
        g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                  0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);
        ListView_SetImageList(g_hList, g_sysImg, LVSIL_SMALL);
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        insert_columns();
        // 状态栏
        g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                    WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                    0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);
#ifndef FDISK_READONLY
        DragAcceptFiles(hwnd, TRUE);   // 读写版才接收拖放导入
#endif
        refresh_all();
        return 0;
    }
    case WM_INITMENUPOPUP:
        // 勾选当前视图模式（对不包含这些项的弹出菜单调用无害）
        CheckMenuRadioItem((HMENU)wp, IDM_VIEW_DESKTOP, IDM_VIEW_LIST,
                           g_viewMode == FDK_VIEW_DESKTOP ? IDM_VIEW_DESKTOP : IDM_VIEW_LIST,
                           MF_BYCOMMAND);
        // 勾选当前语言（语言菜单打开时）
        CheckMenuRadioItem((HMENU)wp, IDM_LANG_ZH, IDM_LANG_EN,
                           g_lang == Lang::En ? IDM_LANG_EN : IDM_LANG_ZH,
                           MF_BYCOMMAND);
        return 0;
    case WM_SIZE: {
        if (!g_hTree) break;
        RECT rc; GetClientRect(hwnd, &rc);
        int sh = 22;
        int tree_w = (rc.right * 28) / 100;
        MoveWindow(g_hTree, 0, 0, tree_w, rc.bottom - sh, TRUE);
        MoveWindow(g_hList, tree_w, 0, rc.right - tree_w, rc.bottom - sh, TRUE);
        MoveWindow(g_hStatus, 0, rc.bottom - sh, rc.right, sh, TRUE);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->hwndFrom == g_hTree && hdr->code == TVN_SELCHANGEDW) {
            HTREEITEM sel = TreeView_GetSelection(g_hTree);
            if (sel) {
                // 由选中节点重建 g_cwd
                std::vector<std::string> parts;
                HTREEITEM it = sel;
                while (it) {
                    TVITEMW tvi = {};
                    tvi.hItem = it;
                    tvi.mask = TVIF_PARAM;
                    TreeView_GetItem(g_hTree, &tvi);
                    Entry* e = (Entry*)tvi.lParam;
                    if (e && e != &g_disk.root()) parts.push_back(e->name);
                    it = TreeView_GetParent(g_hTree, it);
                }
                std::reverse(parts.begin(), parts.end());
                g_cwd.clear();
                for (auto& p : parts) { if (g_cwd.empty()) g_cwd = p; else g_cwd += "/" + p; }
                refresh_list();
                refresh_status();
            }
        }
        if (hdr->hwndFrom == g_hList && hdr->code == NM_DBLCLK) {
            LVHITTESTINFO ht = {};
            DWORD pos = GetMessagePos();
            ht.pt.x = GET_X_LPARAM(pos); ht.pt.y = GET_Y_LPARAM(pos);
            ScreenToClient(g_hList, &ht.pt);
            int idx = ListView_HitTest(g_hList, &ht);
            if (idx >= 0) {
                LVITEMW lvi = {};
                lvi.mask = LVIF_PARAM;
                lvi.iItem = idx;
                if (ListView_GetItem(g_hList, &lvi))
                    open_entry((Entry*)lvi.lParam);
            }
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t path[MAX_PATH];
            DragQueryFileW(drop, i, path, MAX_PATH);
            std::string err;
            DWORD attr = GetFileAttributesW(path);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                // 目录：递归导入
                std::function<bool(const std::wstring&, const std::string&)> rec =
                    [&](const std::wstring& ed, const std::string& id) -> bool {
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW((ed + L"\\*").c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE) return false;
                    bool ok = true;
                    do {
                        std::wstring nm = fd.cFileName;
                        if (nm == L"." || nm == L"..") continue;
                        std::string nm8 = wu8(nm);
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            std::string sub = id + "/" + nm8;
                            if (!g_disk.resolve(sub)) { std::string e2; g_disk.mkdir(sub, e2); }
                            if (!rec(ed + L"\\" + nm, sub)) ok = false;
                        } else {
                            std::string e2;
                            if (!g_disk.import_file(id + "/" + nm8, ed + L"\\" + nm, e2)) ok = false;
                        }
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                    return ok;
                };
                rec(path, g_cwd.empty() ? "/" : g_cwd);
            } else {
                if (!g_disk.import_file(g_cwd, path, err)) {
                    MessageBoxW(g_hWnd, (L(L"导入失败：", L"Import failed: ") + u8w(err)).c_str(), L"fakedisk", MB_ICONERROR);
                }
            }
        }
        DragFinish(drop);
        g_needSave = true;
        save_disk();
        refresh_all();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) >= IDM_FIRST) on_command(LOWORD(wp));
        return 0;
    case WM_CLOSE:
#ifndef FDISK_READONLY
        save_disk();
#endif
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdline, int nShow) {
    g_hInst = hInst;
    SetProcessDPIAware();
    g_lang = load_lang();   // 注册表 -> 系统语言检测

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    GdiplusStartupInput gsi;
    ULONG_PTR gdiToken;
    GdiplusStartup(&gdiToken, &gsi, nullptr);

    init_sys_images();
    register_dialogs();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_MAIN));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"FakeDiskMain";
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MENU);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"FakeDiskMain", main_title(),
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                960, 620, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);
    apply_menu_language(GetMenu(hwnd));   // 菜单按当前语言重设
    DrawMenuBar(hwnd);                    // 强制重绘菜单栏（否则保留资源中文）

    // 命令行参数：直接打开磁盘
    std::wstring cl = cmdline ? cmdline : L"";
    if (!cl.empty()) {
        // 去掉引号
        if (cl.size() >= 2 && cl.front() == L'"' && cl.back() == L'"') cl = cl.substr(1, cl.size() - 2);
        open_disk(cl);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    GdiplusShutdown(gdiToken);
    return 0;
}
