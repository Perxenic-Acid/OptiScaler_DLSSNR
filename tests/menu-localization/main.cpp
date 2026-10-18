#include <menu/menu_localization.h>
#include <imgui/imgui_internal.h>
#include <cstdio>
#include <string>
#include <set>
#ifdef _WIN32
#include <menu/menu_localization_font.h>
#endif

static int failures = 0;
static void Check(bool passed, const char* name)
{
    if (!passed) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}

// 比较有序 printf 占位符，防止翻译更改 va_list 的类型或顺序。
static std::string FormatSignature(std::string_view text)
{
    std::string result;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '%') continue;
        if (i > 0 && text[i - 1] >= '0' && text[i - 1] <= '9') continue;
        if (i + 1 < text.size() && text[i + 1] == '%') { ++i; continue; }
        const auto start = i++;
        while (i < text.size() && std::strchr("-+ #0.123456789*hljztLI", text[i])) ++i;
        if (i < text.size() && std::strchr("diuoxXfFeEgGaAcspn", text[i]))
            result += std::string(text.substr(start, i - start + 1)) + "|";
    }
    return result;
}

int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
#ifdef _WIN32
    MenuL10n::chineseFontAvailable = MenuL10n::AddChineseFont(io.Fonts, 16);
    MenuL10n::SelectLanguage("zh-cn");
    Check(MenuL10n::chineseRequested, "explicit Chinese preference");
    if (!MenuL10n::chineseFontAvailable)
    {
        // 云端英文 Windows 可能没有 CJK 字体；这也是产品必须支持的正常回退路径。
        Check(!MenuL10n::IsChinese(), "missing system font uses English");
        Check(std::strcmp(MenuL10n::Text("Save Settings"), "Save Settings") == 0, "actual missing font fallback");
    }
    MenuL10n::SelectLanguage("en");
    Check(!MenuL10n::chineseRequested, "explicit English preference");
#endif
    const bool actualChineseFont = MenuL10n::chineseFontAvailable;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    ImFontAtlasBuildMain(io.Fonts);
    pixels = io.Fonts->TexData->Pixels;
    width = io.Fonts->TexData->Width;
    height = io.Fonts->TexData->Height;
    io.Fonts->TexData->SetTexID(1);
    Check(pixels != nullptr && width > 0 && height > 0, "font atlas builds without graphics device");
#ifdef _WIN32
    auto* baked = io.Fonts->Fonts[0]->GetFontBaked(16);
    std::set<ImWchar> glyphs;
    for (const auto& entry : MenuL10n::dictionary)
    {
        for (auto* p = entry.chinese; *p;)
        {
            unsigned int c = 0;
            const int size = ImTextCharFromUtf8(&c, p, nullptr);
            Check(size > 0, "valid UTF-8");
            if (size <= 0) break;
            if (c >= 0x100) glyphs.insert(static_cast<ImWchar>(c));
            p += size;
        }
    }
    if (actualChineseFont)
    {
        for (auto c : glyphs)
            Check(baked->FindGlyphNoFallback(c) != nullptr, "translated glyph present");
        std::printf("Verified %zu Chinese/punctuation glyphs, atlas %dx%d\n", glyphs.size(), width, height);
    }
    else
        std::puts("SKIP: Chinese glyph coverage (no system Chinese font); English fallback tested");
#endif
    std::set<std::string_view> keys;
    for (const auto& entry : MenuL10n::dictionary)
    {
        Check(keys.insert(entry.english).second, "unique translation key");
        if (FormatSignature(entry.english) != FormatSignature(entry.chinese))
        {
            std::fprintf(stderr, "FORMAT: %.*s\n", static_cast<int>(entry.english.size()), entry.english.data());
            Check(false, "printf signature preserved");
        }
    }
    MenuL10n::chineseFontAvailable = true;
    MenuL10n::chineseRequested = false;
    const auto english = ImHashStr(MenuL10n::Label("Reset##detail"));
    const auto explicitId = ImHashStr(MenuL10n::Label("Reset###saved-id"));
    MenuL10n::chineseRequested = true;
    Check(ImHashStr(MenuL10n::Label("Reset##detail")) == english, "language preserves widget ID");
    Check(ImHashStr(MenuL10n::Label("Reset##colour")) != english, "distinct reset controls stay distinct");
    Check(ImHashStr(MenuL10n::Label("Reset###saved-id")) == explicitId, "explicit ID preserved");
    Check(explicitId == ImHashStr("Reset###saved-id"), "original explicit ID unchanged");
    Check(std::strcmp(MenuL10n::Label("##hidden"), "##hidden") == 0, "hidden IDs untouched");
    Check(std::strcmp(MenuL10n::Text("unknown future option"), "unknown future option") == 0, "unknown fallback");
    Check(std::strcmp(MenuL10n::Text("Save Settings"), "保存设置") == 0, "Chinese text selected");
    MenuL10n::chineseFontAvailable = false;
    Check(std::strcmp(MenuL10n::Text("Save Settings"), "Save Settings") == 0, "font unavailable fallback");
    MenuL10n::chineseFontAvailable = true;
    ImGuiID checkboxId = 0;
    bool checkbox = true;
    for (int frame = 0; frame < 2; ++frame)
    {
        MenuL10n::chineseRequested = frame != 0;
        MenuL10n::chineseFontAvailable = actualChineseFont;
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(800, 600));
        OptiMenu::Begin("Localization test");
        OptiMenu::Checkbox("Enable Neural Rendering", &checkbox);
        if (frame == 0) checkboxId = ImGui::GetItemID();
        else Check(checkboxId == ImGui::GetItemID(), "real widget ID across language switch");
        int value = 0;
        const char* items[] = { "Off", "Natural", "Cinematic" };
        OptiMenu::Combo("Style", &value, items, 3);
        OptiMenu::Combo("Compare", &value, "Off\0Side by side\0Wipe\0");
        OptiMenu::Text("Off for this session: %s.", "test");
        OptiMenu::TextWrapped("Save Settings");
        ImGui::End();
        ImGui::Render();
        Check(ImGui::GetDrawData()->Valid, "real ImGui draw data produced");
        Check(ImGui::GetDrawData()->CmdListsCount > 0, "visible menu geometry produced");
    }
    ImGui::DestroyContext();
    std::printf("Verified %zu translations; failures=%d\n", keys.size(), failures);
    return failures ? 1 : 0;
}
