#pragma once

#include "localization.h"
#include <imgui/imgui.h>
#include <cstdarg>

// 仅用于 SSMT 面板的显示入口，不修改 ImGui、配置值或渲染后端。
namespace OptiMenu
{
using namespace ImGui;
inline bool Begin(const char* label, bool* open = nullptr, ImGuiWindowFlags flags = 0)
{
    return ImGui::Begin(MenuL10n::Label(label), open, flags);
}
inline bool Button(const char* label, const ImVec2& size = ImVec2(0, 0))
{
    return ImGui::Button(MenuL10n::Label(label), size);
}
inline bool SmallButton(const char* label) { return ImGui::SmallButton(MenuL10n::Label(label)); }
inline bool Checkbox(const char* label, bool* value) { return ImGui::Checkbox(MenuL10n::Label(label), value); }
inline bool CheckboxFlags(const char* label, unsigned int* flags, unsigned int value)
{
    return ImGui::CheckboxFlags(MenuL10n::Label(label), flags, value);
}
inline bool RadioButton(const char* label, bool active) { return ImGui::RadioButton(MenuL10n::Label(label), active); }
inline bool RadioButton(const char* label, int* value, int button)
{
    return ImGui::RadioButton(MenuL10n::Label(label), value, button);
}
inline bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0)
{
    return ImGui::CollapsingHeader(MenuL10n::Label(label), flags);
}
inline bool TreeNode(const char* label) { return ImGui::TreeNode(MenuL10n::Label(label)); }
inline bool BeginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0)
{
    return ImGui::BeginCombo(MenuL10n::Label(label), MenuL10n::Text(preview), flags);
}
inline bool Selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0,
                       const ImVec2& size = ImVec2(0, 0))
{
    return ImGui::Selectable(MenuL10n::Label(label), selected, flags, size);
}
inline void SeparatorText(const char* label) { ImGui::SeparatorText(MenuL10n::Text(label)); }
inline void TableSetupColumn(const char* label, ImGuiTableColumnFlags flags = 0, float width = 0, ImGuiID id = 0)
{
    ImGui::TableSetupColumn(MenuL10n::Text(label), flags, width, id);
}
inline bool SliderFloat(const char* label, float* v, float lo, float hi, const char* fmt = "%.3f",
                        ImGuiSliderFlags flags = 0)
{
    return ImGui::SliderFloat(MenuL10n::Label(label), v, lo, hi, MenuL10n::Text(fmt), flags);
}
inline bool SliderInt(const char* label, int* v, int lo, int hi, const char* fmt = "%d", ImGuiSliderFlags flags = 0)
{
    return ImGui::SliderInt(MenuL10n::Label(label), v, lo, hi, MenuL10n::Text(fmt), flags);
}
inline bool InputFloat(const char* label, float* v, float step = 0, float fast = 0, const char* fmt = "%.3f",
                       ImGuiInputTextFlags flags = 0)
{
    return ImGui::InputFloat(MenuL10n::Label(label), v, step, fast, MenuL10n::Text(fmt), flags);
}
inline bool InputInt(const char* label, int* v, int step = 1, int fast = 100, ImGuiInputTextFlags flags = 0)
{
    return ImGui::InputInt(MenuL10n::Label(label), v, step, fast, flags);
}
inline bool InputScalar(const char* label, ImGuiDataType type, void* value, const void* step = nullptr,
                        const void* fast = nullptr, const char* fmt = nullptr, ImGuiInputTextFlags flags = 0)
{
    return ImGui::InputScalar(MenuL10n::Label(label), type, value, step, fast, MenuL10n::Text(fmt), flags);
}
inline bool ColorEdit3(const char* label, float* color, ImGuiColorEditFlags flags = 0)
{
    return ImGui::ColorEdit3(MenuL10n::Label(label), color, flags);
}
inline void Text(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(MenuL10n::Text(fmt), args);
    va_end(args);
}
inline void TextDisabled(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::TextDisabledV(MenuL10n::Text(fmt), args);
    va_end(args);
}
inline void TextColored(const ImVec4& color, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::TextColoredV(color, MenuL10n::Text(fmt), args);
    va_end(args);
}
inline void TextWrapped(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::TextWrappedV(MenuL10n::Text(fmt), args);
    va_end(args);
}
inline void SetTooltip(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::SetTooltipV(MenuL10n::Text(fmt), args);
    va_end(args);
}
inline void TextUnformatted(const char* text, const char* end = nullptr)
{
    ImGui::TextUnformatted(end == nullptr ? MenuL10n::Text(text) : text, end);
}
inline ImVec2 CalcTextSize(const char* text, const char* end = nullptr, bool hide = false, float wrap = -1)
{
    return ImGui::CalcTextSize(end == nullptr ? MenuL10n::Text(text) : text, end, hide, wrap);
}
inline void TextLinkOpenURL(const char* label, const char* url = nullptr)
{
    ImGui::TextLinkOpenURL(MenuL10n::Label(label), url != nullptr ? url : label);
}

struct ComboItems
{
    const char* const* items;
};
inline bool Combo(const char* label, int* value, const char* const items[], int count, int height = -1)
{
    ComboItems data { items };
    return ImGui::Combo(
        MenuL10n::Label(label), value,
        [](void* p, int i) { return MenuL10n::Text(static_cast<ComboItems*>(p)->items[i]); }, &data, count, height);
}
inline bool Combo(const char* label, int* value, const char* zeros, int height = -1)
{
    int count = 0;
    for (const char* p = zeros; *p; p += std::strlen(p) + 1)
        ++count;
    return ImGui::Combo(
        MenuL10n::Label(label), value,
        [](void* p, int i)
        {
            auto* item = static_cast<const char*>(p);
            while (i-- > 0)
                item += std::strlen(item) + 1;
            return MenuL10n::Text(item);
        },
        const_cast<char*>(zeros), count, height);
}
} // namespace OptiMenu
