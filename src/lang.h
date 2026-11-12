// ============================================================================
// lang.h —— fakedisk 多语言支持（中文 / English）
// 二元语言选择：g_lang 全局语言；L() 宽字符串、L8() 窄字符串(UTF-8)
// ============================================================================
#pragma once

enum class Lang { Zh = 0, En = 1 };
extern Lang g_lang;

inline const wchar_t* L(const wchar_t* zh, const wchar_t* en) {
    return g_lang == Lang::En ? en : zh;
}
inline const char* L8(const char* zh, const char* en) {
    return g_lang == Lang::En ? en : zh;
}

// 系统语言检测（中文系统 -> 中文，否则 English）
inline Lang detect_system_lang() {
    LANGID lid = GetUserDefaultUILanguage();
    WORD primary = PRIMARYLANGID(lid);
    if (primary == LANG_CHINESE) return Lang::Zh;
    return Lang::En;
}
