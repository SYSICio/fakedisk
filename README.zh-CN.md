# fakedisk — 单文件虚拟磁盘

一个**单文件容器**（不是可挂载的磁盘镜像，但用起来很像一块硬盘），配套 **dos / windows / manager 共 4 个工具**，
支持外部文件导入、内部目录管理、内存读取数据、提取运行程序。

## 快速开始

```
bin\fakedisk-dos-manager.exe          # 读写命令行工具
FDISK:\> format demo.fdk 64M /v:MYDISK
demo:\> copy C:\Users\me\photo.png /pics\photo.png
demo:\> dir /pics
demo:\> exit

bin\fakedisk-windows-manager.exe demo.fdk   # 读写图形界面（或 文件→打开磁盘）
bin\fakedisk-windows.exe demo.fdk           # 只读图形界面
bin\fakedisk-dos.exe demo.fdk               # 只读命令行
```

## 四个工具（读取工具与读写工具分离）

| 工具 | 形态 | 角色 | 能力 |
|---|---|---|---|
| `fakedisk-dos.exe` | 命令行 shell | **读取**（dos） | open/dir/cd/type/export/vol/info/fsck/run，**不写盘** |
| `fakedisk-dos-manager.exe` | 命令行 shell | **读写**（dos-manager） | 在 dos 基础上 + format/copy/xcopy/mkdir/rmdir/del/attr/ren/move/touch/vol/defrag，每次修改即保存 |
| `fakedisk-windows.exe` | Win32 图形界面 | **读取**（windows） | 树/列表浏览、文本/图片内存预览、导出、fsck、程序提取运行；修改类按钮可点击但提示"你无权进行此操作"、不接收拖放、**不写盘** |
| `fakedisk-windows-manager.exe` | Win32 图形界面 | **读写**（windows-manager） | windows 全部能力 + 新建磁盘/导入(文件·文件夹·拖放)/新建/删除/重命名/属性/整理/保存 |

> 读取工具打开磁盘后完全不触碰磁盘文件（关闭前后文件 MD5 与修改时间均不变）；
> 读写工具每次修改后立即原子写回。

## dos 命令参考（dos-manager 全量；只读 dos 剔除带 ▲ 项）

```
format <文件> [容量(K/M/G/T)] [/v:卷标] ▲  新建磁盘并挂载
open <文件> / close                        打开 / 关闭
dir [路径] / cd <路径> / mkdir ▲ / rmdir ▲  目录操作
copy <外部文件> <内部路径> ▲                导入单个文件（自动压缩）
xcopy <外部目录> <内部目录> ▲               递归导入整个目录
export <内部路径> <外部文件>                导出
type <路径>                                内存中读取并显示文本
attr <路径> [+R|-R][+H|-H][+S|-S][+A|-A] ▲ 修改属性
ren ▲ / move ▲ / touch ▲ / vol [新卷标] ▲   重命名、移动、更新时间、卷标
run <路径>                                 提取到 %TEMP% 后运行/打开
info / fsck / defrag ▲ / cls / help / exit
```

路径使用 `/` 或 `\`；支持相对路径与 `..`；含空格路径请加双引号。

## GUI 说明

- 左侧目录树 + 右侧文件列表（名称/大小/类型/属性/修改时间）+ 状态栏
- **windows-manager**：文件（打开/新建/导入/导出）、编辑（新建文件夹/重命名/删除/属性）、工具（fsck/整理/运行）、**视图（桌面视图/列表视图）**、**语言（中文/English）**，支持拖放导入
- **windows（只读）**：同一界面，修改类按钮可点击，但点击后提示"你无权进行此操作"；仅浏览/预览/导出/校验/运行
- **桌面视图**：右侧文件区切换为仿 Windows 桌面的大图标网格（48px 系统图标、自动排列），状态栏标注当前视图，双击进目录/预览与列表视图一致
- 双击：文本 → 内置查看器；图片 → GDI+ 预览；程序/其他 → 提取到 `%TEMP%\fakedisk_extract\` 后打开
- 数据文件一律**内存读取**，仅程序执行时才落盘到临时目录

## 多语言（中文 / English）

- **GUI**：菜单栏新增 **语言(L)** → 中文 / English，切换即时生效（菜单、列表列头、状态栏、对话框、提示框、关于框、标题栏全部联动）；选择持久化到注册表 `HKCU\Software\fakedisk\Lang`，重启保留；首次启动无记录时跟随系统语言（中文系统 → 中文）
- **dos**：命令行输出由环境变量 `FAKEDISK_LANG` 控制，`en`（或 `english`）→ 英文，`zh`/`cn` → 中文，缺省跟随系统语言。示例：
  ```
  set FAKEDISK_LANG=en
  fakedisk-dos-manager.exe test.fdk
  ```
- 语言实现：`src\lang.h` 的 `L()`/`L8()` 双语选择；GUI 菜单文本运行时动态重设（DrawMenuBar 强制重绘），对话框静态文本在初始化时按语言设置

## 容器格式

完整规范见 [FORMAT.md](FORMAT.md)：文件头(96B) + 树形目录表 + 数据区，UTF-8 文件名、
四属性位（R/H/S/A）、时间戳、LZNT1 压缩、CRC-32 校验。

## 重新编译

需要 Visual Studio（含 MSVC 工具集）。修改 `src\build.bat` 顶部的 vcvars64 路径后：

```
src\build.bat
```

一次编译产出 4 个 exe 到 `bin\`（dos/dos-manager 差异由宏 `FDISK_READONLY` 控制）。

## 目录结构

```
fakedisk\
├── bin\                编译产物（4 个 exe）
├── src\                源码（fakedisk.h/.cpp 核心引擎、dos、win、dialogs、资源、build.bat、make_icon.py）
├── test\               测试资产与回归脚本（cmds、inspect_fdk.py、gui_sendinput.py、示例 test.fdk）
├── FORMAT.md           容器格式规范
└── README.md
```

## 已通过测试

- 创建/打开/保存（原子写盘）、目录树（多级、中文名）、卷标
- 导入（自动 LZNT1 压缩，未压缩前 352.6KB → 存储 241.5KB）、导出字节一致
- 属性/重命名/移动/时间戳、xcopy 递归导入
- fsck 全量校验（目录表 CRC、偏移重叠、逐文件内容 CRC）
- run 提取运行（批处理脚本成功执行并产出标记文件）
- **只读工具不写盘**（dos 与 windows 关闭前后磁盘文件 MD5/修改时间均不变；dos 的修改命令被拦截）
- **只读 GUI 修改按钮可点击但提示无权**（Alt+F+N 弹"你无权进行此操作"，不进入任何修改流程；windows-manager 同操作正常弹新建对话框）
- **读写工具功能完好**（dos-manager 全命令通过；windows-manager 新建/导入菜单可正常弹出）
- GUI：渲染、双击进目录、文本查看器、GDI+ 图片查看器
- **桌面视图**：视图菜单切换大图标网格；桌面视图下双击进目录正常（assets 3→2 项）；切回列表视图列头/竖排恢复
- **多语言**：dos 中/英两轮输出正常（`FAKEDISK_LANG=en` 后帮助/操作/拦截全英文）；GUI 中文启动 → 语言菜单切英文（菜单 File/Edit/Tools/View/Language/Help、列头 Name/Size/Type/Attr/Modified、状态栏 `[List view] 3 items …`、标题、新建对话框 New fakedisk Disk/Disk file/Browse/OK/Cancel、只读无权提示全部英文）→ 重启后仍英文（注册表持久化）→ 可即时切回中文；仅读/读写两版一致
