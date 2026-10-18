#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace MenuL10n
{
struct Entry
{
    std::string_view english;
    const char* chinese;
};

inline constexpr Entry dictionary[] = {
#include "translations_zh_cn.inc"
};

inline bool chineseRequested = false;
inline bool chineseFontAvailable = false;

inline const Entry* Find(std::string_view english)
{
    // 初始化后仅做二分查找；面板绘制不扫描文件或分配翻译缓存。
    static const auto index = [] {
        std::array<const Entry*, std::size(dictionary)> result {};
        for (size_t i = 0; i < result.size(); ++i)
            result[i] = &dictionary[i];
        std::sort(result.begin(), result.end(), [](auto a, auto b) { return a->english < b->english; });
        return result;
    }();
    auto it = std::lower_bound(index.begin(), index.end(), english,
                               [](auto entry, auto value) { return entry->english < value; });
    return it != index.end() && (*it)->english == english ? *it : nullptr;
}

inline bool IsChinese() { return chineseRequested && chineseFontAvailable; }

inline const char* Text(const char* english)
{
    if (english != nullptr && IsChinese())
    {
        const auto* entry = Find(english);
        if (entry == nullptr)
        {
            const std::string_view value(english);
            const auto hidden = value.find("##");
            if (hidden != std::string_view::npos)
                entry = Find(value.substr(0, hidden));
        }
        if (entry != nullptr)
            return entry->chinese;
    }
    return english;
}

inline const char* Label(const char* english)
{
    if (english == nullptr || english[0] == '#' || english[0] == '\0')
        return english;
    const std::string_view original(english);
    const auto hidden = original.find("##");
    const auto visible = original.substr(0, hidden);
    const auto* entry = Find(visible);
    if (entry == nullptr)
        return english;

    // 中英文共用同一个 ### 标识，切换语言不重置折叠、焦点或快捷键捕获。
    // 保留已有 ### 标识；普通 ## 控件继续使用完整原标签区分同名按钮。
    const auto stable = original.find("###");
    const auto identity = stable == std::string_view::npos ? original : original.substr(stable + 3);
    const std::string_view display = IsChinese() ? entry->chinese : visible;
    thread_local std::array<std::array<char, 4096>, 8> buffers {};
    thread_local size_t next = 0;
    auto& buffer = buffers[next++ % buffers.size()];
    if (display.size() + 3 + identity.size() >= buffer.size())
        return english;
    auto* cursor = buffer.data();
    std::memcpy(cursor, display.data(), display.size());
    cursor += display.size();
    std::memcpy(cursor, "###", 3);
    cursor += 3;
    std::memcpy(cursor, identity.data(), identity.size());
    cursor[identity.size()] = '\0';
    return buffer.data();
}
}
