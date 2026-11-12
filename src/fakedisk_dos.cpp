// ============================================================================
// fakedisk-dos —— 命令行工具
//   默认编译（FDISK_READONLY 未定义）= dos-manager，含全部修改命令
//   定义 FDISK_READONLY 编译        = dos，只读浏览/导出/运行，不写盘
// 用法: fakedisk-dos [磁盘文件]    交互式 MS-DOS 风格 shell
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <ctime>
#include <iostream>
#include <algorithm>
#include "fakedisk.h"
#include "lang.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace fdisk;

Lang g_lang = Lang::Zh;

static Disk g_disk;
static std::wstring g_disk_path;
static std::string g_cwd;           // 当前内部目录，"" 表示根
static bool g_dirty = false;        // 有未保存修改（本实现每次修改即保存，此标志保留备用）

// ---------------------------------------------------------------------------
// 控制台 / 编码工具
// ---------------------------------------------------------------------------
static std::string utf16_to_utf8_s(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring utf8_to_utf16_s(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static void to_lower(std::string& s) { for (auto& c : s) c = (char)tolower((unsigned char)c); }

// 命令参数拆分：支持双引号包裹（含空格）的路径
static std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false;
    for (char c : line) {
        if (c == '"') { inq = !inq; continue; }
        if (c == ' ' && !inq) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// 格式化时间戳为 YYYY-MM-DD HH:MM
static std::string fmt_time(int64_t t) {
    if (t <= 0) return "-- -- --  --:--";
    time_t tt = (time_t)t;
    struct tm tmv;
    localtime_s(&tmv, &tt);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return buf;
}
static std::string fmt_size(uint64_t n) {
    char buf[32];
    if (n >= 1024ULL * 1024 * 1024) snprintf(buf, sizeof(buf), "%.2f GB", double(n) / (1024.0 * 1024 * 1024));
    else if (n >= 1024ULL * 1024) snprintf(buf, sizeof(buf), "%.2f MB", double(n) / (1024.0 * 1024));
    else if (n >= 1024) snprintf(buf, sizeof(buf), "%.2f KB", double(n) / 1024.0);
    else snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
    return buf;
}
static std::string attr_str(uint8_t attr) {
    std::string s;
    s += (attr & ATTR_READONLY) ? "R" : "-";
    s += (attr & ATTR_HIDDEN) ? "H" : "-";
    s += (attr & ATTR_SYSTEM) ? "S" : "-";
    s += (attr & ATTR_ARCHIVE) ? "A" : "-";
    return s;
}

// ---------------------------------------------------------------------------
// 路径展开（支持 .. 与相对路径），返回规范绝对内部路径（"" 表示根）
// ---------------------------------------------------------------------------
static std::string expand_path(const std::string& in) {
    std::string joined;
    if (in.empty()) joined = g_cwd;
    else if (in[0] == '/' || in[0] == '\\') joined = in;
    else joined = g_cwd.empty() ? in : g_cwd + "/" + in;

    std::vector<std::string> stack;
    std::string cur;
    for (size_t i = 0; i <= joined.size(); ++i) {
        char c = (i < joined.size()) ? joined[i] : '/';
        if (c == '/' || c == '\\') {
            if (cur.empty()) { continue; }
            if (cur == ".") { cur.clear(); continue; }
            if (cur == "..") {
                if (!stack.empty()) stack.pop_back();
                cur.clear();
                continue;
            }
            stack.push_back(cur);
            cur.clear();
        } else cur.push_back(c);
    }
    std::string out;
    for (auto& s : stack) { out += "/"; out += s; }
    return out; // "" = 根
}

// 提示符：myDisk:\path>
static std::string prompt() {
    std::wstring file = g_disk_path;
    size_t pos = file.find_last_of(L"\\/");
    if (pos != std::wstring::npos) file = file.substr(pos + 1);
    size_t dot = file.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) file = file.substr(0, dot);
    std::string name = utf16_to_utf8_s(file);
    if (name.empty()) name = "FDISK";
    return name + ":" + (g_cwd.empty() ? "\\" : g_cwd) + "> ";
}

static void print_err(const std::string& e) { printf("%s\n", e.c_str()); }

// ---------------------------------------------------------------------------
// 命令实现
// ---------------------------------------------------------------------------
static bool save_checked() {
    std::string err;
    if (!g_disk.save(err)) { print_err(std::string(L8("保存失败: ", "Save failed: ")) + err); return false; }
    g_dirty = false;
    return true;
}

static int cmd_help(const std::vector<std::string>&);
static int cmd_exit(const std::vector<std::string>&);
static std::string prompt_part();
static int cmd_format(const std::vector<std::string>& args) {
#ifndef FDISK_READONLY
    if (args.size() < 2) { printf(L8("用法: format <文件> [容量(K/M/G/T)] [/v:卷标]\n",
                                     "Usage: format <file> [capacity(K/M/G/T)] [/v:label]\n")); return 1; }
    std::string cap = (args.size() >= 3 && args[2][0] != '/') ? args[2] : "";
    std::string label;
    for (auto& a : args) if (a.rfind("/v:", 0) == 0) label = a.substr(3);
    std::string err;
    if (!Disk::create(utf8_to_utf16_s(args[1]), cap, label, err)) { print_err(std::string(L8("创建失败: ", "Create failed: ")) + err); return 1; }
    if (!g_disk.open(utf8_to_utf16_s(args[1]), err)) { print_err(std::string(L8("打开失败: ", "Open failed: ")) + err); return 1; }
    g_disk_path = utf8_to_utf16_s(args[1]);
    g_cwd.clear();
    printf(L8("已创建并挂载 fakedisk：%s（容量 %s，卷标 %s）\n",
              "Created and mounted fakedisk: %s (capacity %s, label %s)\n"),
           args[1].c_str(),
           fmt_size(g_disk.capacity()).c_str(), g_disk.label().empty() ? L8("(无)", "(none)") : g_disk.label().c_str());
    return 0;
#else
    printf(L8("只读工具：format 需要 fakedisk-dos-manager.exe\n",
              "Read-only tool: format requires fakedisk-dos-manager.exe\n"));
    return 1;
#endif
}

static int cmd_open(const std::vector<std::string>& args) {
    if (args.size() < 2) { printf(L8("用法: open <文件>\n", "Usage: open <file>\n")); return 1; }
    std::string err;
    if (!g_disk.open(utf8_to_utf16_s(args[1]), err)) { print_err(std::string(L8("打开失败: ", "Open failed: ")) + err); return 1; }
    g_disk_path = utf8_to_utf16_s(args[1]);
    g_cwd.clear();
    printf(L8("已打开：%s（卷标 %s，已用 %s / %s）\n",
              "Opened: %s (label %s, used %s / %s)\n"), args[1].c_str(),
           g_disk.label().empty() ? L8("(无)", "(none)") : g_disk.label().c_str(),
           fmt_size(g_disk.used_bytes()).c_str(), fmt_size(g_disk.capacity()).c_str());
    return 0;
}

static int cmd_close(const std::vector<std::string>&) {
    if (g_disk_path.empty()) { printf(L8("当前没有打开的磁盘\n", "No disk is currently open\n")); return 1; }
#ifndef FDISK_READONLY
    std::string err;
    if (!g_disk.save(err)) { print_err(std::string(L8("保存失败: ", "Save failed: ")) + err); return 1; }
    g_disk_path.clear(); g_cwd.clear();
    printf(L8("已关闭并保存\n", "Closed and saved\n"));
#else
    g_disk_path.clear(); g_cwd.clear();
    printf(L8("已关闭（只读，不保存）\n", "Closed (read-only, not saved)\n"));
#endif
    return 0;
}

static int cmd_dir(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘，先 open 或 format", "No disk open, run open or format first")); return 1; }
    std::string target = args.size() >= 2 ? expand_path(args[1]) : g_cwd;
    Entry* e = g_disk.resolve(target);
    if (!e) { print_err(L8("路径不存在", "Path does not exist")); return 1; }
    if (!e->is_dir()) { print_err(L8("不是目录", "Not a directory")); return 1; }

    printf(L8(" 卷 %s 中的卷是 %s\n", " Volume in drive %s is %s\n"),
           prompt_part().c_str(), g_disk.label().empty() ? "FDISK" : g_disk.label().c_str());
    printf(L8(" 目录: %s\n\n", " Directory: %s\n\n"), target.empty() ? "\\" : target.c_str());

    std::vector<const Entry*> dirs, files;
    for (auto& c : e->children) (c.is_dir() ? dirs : files).push_back(&c);
    std::sort(dirs.begin(), dirs.end(), [](auto a, auto b) { return a->name < b->name; });
    std::sort(files.begin(), files.end(), [](auto a, auto b) { return a->name < b->name; });

    printf("%-12s %-10s %-16s %s\n", "<DIR>", "", "--", ".");
    printf("%-12s %-10s %-16s %s\n", "<DIR>", "", "--", "..");
    uint64_t sum = 0; int fcnt = 0, dcnt = 0;
    for (auto* d : dirs) {
        printf("%-12s %-10s %-16s %s\n", "<DIR>", "", fmt_time(d->modified).c_str(), d->name.c_str());
        ++dcnt;
    }
    for (auto* f : files) {
        char sz[16]; snprintf(sz, sizeof(sz), "%llu", (unsigned long long)f->size);
        printf("%-12s %-10s %-16s %s\n", sz, attr_str(f->attr).c_str(),
               fmt_time(f->modified).c_str(), f->name.c_str());
        sum += f->size; ++fcnt;
    }
    printf(L8("\n %d 个文件  %s 字节\n", "\n %d File(s)  %s bytes\n"), fcnt, fmt_size(sum).c_str());
    printf(L8(" %d 个目录  %s 可用（共 %s）\n", " %d Dir(s)  %s free (of %s)\n"), dcnt,
           fmt_size(g_disk.free_bytes()).c_str(), fmt_size(g_disk.capacity()).c_str());
    return 0;
}

// dir 打印卷名用的辅助（同 prompt 的盘名）
static std::string prompt_part();

static int cmd_cd(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 2) { g_cwd.clear(); return 0; }
    std::string t = expand_path(args[1]);
    Entry* e = g_disk.resolve(t);
    if (!e || !e->is_dir()) { print_err(L8("目录不存在", "Directory does not exist")); return 1; }
    g_cwd = t;
    return 0;
}

static int cmd_mkdir(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 2) { printf(L8("用法: mkdir <路径>\n", "Usage: mkdir <path>\n")); return 1; }
    std::string err;
    if (!g_disk.mkdir(expand_path(args[1]), err)) { print_err(err); return 1; }
    return save_checked() ? 0 : 1;
}

static int cmd_rmdir(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 2) { printf(L8("用法: rmdir <路径>\n", "Usage: rmdir <path>\n")); return 1; }
    std::string t = expand_path(args[1]);
    Entry* e = g_disk.resolve(t);
    if (!e) { print_err(L8("路径不存在", "Path does not exist")); return 1; }
    if (!e->is_dir()) { print_err(L8("不是目录（删除文件请用 del）", "Not a directory (use del for files)")); return 1; }
    std::string err;
    if (!g_disk.remove(t, err)) { print_err(err); return 1; }
    return save_checked() ? 0 : 1;
}

static int cmd_del(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 2) { printf(L8("用法: del <路径>\n", "Usage: del <path>\n")); return 1; }
    std::string t = expand_path(args[1]);
    Entry* e = g_disk.resolve(t);
    if (!e) { print_err(L8("路径不存在", "Path does not exist")); return 1; }
    if (e->is_dir()) { print_err(L8("是目录（删除目录请用 rmdir）", "Is a directory (use rmdir for directories)")); return 1; }
    std::string err;
    if (!g_disk.remove(t, err)) { print_err(err); return 1; }
    return save_checked() ? 0 : 1;
}

static int cmd_copy(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 3) { printf(L8("用法: copy <外部文件> <内部路径>\n", "Usage: copy <external file> <internal path>\n")); return 1; }
    std::string err;
    if (!g_disk.import_file(expand_path(args[2]), utf8_to_utf16_s(args[1]), err)) { print_err(err); return 1; }
    bool ok = save_checked();
    if (ok) printf(L8("已导入：%s → %s\n", "Imported: %s -> %s\n"), args[1].c_str(), expand_path(args[2]).c_str());
    return ok ? 0 : 1;
}

// xcopy <外部目录> <内部目录> ：递归导入整个目录
static bool import_dir_recursive(const std::wstring& ext_dir, const std::string& int_dir) {
    std::wstring pat = ext_dir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { printf(L8("无法读取外部目录\n", "Cannot read external directory\n")); return false; }
    bool ok = true;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::string name8 = utf16_to_utf8_s(name);
        std::string err;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::string sub = int_dir + "/" + name8;
            if (!g_disk.mkdir(sub, err)) {
                // 已存在则忽略（引擎错误消息固定为中文，不随界面语言）
                if (err.find("已存在") == std::string::npos) { printf(L8("mkdir %s 失败: %s\n", "mkdir %s failed: %s\n"), sub.c_str(), err.c_str()); ok = false; }
            }
            if (!import_dir_recursive(ext_dir + L"\\" + name, sub)) ok = false;
        } else {
            if (!g_disk.import_file(int_dir + "/" + name8, ext_dir + L"\\" + name, err)) {
                printf(L8("导入 %s 失败: %s\n", "Import %s failed: %s\n"), name8.c_str(), err.c_str()); ok = false;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static int cmd_xcopy(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 3) { printf(L8("用法: xcopy <外部目录> <内部目录>\n", "Usage: xcopy <external dir> <internal dir>\n")); return 1; }
    std::string int_dir = expand_path(args[2]);
    if (int_dir.empty()) int_dir = "/";
    Entry* e = g_disk.resolve(int_dir);
    if (!e || !e->is_dir()) {
        std::string err;
        if (!g_disk.mkdir(int_dir, err)) { print_err(err); return 1; }
    }
    if (!import_dir_recursive(utf8_to_utf16_s(args[1]), int_dir)) { return 1; }
    return save_checked() ? 0 : 1;
}

static int cmd_export(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 3) { printf(L8("用法: export <内部路径> <外部文件>\n", "Usage: export <internal path> <external file>\n")); return 1; }
    std::string err;
    if (!g_disk.export_file(expand_path(args[1]), utf8_to_utf16_s(args[2]), err)) { print_err(err); return 1; }
    printf(L8("已导出：%s → %s\n", "Exported: %s -> %s\n"), args[1].c_str(), args[2].c_str());
    return 0;
}

static int cmd_type(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 2) { printf(L8("用法: type <内部路径>\n", "Usage: type <internal path>\n")); return 1; }
    std::vector<uint8_t> data;
    std::string err;
    if (!g_disk.read_file(expand_path(args[1]), data, err)) { print_err(err); return 1; }
    // 在内存中读取后打印（文本）；控制字符替换为 '.'
    for (uint8_t b : data) {
        if (b == '\r' || b == '\n' || b == '\t') { putchar(b); continue; }
        if (b < 0x20) putchar('.');
        else putchar(b);
    }
    printf("\n");
    return 0;
}

static int cmd_attr(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 2) { printf(L8("用法: attr <路径> [+R|-R] [+H|-H] [+S|-S] [+A|-A]\n",
                                     "Usage: attr <path> [+R|-R] [+H|-H] [+S|-S] [+A|-A]\n")); return 1; }
    std::string t = expand_path(args[1]);
    Entry* e = g_disk.resolve(t);
    if (!e) { print_err(L8("路径不存在", "Path does not exist")); return 1; }
    uint8_t add = 0, del = 0;
    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a.size() < 2) continue;
        uint8_t bit = 0;
        char c = (char)toupper((unsigned char)a[1]);
        if (c == 'R') bit = ATTR_READONLY;
        else if (c == 'H') bit = ATTR_HIDDEN;
        else if (c == 'S') bit = ATTR_SYSTEM;
        else if (c == 'A') bit = ATTR_ARCHIVE;
        else continue;
        if (a[0] == '+') add |= bit;
        else if (a[0] == '-') del |= bit;
    }
    std::string err;
    if (!g_disk.set_attr(t, add, del, err)) { print_err(err); return 1; }
    printf(L8("%s 属性 → %s\n", "%s attributes -> %s\n"), t.c_str(), attr_str(e->attr).c_str());
    return save_checked() ? 0 : 1;
}

static int cmd_ren(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 3) { printf(L8("用法: ren <路径> <新名称>\n", "Usage: ren <path> <new name>\n")); return 1; }
    std::string err;
    if (!g_disk.rename(expand_path(args[1]), args[2], err)) { print_err(err); return 1; }
    return save_checked() ? 0 : 1;
}

static int cmd_move(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 3) { printf(L8("用法: move <源路径> <目标路径>\n", "Usage: move <source> <destination>\n")); return 1; }
    std::string err;
    if (!g_disk.move(expand_path(args[1]), expand_path(args[2]), err)) { print_err(err); return 1; }
    return save_checked() ? 0 : 1;
}

static int cmd_touch(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 2) { printf(L8("用法: touch <路径>\n", "Usage: touch <path>\n")); return 1; }
    std::string err;
    if (!g_disk.set_time(expand_path(args[1]), 0, now_unix(), err)) { print_err(err); return 1; }
    return save_checked() ? 0 : 1;
}

static int cmd_vol(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() >= 2) {
#ifndef FDISK_READONLY
        g_disk.set_label(args[1]);
        return save_checked() ? 0 : 1;
#else
        printf(L8("只读工具：设置卷标需要 fakedisk-dos-manager.exe\n",
                  "Read-only tool: setting label requires fakedisk-dos-manager.exe\n"));
        return 1;
#endif
    }
    printf(L8(" 卷 %s 中的卷是 %s\n", " Volume in drive %s is %s\n"), prompt_part().c_str(),
           g_disk.label().empty() ? "FDISK" : g_disk.label().c_str());
    return 0;
}

static int cmd_info(const std::vector<std::string>&) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    printf(L8("磁盘文件 : %s\n", "Disk file : %s\n"), utf16_to_utf8_s(g_disk_path).c_str());
    printf(L8("卷标     : %s\n", "Label     : %s\n"), g_disk.label().empty() ? L8("(无)", "(none)") : g_disk.label().c_str());
    printf(L8("容量     : %s\n", "Capacity  : %s\n"), fmt_size(g_disk.capacity()).c_str());
    printf(L8("已用     : %s（%s 字节）\n", "Used      : %s (%s bytes)\n"),
           fmt_size(g_disk.used_bytes()).c_str(), std::to_string(g_disk.used_bytes()).c_str());
    printf(L8("存储占用 : %s\n", "Stored    : %s\n"), fmt_size(g_disk.stored_bytes()).c_str());
    printf(L8("剩余     : %s\n", "Free      : %s\n"), fmt_size(g_disk.free_bytes()).c_str());
    printf(L8("压缩     : %s\n", "Compressed: %s\n"), g_disk.has_compressed() ? L8("是", "Yes") : L8("否", "No"));
    printf(L8("格式版本 : %u\n", "Format v. : %u\n"), FORMAT_VERSION);
    return 0;
}

static int cmd_fsck(const std::vector<std::string>&) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    std::string err;
    std::vector<std::string> problems;
    bool ok = g_disk.verify(err, &problems);
    if (ok) printf(L8("fsck：未发现错误（目录表 CRC、偏移与全部文件内容校验通过）\n",
                      "fsck: no errors found (TOC CRC, offsets and all file contents verified)\n"));
    else {
        printf(L8("fsck：发现 %d 个问题\n", "fsck: found %d problem(s)\n"), (int)problems.size());
        for (auto& p : problems) printf("  - %s\n", p.c_str());
    }
    return ok ? 0 : 1;
}

static int cmd_defrag(const std::vector<std::string>&) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    std::string err;
    if (!g_disk.defrag(err)) { print_err(err); return 1; }
    printf(L8("已整理数据区：存储占用 %s\n", "Data area defragmented: stored %s\n"),
           fmt_size(g_disk.stored_bytes()).c_str());
    return 0;
}

static int cmd_run(const std::vector<std::string>& args) {
    if (g_disk_path.empty()) { print_err(L8("没有打开的磁盘", "No disk is currently open")); return 1; }
    if (args.size() < 2) { printf(L8("用法: run <内部路径>\n", "Usage: run <internal path>\n")); return 1; }
    std::string t = expand_path(args[1]);
    std::vector<uint8_t> data;
    std::string err;
    if (!g_disk.read_file(t, data, err)) { print_err(err); return 1; }   // 数据文件在内存中读取
    Entry* e = g_disk.resolve(t);
    if (!e) { print_err(L8("路径不存在", "Path does not exist")); return 1; }

    // 提取到临时目录（程序 / 其他文件统一提取后打开）
    wchar_t tmpbuf[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpbuf);
    std::wstring lab = utf8_to_utf16_s(g_disk.label());
    if (lab.empty()) lab = L"disk";
    std::wstring base = std::wstring(tmpbuf) + L"fakedisk_extract\\";
    CreateDirectoryW(base.c_str(), nullptr);
    std::wstring dir = base + lab + L"\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring out = dir + utf8_to_utf16_s(e->name);
    if (!write_external_file(out, data.data(), data.size(), err)) { print_err(std::string(L8("提取失败: ", "Extract failed: ")) + err); return 1; }

    HINSTANCE r = ShellExecuteW(nullptr, L"open", out.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) { print_err(L8("无法打开（系统未关联程序或执行被拒绝）", "Cannot open (no associated program or execution was denied)")); return 1; }
    printf(L8("已提取并打开：%s\n", "Extracted and opened: %s\n"), utf16_to_utf8_s(out).c_str());
    return 0;
}

static int cmd_cls(const std::vector<std::string>&) { system("cls"); return 0; }

// ---------------------------------------------------------------------------
// 命令表
// ---------------------------------------------------------------------------
struct Cmd { const char* name; const char* alias; int (*fn)(const std::vector<std::string>&); const char* help; };
#ifdef FDISK_READONLY
// 只读版：修改命令统一拦截，提示使用 dos-manager
static int cmd_ro_stub(const std::vector<std::string>&) {
    printf(L8("只读工具：此操作需要 fakedisk-dos-manager.exe\n",
              "Read-only tool: this operation requires fakedisk-dos-manager.exe\n"));
    return 1;
}
static const Cmd cmds[] = {
    {"help",   "?",      cmd_help,  "显示帮助"},
    {"open",   nullptr,  cmd_open,  "open <文件>  打开已有 fakedisk"},
    {"close",  nullptr,  cmd_close, "关闭当前 fakedisk（只读，不保存）"},
    {"dir",    "ls",     cmd_dir,   "dir [路径]  列出目录"},
    {"cd",     nullptr,  cmd_cd,    "cd <路径>  切换目录（cd\\ 回根）"},
    {"export", nullptr,  cmd_export,"export <内部路径> <外部文件>  导出文件"},
    {"type",   nullptr,  cmd_type,  "type <路径>  在内存中读取并显示文本文件"},
    {"vol",    nullptr,  cmd_vol,   "vol  显示卷标"},
    {"info",   nullptr,  cmd_info,  "info  显示磁盘信息"},
    {"fsck",   "chkdsk", cmd_fsck,  "fsck  校验容器完整性"},
    {"run",    nullptr,  cmd_run,   "run <路径>  打开内部文件/运行内部程序"},
    {"cls",    nullptr,  cmd_cls,   "清屏"},
    {"exit",   "quit",   cmd_exit,  "退出"},
};
#else
static const Cmd cmds[] = {
    {"help",   "?",      cmd_help,  "显示帮助"},
    {"format", nullptr,  cmd_format,"format <文件> [容量] [/v:卷标]  创建新 fakedisk 并挂载"},
    {"open",   nullptr,  cmd_open,  "open <文件>  打开已有 fakedisk"},
    {"close",  nullptr,  cmd_close, "关闭当前 fakedisk 并保存"},
    {"dir",    "ls",     cmd_dir,   "dir [路径]  列出目录"},
    {"cd",     nullptr,  cmd_cd,    "cd <路径>  切换目录（cd\\ 回根）"},
    {"mkdir",  "md",     cmd_mkdir, "mkdir <路径>  新建目录"},
    {"rmdir",  "rd",     cmd_rmdir, "rmdir <路径>  删除空目录"},
    {"del",    "erase",  cmd_del,   "del <路径>  删除文件"},
    {"copy",   nullptr,  cmd_copy,  "copy <外部文件> <内部路径>  导入外部文件"},
    {"xcopy",  nullptr,  cmd_xcopy, "xcopy <外部目录> <内部目录>  递归导入整个目录"},
    {"export", nullptr,  cmd_export,"export <内部路径> <外部文件>  导出文件"},
    {"type",   nullptr,  cmd_type,  "type <路径>  在内存中读取并显示文本文件"},
    {"attr",   nullptr,  cmd_attr,  "attr <路径> [+R|-R][+H|-H][+S|-S][+A|-A]  修改属性"},
    {"ren",    "rename", cmd_ren,   "ren <路径> <新名称>  重命名"},
    {"move",   nullptr,  cmd_move,  "move <源> <目标>  移动条目"},
    {"touch",  nullptr,  cmd_touch, "touch <路径>  更新修改时间"},
    {"vol",    nullptr,  cmd_vol,   "vol [新卷标]  显示/设置卷标"},
    {"info",   nullptr,  cmd_info,  "info  显示磁盘信息"},
    {"fsck",   "chkdsk", cmd_fsck,  "fsck  校验容器完整性"},
    {"defrag", nullptr,  cmd_defrag,"defrag  整理数据区"},
    {"run",    nullptr,  cmd_run,   "run <路径>  打开内部文件/运行内部程序"},
    {"cls",    nullptr,  cmd_cls,   "清屏"},
    {"exit",   "quit",   cmd_exit,  "退出"},
};
#endif
static const int kCmdCount = (int)(sizeof(cmds) / sizeof(cmds[0]));

#ifdef FDISK_READONLY
// 只读版：已知的修改命令名（命令表中未注册，命中时给出明确提示）
static const char* const kRO_CMDS[] = {
    "format", "mkdir", "md", "rmdir", "rd", "del", "erase",
    "copy", "xcopy", "attr", "ren", "rename", "move", "touch", "defrag"
};
static bool is_ro_cmd(const std::string& c) {
    for (auto* s : kRO_CMDS) if (c == s) return true;
    return false;
}
#endif

static int cmd_exit(const std::vector<std::string>&) { return -1; }

static std::string prompt_part() {
    std::wstring file = g_disk_path;
    size_t pos = file.find_last_of(L"\\/");
    if (pos != std::wstring::npos) file = file.substr(pos + 1);
    size_t dot = file.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) file = file.substr(0, dot);
    return utf16_to_utf8_s(file);
}

static int cmd_help(const std::vector<std::string>&) {
#ifdef FDISK_READONLY
    static const char* const zh[] = {
        "显示帮助",
        "open <文件>  打开已有 fakedisk",
        "关闭当前 fakedisk（只读，不保存）",
        "dir [路径]  列出目录",
        "cd <路径>  切换目录（cd\\ 回根）",
        "export <内部路径> <外部文件>  导出文件",
        "type <路径>  在内存中读取并显示文本文件",
        "vol  显示卷标",
        "info  显示磁盘信息",
        "fsck  校验容器完整性",
        "run <路径>  打开内部文件/运行内部程序",
        "清屏",
        "退出",
    };
    static const char* const en[] = {
        "Show help",
        "open <file>  Open an existing fakedisk",
        "Close current fakedisk (read-only, not saved)",
        "dir [path]  List directory",
        "cd <path>  Change directory (cd\\ to root)",
        "export <internal path> <external file>  Export file",
        "type <path>  Read in memory and display a text file",
        "vol  Show volume label",
        "info  Show disk information",
        "fsck  Verify container integrity",
        "run <path>  Open internal file / run internal program",
        "Clear screen",
        "Exit",
    };
    printf(L8("fakedisk-dos v1.0 —— 单文件虚拟磁盘命令行工具（只读）\n",
              "fakedisk-dos v1.0 - Single-file virtual disk CLI tool (read-only)\n"));
    printf(L8("修改操作请使用 fakedisk-dos-manager.exe\n\n",
              "Use fakedisk-dos-manager.exe for modifying operations\n\n"));
#else
    static const char* const zh[] = {
        "显示帮助",
        "format <文件> [容量] [/v:卷标]  创建新 fakedisk 并挂载",
        "open <文件>  打开已有 fakedisk",
        "关闭当前 fakedisk 并保存",
        "dir [路径]  列出目录",
        "cd <路径>  切换目录（cd\\ 回根）",
        "mkdir <路径>  新建目录",
        "rmdir <路径>  删除空目录",
        "del <路径>  删除文件",
        "copy <外部文件> <内部路径>  导入外部文件",
        "xcopy <外部目录> <内部目录>  递归导入整个目录",
        "export <内部路径> <外部文件>  导出文件",
        "type <路径>  在内存中读取并显示文本文件",
        "attr <路径> [+R|-R][+H|-H][+S|-S][+A|-A]  修改属性",
        "ren <路径> <新名称>  重命名",
        "move <源> <目标>  移动条目",
        "touch <路径>  更新修改时间",
        "vol [新卷标]  显示/设置卷标",
        "info  显示磁盘信息",
        "fsck  校验容器完整性",
        "defrag  整理数据区",
        "run <路径>  打开内部文件/运行内部程序",
        "清屏",
        "退出",
    };
    static const char* const en[] = {
        "Show help",
        "format <file> [capacity] [/v:label]  Create a new fakedisk and mount it",
        "open <file>  Open an existing fakedisk",
        "Close current fakedisk and save",
        "dir [path]  List directory",
        "cd <path>  Change directory (cd\\ to root)",
        "mkdir <path>  Create directory",
        "rmdir <path>  Remove empty directory",
        "del <path>  Delete file",
        "copy <external file> <internal path>  Import external file",
        "xcopy <external dir> <internal dir>  Recursively import a directory",
        "export <internal path> <external file>  Export file",
        "type <path>  Read in memory and display a text file",
        "attr <path> [+R|-R][+H|-H][+S|-S][+A|-A]  Change attributes",
        "ren <path> <new name>  Rename",
        "move <source> <dest>  Move item",
        "touch <path>  Update modification time",
        "vol [new label]  Show/set volume label",
        "info  Show disk information",
        "fsck  Verify container integrity",
        "defrag  Defragment data area",
        "run <path>  Open internal file / run internal program",
        "Clear screen",
        "Exit",
    };
    printf(L8("fakedisk-dos-manager v1.0 —— 单文件虚拟磁盘命令行工具（读写）\n\n",
              "fakedisk-dos-manager v1.0 - Single-file virtual disk CLI tool (read/write)\n\n"));
#endif
    for (int i = 0; i < kCmdCount; ++i)
        printf("  %-8s %s\n", cmds[i].name, g_lang == Lang::En ? en[i] : zh[i]);
    printf(L8("\n路径使用 / 或 \\ 分隔；支持相对路径与 ..；含空格路径请加双引号。\n",
              "\nPaths use / or \\; relative paths and .. are supported; quote paths with spaces.\n"));
    return 0;
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------
static int run_shell() {
    std::string line;
    while (true) {
        printf("%s", prompt().c_str());
        fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        // 去掉首尾空白
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        size_t b = line.find_last_not_of(" \t");
        line = line.substr(a, b - a + 1);
        if (line.empty()) continue;

        auto tok = tokenize(line);
        std::string cmd = tok[0];
        to_lower(cmd);

        const Cmd* found = nullptr;
        for (int i = 0; i < kCmdCount; ++i) {
            if (cmd == cmds[i].name || (cmds[i].alias && cmd == cmds[i].alias)) { found = &cmds[i]; break; }
        }
        if (!found) {
#ifdef FDISK_READONLY
            if (is_ro_cmd(cmd))
                printf(L8("只读工具：%s 需要 fakedisk-dos-manager.exe\n",
                          "Read-only tool: %s requires fakedisk-dos-manager.exe\n"), tok[0].c_str());
            else
#endif
            printf(L8("未知命令：%s（输入 help 查看命令列表）\n",
                      "Unknown command: %s (type help for the command list)\n"), tok[0].c_str());
            continue;
        }
        int r = found->fn(tok);
        if (r == -1) break;
    }
    // 退出前保存（只读版不写盘）
    if (!g_disk_path.empty()) {
#ifndef FDISK_READONLY
        std::string err;
        if (!g_disk.save(err)) printf(L8("\n退出时保存失败: %s\n", "\nFailed to save on exit: %s\n"), err.c_str());
        else printf(L8("\n已保存并退出\n", "\nSaved and exited\n"));
#else
        printf(L8("\n退出\n", "\nExit\n"));
#endif
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // 语言：环境变量 FAKEDISK_LANG=zh/en（缺省中文）
    const char* env = getenv("FAKEDISK_LANG");
    if (env && (_stricmp(env, "en") == 0 || _stricmp(env, "english") == 0)) g_lang = Lang::En;
    else if (env && (_stricmp(env, "zh") == 0 || _stricmp(env, "zh-cn") == 0 || _stricmp(env, "cn") == 0)) g_lang = Lang::Zh;
    else g_lang = detect_system_lang();
#ifdef FDISK_READONLY
    printf(L8("fakedisk-dos v1.0 —— 单文件虚拟磁盘命令行工具（只读）\n",
              "fakedisk-dos v1.0 - Single-file virtual disk CLI tool (read-only)\n"));
    printf(L8("修改操作请使用 fakedisk-dos-manager.exe。\n\n",
              "Use fakedisk-dos-manager.exe for modifying operations.\n\n"));
#else
    printf(L8("fakedisk-dos-manager v1.0 —— 单文件虚拟磁盘命令行工具（读写）\n",
              "fakedisk-dos-manager v1.0 - Single-file virtual disk CLI tool (read/write)\n"));
    printf(L8("输入 help 查看命令；exit 退出。\n\n",
              "Type help for commands; exit to quit.\n\n"));
#endif

    if (argc >= 2) {
        std::string err;
        if (!g_disk.open(argv[1], err)) {
            printf(L8("打开 %S 失败：%s\n", "Failed to open %S: %s\n"), argv[1], err.c_str());
#ifndef FDISK_READONLY
            printf(L8("如需新建，请使用: format <文件> [容量]\n",
                      "To create a new one, use: format <file> [capacity]\n"));
#else
            printf(L8("新建磁盘请使用 fakedisk-dos-manager.exe。\n",
                      "Use fakedisk-dos-manager.exe to create a new disk.\n"));
#endif
        } else {
            g_disk_path = argv[1];
            g_cwd.clear();
            printf(L8("已打开：%S（卷标 %s，已用 %s / %s）\n\n",
                      "Opened: %S (label %s, used %s / %s)\n\n"), argv[1],
                   g_disk.label().empty() ? L8("(无)", "(none)") : g_disk.label().c_str(),
                   fmt_size(g_disk.used_bytes()).c_str(), fmt_size(g_disk.capacity()).c_str());
        }
    }
    return run_shell();
}
