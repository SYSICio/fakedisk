#pragma once
// 对话框模块接口（新建磁盘 / 属性 / 输入框）
#include <windows.h>
#include <string>
#include "fakedisk.h"

extern fdisk::Disk g_disk;
extern HWND g_hWnd, g_hTree, g_hList, g_hStatus;

bool open_disk(const std::wstring& file);
void refresh_all();
bool save_disk();

void register_dialogs();
bool input_dialog(const std::wstring& title, const std::wstring& prompt,
                  wchar_t* buf, int maxlen);
void new_disk_dialog();
void prop_dialog();
