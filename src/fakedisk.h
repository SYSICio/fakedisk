#pragma once
// ============================================================================
// fakedisk 核心格式引擎 v1.0
// 单文件虚拟磁盘容器：文件头 + 树形目录表 + 数据区
// 纯 C++17，无第三方依赖（压缩使用 Windows 内置 LZNT1，动态加载 ntdll）
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>
#include <cwchar>
#include <functional>

namespace fdisk {

// ---- 格式常量 --------------------------------------------------------------
constexpr uint64_t HEADER_SIZE    = 96;
constexpr uint32_t FORMAT_VERSION = 1;
constexpr uint8_t  TYPE_FILE      = 0;
constexpr uint8_t  TYPE_DIR       = 1;

// 属性位
constexpr uint8_t ATTR_READONLY = 0x01;
constexpr uint8_t ATTR_HIDDEN   = 0x02;
constexpr uint8_t ATTR_SYSTEM   = 0x04;
constexpr uint8_t ATTR_ARCHIVE  = 0x08;

constexpr uint32_t MAX_NAME_BYTES = 255; // 单条目名称上限（UTF-8 字节数）

// ---- 目录树节点 -------------------------------------------------------------
struct Entry {
    uint8_t  type  = TYPE_FILE;
    uint8_t  attr  = ATTR_ARCHIVE;   // 文件默认存档位
    std::string name;                // UTF-8，不含分隔符
    int64_t  created  = 0;           // Unix 秒
    int64_t  modified = 0;
    uint64_t size       = 0;         // 解压后大小
    uint64_t data_off   = 0;         // 数据区偏移
    uint32_t stored_size = 0;        // 存储字节数
    bool     compressed = false;
    uint32_t crc        = 0;         // 解压后内容 CRC-32
    std::vector<Entry> children;

    bool is_dir() const { return type == TYPE_DIR; }
    Entry* find_child(const std::string& n);
    const Entry* find_child(const std::string& n) const;
};

// ---- 磁盘对象 ---------------------------------------------------------------
class Disk {
public:
    Disk() = default;
    ~Disk() = default;
    Disk(const Disk&) = delete;
    Disk& operator=(const Disk&) = delete;

    // ---- 打开 / 创建 / 保存 ----
    // 创建新容器（容量支持 10M / 1G / 512K 等后缀，纯数字视为字节）
    static bool create(const std::wstring& file, const std::string& cap_str,
                       const std::string& label, std::string& err);
    // 打开已有容器（全量读入内存）
    bool open(const std::wstring& file, std::string& err);
    // 将内存中的目录树 + 数据区整体写回文件（自动重排数据区使其紧凑）
    bool save(std::string& err);

    // ---- 只读信息 ----
    const std::wstring& path() const { return path_; }
    const std::string&  label() const { return label_; }
    uint64_t capacity() const { return capacity_; }
    uint64_t used_bytes() const { return used_; }          // 全部文件解压后大小
    uint64_t stored_bytes() const { return stored_; }      // 数据区实际占用
    uint64_t free_bytes() const { return capacity_ > used_ ? capacity_ - used_ : 0; }
    Entry&   root() { return root_; }
    const Entry& root() const { return root_; }
    bool     has_compressed() const { return has_compressed_; }

    // ---- 路径解析 ----
    // 统一内部路径：/ 或 \ 分隔；"" 或 "/" 表示根；不支持 ..（越界即失败）
    Entry* resolve(const std::string& path);
    const Entry* resolve(const std::string& path) const;

    // ---- 修改操作（均在内存中执行，需再调用 save 落盘）----
    bool mkdir(const std::string& path, std::string& err);
    bool remove(const std::string& path, std::string& err);          // 文件或目录（递归）
    bool rename(const std::string& path, const std::string& new_name, std::string& err);
    bool move(const std::string& src_path, const std::string& dst_path, std::string& err);
    bool set_attr(const std::string& path, uint8_t add_bits, uint8_t del_bits, std::string& err);
    bool set_time(const std::string& path, int64_t created, int64_t modified, std::string& err);
    bool set_label(const std::string& label);                          // 卷标

    // ---- 文件内容 ----
    // 把外部文件导入为 dest_path（覆盖同名）；自动尝试压缩
    bool import_file(const std::string& dest_path, const std::wstring& ext_file, std::string& err);
    // 把内部文件导出到外部路径
    bool export_file(const std::string& int_path, const std::wstring& ext_file, std::string& err);
    // 读取内部文件内容（解压后完整字节，内存读取）
    bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& err);
    // 把一段字节写入内部文件（覆盖同名）
    bool write_bytes(const std::string& dest_path, const std::vector<uint8_t>& data,
                     const std::string& name_hint, std::string& err);

    // ---- 维护 ----
    // fsck：全量校验（头部/目录表 CRC、偏移越界/重叠、逐文件内容 CRC）
    bool verify(std::string& err, std::vector<std::string>* problems = nullptr);
    // 整理：重建数据区（save 内部已做；此接口仅作显式入口）
    bool defrag(std::string& err) { return save(err); }

private:
    bool parse_header(const uint8_t* p, size_t n, std::string& err);
    bool parse_toc(const uint8_t* p, size_t n, std::string& err);
    bool serialize_toc(std::vector<uint8_t>& out) const;
    void rebuild_data(std::vector<uint8_t>& new_data);
    bool write_file_atomic(std::string& err);
    bool add_file_bytes(Entry* parent, const std::string& name,
                        const uint8_t* data, size_t n, std::string& err);
    bool read_file_internal(const Entry* e, std::vector<uint8_t>& out, std::string& err);
    uint64_t compute_used() const;   // 遍历统计 used_/stored_

    std::wstring path_;
    std::string  label_;
    uint64_t capacity_ = 0;
    uint64_t used_ = 0, stored_ = 0;
    bool     has_compressed_ = false;
    Entry    root_;
    std::vector<uint8_t> data_;      // 数据区（存储格式，可能含压缩内容）
    // 打开时记录的布局
    uint64_t toc_offset_ = 0, toc_size_ = 0;
    uint64_t data_offset_ = 0, data_size_ = 0;
    uint32_t toc_crc_ = 0;
};

// ---- 工具函数 ----
uint32_t crc32(const uint8_t* p, size_t n);
int64_t  now_unix();
// 外部文件读写（Windows）
bool read_external_file(const std::wstring& path, std::vector<uint8_t>& out, std::string& err);
bool write_external_file(const std::wstring& path, const uint8_t* p, size_t n, std::string& err);
// LZNT1 压缩/解压（失败返回 false，调用方据此降级为不压缩 / 报错）
bool lznt1_compress(const uint8_t* in, size_t n, std::vector<uint8_t>& out);
bool lznt1_decompress(const uint8_t* in, size_t n, size_t expect_out, std::vector<uint8_t>& out);

} // namespace fdisk
