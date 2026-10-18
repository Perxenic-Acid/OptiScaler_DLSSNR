#pragma once

#include "menu_localization.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>

namespace MenuL10n
{
inline void SelectLanguage(std::string_view preference)
{
    chineseRequested = preference == "zh-cn" || preference == "zh-CN" ||
                       (preference == "auto" && PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE);
}

inline bool AddChineseFont(ImFontAtlas* atlas, float size)
{
    // 初始化时收集所需字形；新版 ImGui 的动态图集按需载入，不在每帧重新读取字体文件。
    static ImVector<ImWchar> ranges;
    if (ranges.empty())
    {
        ImFontGlyphRangesBuilder builder;
        for (const auto& entry : dictionary)
            builder.AddText(entry.chinese);
        builder.BuildRanges(&ranges);
    }

    wchar_t windows[MAX_PATH] {};
    const auto length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    const auto fonts = std::filesystem::path(windows) / L"Fonts";
    for (const auto* name : { L"msyh.ttc", L"msyh.ttf", L"simhei.ttf", L"simsun.ttc", L"msjh.ttc" })
    {
        std::ifstream file(fonts / name, std::ios::binary | std::ios::ate);
        if (!file)
            continue;
        const auto length = file.tellg();
        if (length <= 0 || length > 64 * 1024 * 1024)
            continue;
        void* bytes = ImGui::MemAlloc(static_cast<size_t>(length));
        if (bytes == nullptr)
            continue;
        file.seekg(0);
        if (!file.read(static_cast<char*>(bytes), length))
        {
            ImGui::MemFree(bytes);
            continue;
        }
        ImFontConfig config;
        config.MergeMode = true;
        config.FontDataOwnedByAtlas = true;
        config.OversampleH = config.OversampleV = 1;
        // AddFontFromMemoryTTF 接管 bytes，图集销毁时统一释放。
        if (atlas->AddFontFromMemoryTTF(bytes, static_cast<int>(length), size, &config, ranges.Data))
            return true;
    }
    return false;
}
}
