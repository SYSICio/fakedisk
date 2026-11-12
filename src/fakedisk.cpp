// ============================================================================
// fakedisk 核心格式引擎 v1.0 —— 实现
// ============================================================================
#include "fakedisk.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <set>

namespace fdisk {

// ---------------------------------------------------------------------------
// 小工具：字节序写入/读取（小端）
// ---------------------------------------------------------------------------
static void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x & 0xFF)); v.push_back(uint8_t((x >> 8) & 0xFF));
}
static void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x & 0xFF)); v.push_back(uint8_t((x >> 8) & 0xFF));
    v.push_back(uint8_t((x >> 16) & 0xFF)); v.push_back(uint8_t((x >> 24) & 0xFF));
}
static void put_u64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xFF));
}
static void put_i64(std::vector<uint8_t>& v, int64_t x) { put_u64(v, uint64_t(x)); }

// 指针版本（写入固定布局缓冲区）
static void put_u16(uint8_t* p, uint16_t x) { p[0] = uint8_t(x & 0xFF); p[1] = uint8_t((x >> 8) & 0xFF); }
static void put_u32(uint8_t* p, uint32_t x) {
    p[0] = uint8_t(x & 0xFF); p[1] = uint8_t((x >> 8) & 0xFF);
    p[2] = uint8_t((x >> 16) & 0xFF); p[3] = uint8_t((x >> 24) & 0xFF);
}
static void put_u64(uint8_t* p, uint64_t x) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t((x >> (8 * i)) & 0xFF);
}

static uint16_t get_u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
static uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static uint64_t get_u64(const uint8_t* p) {
    uint64_t x = 0;
    for (int i = 7; i >= 0; --i) x = (x << 8) | p[i];
    return x;
}
static int64_t get_i64(const uint8_t* p) { return int64_t(get_u64(p)); }

// ---------------------------------------------------------------------------
// CRC-32（多项式 0xEDB88320，zlib 兼容）
// ---------------------------------------------------------------------------
uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

int64_t now_unix() { return (int64_t)time(nullptr); }

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16
// ---------------------------------------------------------------------------
static std::string utf16_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring utf8_to_utf16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------------------------------------------------------
// 外部文件读写
// ---------------------------------------------------------------------------
static std::string win_err_string(DWORD e = GetLastError()) {
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, buf, 511, nullptr);
    std::wstring ws = buf;
    while (!ws.empty() && (ws.back() == L'\r' || ws.back() == L'\n' || ws.back() == L' ')) ws.pop_back();
    return utf16_to_utf8(ws);
}

bool read_external_file(const std::wstring& path, std::vector<uint8_t>& out, std::string& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "无法读取外部文件: " + win_err_string(); return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || (uint64_t)sz.QuadPart > (uint64_t)0x7FFFFFFF) {
        err = "文件过大或读取失败"; CloseHandle(h); return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0, total = 0;
    while (total < out.size()) {
        if (!ReadFile(h, out.data() + total, (DWORD)(out.size() - total), &rd, nullptr) || rd == 0) {
            err = "读取文件失败"; CloseHandle(h); return false;
        }
        total += rd;
    }
    CloseHandle(h);
    return true;
}

bool write_external_file(const std::wstring& path, const uint8_t* p, size_t n, std::string& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "无法写入外部文件: " + win_err_string(); return false; }
    DWORD wr = 0, total = 0;
    while (total < n) {
        if (!WriteFile(h, p + total, (DWORD)(n - total), &wr, nullptr) || wr == 0) {
            err = "写入文件失败"; CloseHandle(h); return false;
        }
        total += wr;
    }
    CloseHandle(h);
    return true;
}

// ---------------------------------------------------------------------------
// LZNT1 压缩（Windows 内置，动态加载 ntdll）
// ---------------------------------------------------------------------------
typedef LONG NTSTATUS;
#define NT_SUCCESS(st) (((NTSTATUS)(st)) >= 0)
#ifndef COMPRESSION_FORMAT_LZNT1
#define COMPRESSION_FORMAT_LZNT1 0x0002
#endif
typedef NTSTATUS(NTAPI* fn_RtlGetCompressionWorkSpaceSize)(USHORT, PULONG, PULONG);
typedef NTSTATUS(NTAPI* fn_RtlCompressBuffer)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG, PULONG, PVOID);
typedef NTSTATUS(NTAPI* fn_RtlDecompressBuffer)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG);

static void lznt1_load(fn_RtlGetCompressionWorkSpaceSize& a, fn_RtlCompressBuffer& b, fn_RtlDecompressBuffer& c) {
    static HMODULE h = GetModuleHandleW(L"ntdll.dll");
    a = h ? (fn_RtlGetCompressionWorkSpaceSize)GetProcAddress(h, "RtlGetCompressionWorkSpaceSize") : nullptr;
    b = h ? (fn_RtlCompressBuffer)GetProcAddress(h, "RtlCompressBuffer") : nullptr;
    c = h ? (fn_RtlDecompressBuffer)GetProcAddress(h, "RtlDecompressBuffer") : nullptr;
}

bool lznt1_compress(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
    fn_RtlGetCompressionWorkSpaceSize fws; fn_RtlCompressBuffer fc; fn_RtlDecompressBuffer fd;
    lznt1_load(fws, fc, fd);
    if (!fws || !fc || n == 0 || n > 0x7FFFFFFF) return false;
    ULONG ws_need = 0, ws_frag = 0;
    if (fws(COMPRESSION_FORMAT_LZNT1, &ws_need, &ws_frag) != 0) return false;
    std::vector<uint8_t> ws(ws_need);
    // LZNT1 最坏情况上界
    size_t cap = n + (n >> 12) * 16 + 64;
    std::vector<uint8_t> buf(cap);
    ULONG final = 0;
    NTSTATUS st = fc(COMPRESSION_FORMAT_LZNT1, (PUCHAR)in, (ULONG)n,
                     buf.data(), (ULONG)buf.size(), 4096, &final, ws.data());
    if (!NT_SUCCESS(st) || final == 0 || final >= n) return false;
    out.assign(buf.begin(), buf.begin() + final);
    return true;
}

bool lznt1_decompress(const uint8_t* in, size_t n, size_t expect_out, std::vector<uint8_t>& out) {
    fn_RtlGetCompressionWorkSpaceSize fws; fn_RtlCompressBuffer fc; fn_RtlDecompressBuffer fd;
    lznt1_load(fws, fc, fd);
    if (!fd || expect_out == 0 || expect_out > 0x7FFFFFFF) return false;
    out.resize(expect_out);
    ULONG final = 0;
    NTSTATUS st = fd(COMPRESSION_FORMAT_LZNT1, out.data(), (ULONG)expect_out,
                     (PUCHAR)in, (ULONG)n, &final);
    if (!NT_SUCCESS(st)) return false;
    out.resize(final);
    return true;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
Entry* Entry::find_child(const std::string& n) {
    for (auto& c : children) if (c.name == n) return &c;
    return nullptr;
}
const Entry* Entry::find_child(const std::string& n) const {
    for (auto& c : children) if (c.name == n) return &c;
    return nullptr;
}

// ---------------------------------------------------------------------------
// 路径工具
// ---------------------------------------------------------------------------
// 规范化：统一为 / 分隔、去前导 /、去尾 /。返回 "" 表示根。
static std::string norm_path(const std::string& path) {
    std::string p;
    p.reserve(path.size());
    for (char ch : path) p.push_back(ch == '\\' ? '/' : ch);
    size_t a = 0, b = p.size();
    while (a < b && p[a] == '/') ++a;
    while (b > a && p[b - 1] == '/') --b;
    p = p.substr(a, b - a);
    return p;
}

// 把 "父路径 / 名称" 拆开。parent 可为 ""（根）。返回 false 表示名称非法。
static bool split_last(const std::string& norm, std::string& parent, std::string& name) {
    if (norm.empty()) { parent.clear(); name.clear(); return false; }
    size_t pos = norm.rfind('/');
    if (pos == std::string::npos) { parent.clear(); name = norm; }
    else { parent = norm.substr(0, pos); name = norm.substr(pos + 1); }
    if (name.empty() || name == "." || name == ".." || name.size() > MAX_NAME_BYTES ||
        name.find_first_of("/\\") != std::string::npos)
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// 序列化/反序列化 TOC
// ---------------------------------------------------------------------------
static void serialize_node(std::vector<uint8_t>& out, const Entry& e) {
    out.push_back(e.type);
    out.push_back(e.attr);
    put_u16(out, (uint16_t)e.name.size());
    out.insert(out.end(), e.name.begin(), e.name.end());
    put_i64(out, e.created);
    put_i64(out, e.modified);
    put_u64(out, e.size);
    put_u64(out, e.data_off);
    put_u32(out, e.stored_size);
    out.push_back(e.compressed ? 1 : 0);
    put_u32(out, e.crc);
    put_u32(out, e.is_dir() ? (uint32_t)e.children.size() : 0);
    if (e.is_dir()) {
        // 目录在前、文件在后
        for (auto& c : e.children) if (c.is_dir()) serialize_node(out, c);
        for (auto& c : e.children) if (!c.is_dir()) serialize_node(out, c);
    }
}

bool Disk::serialize_toc(std::vector<uint8_t>& out) const {
    out.clear();
    serialize_node(out, root_);
    return true;
}

// 返回消费的字节数；失败返回 0
static size_t parse_node(const uint8_t* p, size_t remain, Entry& e) {
    if (remain < 1 + 1 + 2 + 8 + 8 + 8 + 8 + 4 + 1 + 4 + 4) return 0;
    size_t o = 0;
    e.type = p[o++];
    e.attr = p[o++];
    uint16_t nl = get_u16(p + o); o += 2;
    if (nl > MAX_NAME_BYTES || o + nl > remain) return 0;
    e.name.assign((const char*)p + o, nl); o += nl;
    e.created = get_i64(p + o); o += 8;
    e.modified = get_i64(p + o); o += 8;
    e.size = get_u64(p + o); o += 8;
    e.data_off = get_u64(p + o); o += 8;
    e.stored_size = get_u32(p + o); o += 4;
    e.compressed = p[o++] != 0;
    e.crc = get_u32(p + o); o += 4;
    uint32_t cc = get_u32(p + o); o += 4;
    if (e.is_dir()) {
        if (cc > 0xFFFFFF) return 0;
        e.children.reserve(cc);
        for (uint32_t i = 0; i < cc; ++i) {
            Entry child;
            size_t used = parse_node(p + o, remain - o, child);
            if (used == 0) return 0;
            o += used;
            e.children.push_back(std::move(child));
        }
    }
    return o;
}

bool Disk::parse_toc(const uint8_t* p, size_t n, std::string& err) {
    root_.children.clear();
    size_t used = parse_node(p, n, root_);
    if (used == 0 || used != n) { err = "目录表损坏（解析失败）"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------
bool Disk::parse_header(const uint8_t* p, size_t n, std::string& err) {
    if (n < HEADER_SIZE) { err = "文件过小，不是有效的 fakedisk"; return false; }
    if (memcmp(p, "FDISK01\0", 8) != 0) { err = "魔数不匹配，不是 fakedisk 文件"; return false; }
    uint32_t hcrc = get_u32(p + 88);
    if (crc32(p + 8, 80) != hcrc) { err = "文件头校验失败（可能已损坏）"; return false; }
    uint32_t ver = get_u16(p + 8);
    if (ver != FORMAT_VERSION) { err = "不支持的格式版本"; return false; }
    // flags: bit0 = 含压缩条目（仅信息）
    label_.assign((const char*)p + 12, strnlen((const char*)p + 12, 32));
    capacity_ = get_u64(p + 44);
    toc_offset_ = get_u64(p + 52);
    toc_size_ = get_u64(p + 60);
    data_offset_ = get_u64(p + 68);
    data_size_ = get_u64(p + 76);
    toc_crc_ = get_u32(p + 84);
    if (toc_offset_ != HEADER_SIZE) { err = "目录表偏移异常"; return false; }
    if (toc_offset_ + toc_size_ > n || data_offset_ + data_size_ > n) { err = "文件布局越界"; return false; }
    if (data_offset_ != toc_offset_ + toc_size_) { err = "数据区偏移异常"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// 打开 / 创建 / 保存
// ---------------------------------------------------------------------------
bool Disk::open(const std::wstring& file, std::string& err) {
    std::vector<uint8_t> img;
    if (!read_external_file(file, img, err)) return false;
    if (!parse_header(img.data(), img.size(), err)) return false;
    std::vector<uint8_t> toc(img.begin() + (ptrdiff_t)toc_offset_,
                             img.begin() + (ptrdiff_t)(toc_offset_ + toc_size_));
    if (crc32(toc.data(), toc.size()) != toc_crc_) { err = "目录表校验失败（可能已损坏）"; return false; }
    if (!parse_toc(toc.data(), toc.size(), err)) return false;
    data_.assign(img.begin() + (ptrdiff_t)data_offset_,
                 img.begin() + (ptrdiff_t)(data_offset_ + data_size_));
    path_ = file;
    used_ = compute_used();
    stored_ = data_.size();
    return true;
}

uint64_t Disk::compute_used() const {
    uint64_t sum = 0;
    std::function<void(const Entry&)> walk = [&](const Entry& e) {
        if (e.is_dir()) { for (auto& c : e.children) walk(c); }
        else sum += e.size;
    };
    walk(root_);
    return sum;
}

static uint64_t parse_capacity(const std::string& s) {
    if (s.empty()) return 64ULL * 1024 * 1024;
    std::string t = s;
    for (auto& c : t) c = (char)toupper((unsigned char)c);
    uint64_t mult = 1;
    char last = t.empty() ? 0 : t.back();
    if (last == 'K') { mult = 1024ULL; t.pop_back(); }
    else if (last == 'M') { mult = 1024ULL * 1024; t.pop_back(); }
    else if (last == 'G') { mult = 1024ULL * 1024 * 1024; t.pop_back(); }
    else if (last == 'T') { mult = 1024ULL * 1024 * 1024 * 1024; t.pop_back(); }
    char* end = nullptr;
    unsigned long long v = strtoull(t.c_str(), &end, 10);
    if (end == t.c_str() || v == 0) return 64ULL * 1024 * 1024;
    uint64_t cap = v * mult;
    if (cap < 1024) cap = 1024; // 最小 1K
    return cap;
}

bool Disk::create(const std::wstring& file, const std::string& cap_str,
                  const std::string& label, std::string& err) {
    uint64_t cap = parse_capacity(cap_str);
    std::string lab = label;
    if (lab.size() > 31) lab = lab.substr(0, 31);
    if (lab.find_first_of("/\\") != std::string::npos) lab.clear(); // 卷标不允许分隔符

    Entry root;
    root.type = TYPE_DIR;
    root.attr = 0;
    root.created = root.modified = now_unix();

    std::vector<uint8_t> toc;
    serialize_node(toc, root);
    uint32_t toc_crc = crc32(toc.data(), toc.size());

    std::vector<uint8_t> img(HEADER_SIZE + toc.size(), 0);
    memcpy(img.data(), "FDISK01\0", 8);
    put_u16(img.data() + 8, FORMAT_VERSION);
    put_u16(img.data() + 10, 0);
    if (!lab.empty()) memcpy(img.data() + 12, lab.data(), lab.size());
    put_u64(img.data() + 44, cap);
    put_u64(img.data() + 52, HEADER_SIZE);
    put_u64(img.data() + 60, toc.size());
    put_u64(img.data() + 68, HEADER_SIZE + toc.size());
    put_u64(img.data() + 76, 0);
    put_u32(img.data() + 84, toc_crc);
    put_u32(img.data() + 92, 0);
    uint32_t hcrc = crc32(img.data() + 8, 80);
    put_u32(img.data() + 88, hcrc);
    if (!toc.empty()) memcpy(img.data() + HEADER_SIZE, toc.data(), toc.size());
    return write_external_file(file, img.data(), img.size(), err);
}

void Disk::rebuild_data(std::vector<uint8_t>& new_data) {
    has_compressed_ = false;
    std::function<void(Entry&)> walk = [&](Entry& e) {
        if (e.is_dir()) {
            for (auto& c : e.children) walk(c);
        } else {
            if (e.stored_size > 0 && e.data_off + e.stored_size <= data_.size()) {
                uint64_t old_off = e.data_off;
                e.data_off = new_data.size();
                new_data.insert(new_data.end(),
                                data_.begin() + (ptrdiff_t)old_off,
                                data_.begin() + (ptrdiff_t)(old_off + e.stored_size));
            } else {
                e.data_off = 0;
                e.stored_size = 0;
                e.size = 0;
                e.compressed = false;
                e.crc = 0;
            }
            if (e.compressed) has_compressed_ = true;
        }
    };
    walk(root_);
}

bool Disk::save(std::string& err) {
    std::vector<uint8_t> new_data;
    rebuild_data(new_data);

    std::vector<uint8_t> toc;
    serialize_toc(toc);
    uint32_t toc_crc = crc32(toc.data(), toc.size());

    std::vector<uint8_t> img(HEADER_SIZE + toc.size() + new_data.size(), 0);
    memcpy(img.data(), "FDISK01\0", 8);
    put_u16(img.data() + 8, FORMAT_VERSION);
    put_u16(img.data() + 10, has_compressed_ ? 1 : 0);
    if (!label_.empty()) memcpy(img.data() + 12, label_.data(), (std::min)((size_t)31, label_.size()));
    put_u64(img.data() + 44, capacity_);
    put_u64(img.data() + 52, HEADER_SIZE);
    put_u64(img.data() + 60, toc.size());
    put_u64(img.data() + 68, HEADER_SIZE + toc.size());
    put_u64(img.data() + 76, new_data.size());
    put_u32(img.data() + 84, toc_crc);
    put_u32(img.data() + 92, 0);
    uint32_t hcrc = crc32(img.data() + 8, 80);
    put_u32(img.data() + 88, hcrc);
    if (!toc.empty()) memcpy(img.data() + HEADER_SIZE, toc.data(), toc.size());
    if (!new_data.empty())
        memcpy(img.data() + HEADER_SIZE + toc.size(), new_data.data(), new_data.size());

    // 原子写：先写临时文件，再替换
    std::wstring tmp = path_ + L".tmp";
    if (!write_external_file(tmp, img.data(), img.size(), err)) return false;
    if (!MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = "替换文件失败: " + win_err_string();
        return false;
    }
    data_.swap(new_data);
    stored_ = data_.size();
    used_ = compute_used();
    return true;
}

// ---------------------------------------------------------------------------
// 路径解析
// ---------------------------------------------------------------------------
Entry* Disk::resolve(const std::string& path) {
    std::string p = norm_path(path);
    if (p.empty()) return &root_;
    Entry* cur = &root_;
    size_t start = 0;
    while (start <= p.size()) {
        size_t pos = p.find('/', start);
        std::string part = p.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        if (!part.empty()) {
            if (part == "..") return nullptr; // 不支持越界
            if (!cur->is_dir()) return nullptr;
            cur = cur->find_child(part);
            if (!cur) return nullptr;
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return cur;
}

const Entry* Disk::resolve(const std::string& path) const {
    return const_cast<Disk*>(this)->resolve(path);
}

// ---------------------------------------------------------------------------
// 修改操作
// ---------------------------------------------------------------------------
bool Disk::mkdir(const std::string& path, std::string& err) {
    std::string norm = norm_path(path);
    std::string parent, name;
    if (!split_last(norm, parent, name)) { err = "无效的目录名"; return false; }
    Entry* p = resolve(parent);
    if (!p || !p->is_dir()) { err = "父目录不存在"; return false; }
    if (p->find_child(name)) { err = "已存在同名条目"; return false; }
    Entry e;
    e.type = TYPE_DIR;
    e.attr = 0;
    e.name = name;
    e.created = e.modified = now_unix();
    p->children.push_back(std::move(e));
    return true;
}

bool Disk::remove(const std::string& path, std::string& err) {
    std::string norm = norm_path(path);
    std::string parent, name;
    if (!split_last(norm, parent, name)) { err = "无效路径"; return false; }
    Entry* p = resolve(parent);
    if (!p || !p->is_dir()) { err = "父目录不存在"; return false; }
    auto it = std::find_if(p->children.begin(), p->children.end(),
                           [&](Entry& e) { return e.name == name; });
    if (it == p->children.end()) { err = "条目不存在"; return false; }
    p->children.erase(it);
    return true;
}

bool Disk::rename(const std::string& path, const std::string& new_name, std::string& err) {
    if (new_name.empty() || new_name == "." || new_name == ".." ||
        new_name.size() > MAX_NAME_BYTES || new_name.find_first_of("/\\") != std::string::npos) {
        err = "无效的新名称"; return false;
    }
    std::string norm = norm_path(path);
    std::string parent, name;
    if (!split_last(norm, parent, name)) { err = "无效路径"; return false; }
    Entry* p = resolve(parent);
    if (!p || !p->is_dir()) { err = "父目录不存在"; return false; }
    Entry* e = p->find_child(name);
    if (!e) { err = "条目不存在"; return false; }
    if (p->find_child(new_name)) { err = "已存在同名条目"; return false; }
    e->name = new_name;
    return true;
}

bool Disk::move(const std::string& src_path, const std::string& dst_path, std::string& err) {
    std::string ns = norm_path(src_path), nd = norm_path(dst_path);
    if (ns.empty() || nd.empty()) { err = "无效路径"; return false; }
    std::string sp, sn, dp, dn;
    if (!split_last(ns, sp, sn) || !split_last(nd, dp, dn)) { err = "无效路径"; return false; }
    Entry* sp_ = resolve(sp);
    Entry* dp_ = resolve(dp);
    if (!sp_ || !sp_->is_dir() || !dp_ || !dp_->is_dir()) { err = "父目录不存在"; return false; }
    Entry* e = sp_->find_child(sn);
    if (!e) { err = "源条目不存在"; return false; }
    if (dp_->find_child(dn)) { err = "目标位置已存在同名条目"; return false; }
    // 禁止把目录移入自身子树（目标路径不得以源路径为前缀）
    if (e->is_dir()) {
        if (nd.compare(0, ns.size() + 1, ns + "/") == 0) { err = "不能把目录移入自身内部"; return false; }
    }
    // 摘除
    auto it = std::find_if(sp_->children.begin(), sp_->children.end(),
                           [&](Entry& x) { return x.name == sn; });
    Entry moved = std::move(*it);
    sp_->children.erase(it);
    moved.name = dn;
    dp_->children.push_back(std::move(moved));
    return true;
}

bool Disk::set_attr(const std::string& path, uint8_t add_bits, uint8_t del_bits, std::string& err) {
    Entry* e = resolve(path);
    if (!e) { err = "条目不存在"; return false; }
    e->attr = (uint8_t)((e->attr | add_bits) & ~del_bits);
    return true;
}

bool Disk::set_time(const std::string& path, int64_t created, int64_t modified, std::string& err) {
    Entry* e = resolve(path);
    if (!e) { err = "条目不存在"; return false; }
    if (created) e->created = created;
    if (modified) e->modified = modified;
    return true;
}

bool Disk::set_label(const std::string& label) {
    std::string lab = label;
    if (lab.size() > 31) lab = lab.substr(0, 31);
    if (lab.find_first_of("/\\") != std::string::npos) lab.clear();
    label_ = lab;
    return true;
}

// ---------------------------------------------------------------------------
// 文件内容
// ---------------------------------------------------------------------------
bool Disk::read_file(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    Entry* e = resolve(path);
    if (!e) { err = "条目不存在"; return false; }
    if (e->is_dir()) { err = "这是一个目录"; return false; }
    if (e->stored_size == 0) { out.clear(); return true; }
    if (e->data_off + e->stored_size > data_.size()) { err = "数据越界（容器损坏）"; return false; }
    const uint8_t* p = data_.data() + e->data_off;
    if (e->compressed) {
        if (!lznt1_decompress(p, e->stored_size, (size_t)e->size, out)) {
            err = "解压失败（容器损坏？）"; return false;
        }
    } else {
        out.assign(p, p + e->stored_size);
    }
    if (e->crc && crc32(out.data(), out.size()) != e->crc) {
        err = "内容校验失败（容器损坏？）"; return false;
    }
    return true;
}

bool Disk::add_file_bytes(Entry* parent, const std::string& name,
                          const uint8_t* data, size_t n, std::string& err) {
    if (name.empty() || name.size() > MAX_NAME_BYTES ||
        name.find_first_of("/\\") != std::string::npos || name == "." || name == "..") {
        err = "非法文件名"; return false;
    }
    std::vector<uint8_t> stored;
    bool comp = false;
    if (n >= 256 && lznt1_compress(data, n, stored) && stored.size() < n) comp = true;
    else stored.assign(data, data + n);

    Entry* existing = parent->find_child(name);
    if (existing && existing->is_dir()) { err = "同名目录已存在"; return false; }
    if (existing) {
        existing->size = n;
        existing->data_off = data_.size();
        existing->stored_size = (uint32_t)stored.size();
        existing->compressed = comp;
        existing->crc = crc32(data, n);
        existing->modified = now_unix();
    } else {
        Entry e;
        e.type = TYPE_FILE;
        e.attr = ATTR_ARCHIVE;
        e.name = name;
        e.created = e.modified = now_unix();
        e.size = n;
        e.data_off = data_.size();
        e.stored_size = (uint32_t)stored.size();
        e.compressed = comp;
        e.crc = crc32(data, n);
        parent->children.push_back(std::move(e));
    }
    data_.insert(data_.end(), stored.begin(), stored.end());
    if (comp) has_compressed_ = true;
    return true;
}

bool Disk::write_bytes(const std::string& dest_path, const std::vector<uint8_t>& data,
                       const std::string& name_hint, std::string& err) {
    std::string norm = norm_path(dest_path);
    if (norm.empty()) { err = "无效路径"; return false; }
    std::string parent, name;
    bool trailing = !dest_path.empty() && (dest_path.back() == '/' || dest_path.back() == '\\');
    if (trailing) {
        parent = norm;
        name = name_hint;
    } else {
        if (!split_last(norm, parent, name)) { err = "无效的目标路径"; return false; }
    }
    if (name.empty()) name = name_hint;
    Entry* p = resolve(parent);
    if (!p || !p->is_dir()) { err = "目标目录不存在"; return false; }
    return add_file_bytes(p, name, data.data(), data.size(), err);
}

bool Disk::import_file(const std::string& dest_path, const std::wstring& ext_file, std::string& err) {
    std::vector<uint8_t> data;
    if (!read_external_file(ext_file, data, err)) return false;
    std::wstring fn = ext_file;
    size_t pos = fn.find_last_of(L"\\/");
    if (pos != std::wstring::npos) fn = fn.substr(pos + 1);
    std::string hint = utf16_to_utf8(fn);
    return write_bytes(dest_path, data, hint, err);
}

bool Disk::export_file(const std::string& int_path, const std::wstring& ext_file, std::string& err) {
    std::vector<uint8_t> data;
    if (!read_file(int_path, data, err)) return false;
    return write_external_file(ext_file, data.data(), data.size(), err);
}

// ---------------------------------------------------------------------------
// 维护：fsck / 容量
// ---------------------------------------------------------------------------
bool Disk::verify(std::string& err, std::vector<std::string>* problems) {
    bool ok = true;
    auto problem = [&](const std::string& s) {
        ok = false;
        if (problems) problems->push_back(s);
        else if (err.empty()) err = s;
    };
    // 遍历所有文件条目：偏移检查 + 内容 CRC
    std::function<void(const Entry&)> walk = [&](const Entry& e) {
        if (e.is_dir()) { for (auto& c : e.children) walk(c); return; }
        if (e.data_off + e.stored_size > data_.size()) {
            problem("数据越界: /" + e.name);
            return;
        }
        if (e.stored_size > 0) {
            std::vector<uint8_t> out;
            std::string dummy;
            if (!read_file_internal(&e, out, dummy)) {
                problem("读取失败: " + e.name);
                return;
            }
            if (e.crc && crc32(out.data(), out.size()) != e.crc)
                problem("CRC 不匹配: " + e.name);
        }
    };
    walk(root_);
    // 偏移重叠检测
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    std::function<void(const Entry&)> walk2 = [&](const Entry& e) {
        if (e.is_dir()) { for (auto& c : e.children) walk2(c); }
        else if (e.stored_size > 0) ranges.push_back({ e.data_off, e.data_off + e.stored_size });
    };
    walk2(root_);
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].second)
            problem("数据区存在重叠区间");
    }
    return ok;
}

bool Disk::read_file_internal(const Entry* e, std::vector<uint8_t>& out, std::string& err) {
    if (e->stored_size == 0) { out.clear(); return true; }
    const uint8_t* p = data_.data() + e->data_off;
    if (e->compressed) {
        if (!lznt1_decompress(p, e->stored_size, (size_t)e->size, out)) { err = "解压失败"; return false; }
    } else {
        out.assign(p, p + e->stored_size);
    }
    return true;
}

} // namespace fdisk
