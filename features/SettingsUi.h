#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "imgui.h"

namespace SettingsUi {

inline ImVec4 Accent()  { return ImVec4(0.29f, 0.63f, 0.98f, 1.00f); }
inline ImVec4 Violet()  { return ImVec4(0.62f, 0.49f, 0.96f, 1.00f); }
inline ImVec4 Success() { return ImVec4(0.32f, 0.82f, 0.64f, 1.00f); }
inline ImVec4 Warning() { return ImVec4(0.98f, 0.69f, 0.32f, 1.00f); }
inline ImVec4 TextColor() { return ImVec4(0.92f, 0.94f, 0.97f, 1.00f); }
inline ImVec4 MutedColor() { return ImVec4(0.57f, 0.62f, 0.70f, 1.00f); }
inline ImVec4 Surface0() { return ImVec4(0.035f, 0.043f, 0.055f, 1.00f); }
inline ImVec4 Surface1() { return ImVec4(0.051f, 0.060f, 0.076f, 0.99f); }
inline ImVec4 Surface2() { return ImVec4(0.066f, 0.078f, 0.098f, 1.00f); }
inline ImVec4 Hairline() { return ImVec4(0.16f, 0.19f, 0.24f, 0.82f); }

inline void PushTheme()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 12.0f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.025f, 0.031f, 0.041f, 0.998f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,        Surface0());
    ImGui::PushStyleColor(ImGuiCol_PopupBg,        ImVec4(0.043f, 0.051f, 0.066f, 0.998f));
    ImGui::PushStyleColor(ImGuiCol_Border,         Hairline());
    ImGui::PushStyleColor(ImGuiCol_BorderShadow,   ImVec4(0.0f, 0.0f, 0.0f, 0.28f));
    ImGui::PushStyleColor(ImGuiCol_Text,           TextColor());
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,   ImVec4(0.43f, 0.47f, 0.54f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.071f, 0.083f, 0.104f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.090f, 0.112f, 0.143f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.105f, 0.133f, 0.171f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,      Accent());
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,     Accent());
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Violet());
    ImGui::PushStyleColor(ImGuiCol_Button,         ImVec4(0.078f, 0.094f, 0.120f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.105f, 0.137f, 0.184f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.145f, 0.194f, 0.267f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Header,         ImVec4(0.074f, 0.098f, 0.132f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.102f, 0.145f, 0.204f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.130f, 0.184f, 0.258f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Tab,            ImVec4(0.055f, 0.066f, 0.084f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered,     ImVec4(0.091f, 0.132f, 0.188f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabSelected,    ImVec4(0.098f, 0.142f, 0.205f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, Accent());
    ImGui::PushStyleColor(ImGuiCol_Separator,      Hairline());
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.025f, 0.031f, 0.041f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.039f, 0.048f, 0.063f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,    ImVec4(0.021f, 0.026f, 0.034f, 0.82f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,  ImVec4(0.16f, 0.19f, 0.24f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, Accent());
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, Violet());
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.22f, 0.48f, 0.86f, 0.42f));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,  ImVec4(0.066f, 0.079f, 0.101f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg,     ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt,  ImVec4(0.075f, 0.086f, 0.105f, 0.42f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,  Accent());
    ImGui::PushStyleColor(ImGuiCol_PlotHistogramHovered, Violet());
    ImGui::PushStyleColor(ImGuiCol_NavCursor,      ImVec4(0.30f, 0.64f, 1.00f, 0.90f));
}

inline void PopTheme()
{
    ImGui::PopStyleColor(37);
    ImGui::PopStyleVar(9);
}

inline bool BeginCard(const char* id, const char* title, const char* subtitle = nullptr)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Surface1());
    ImGui::PushStyleColor(ImGuiCol_Border, Hairline());
    const bool visible = ImGui::BeginChild(
        id, ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
            ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (visible) {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float headerIndent = 11.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(cursor.x, cursor.y + 1.0f),
            ImVec2(cursor.x + 3.0f, cursor.y + ImGui::GetTextLineHeight() + 1.0f),
            ImGui::ColorConvertFloat4ToU32(Accent()), 1.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + headerIndent);
        ImGui::TextColored(TextColor(), "%s", title);
        if (subtitle && subtitle[0]) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + headerIndent);
            ImGui::TextColored(MutedColor(), "%s", subtitle);
        }
        ImGui::Dummy(ImVec2(0.0f, 3.0f));
        const ImVec2 lineStart = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            lineStart,
            ImVec2(lineStart.x + ImGui::GetContentRegionAvail().x, lineStart.y),
            ImGui::ColorConvertFloat4ToU32(Hairline()));
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
    }
    return visible;
}

inline void EndCard()
{
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    ImGui::Spacing();
}

inline void Subsection(const char* title, const char* description = nullptr)
{
    ImGui::Spacing();
    ImGui::TextColored(TextColor(), "%s", title);
    if (description && description[0])
        ImGui::TextColored(MutedColor(), "%s", description);
}

inline void Muted(const char* fmt, ...)
{
    char buffer[512] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    ImGui::TextColored(MutedColor(), "%s", buffer);
}

inline void MutedWrapped(const char* fmt, ...)
{
    char buffer[512] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    ImGui::PushStyleColor(ImGuiCol_Text, MutedColor());
    ImGui::TextWrapped("%s", buffer);
    ImGui::PopStyleColor();
}

inline void Status(const char* label, const char* value, bool good)
{
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(cursor.x + 4.0f, cursor.y + ImGui::GetTextLineHeight() * 0.5f),
        3.0f, ImGui::ColorConvertFloat4ToU32(good ? Success() : Warning()));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
    ImGui::TextColored(MutedColor(), "%s", label);
    ImGui::SameLine();
    ImGui::TextColored(good ? Success() : Warning(), "%s", value);
}

inline bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f))
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.19f, 0.45f, 0.83f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 0.96f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.17f, 0.38f, 0.71f, 1.00f));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

inline void MetricTile(const char* id,
                       const char* label,
                       const char* value,
                       const char* detail,
                       const ImVec4& valueColor)
{
    const float lineHeight = ImGui::GetTextLineHeight();
    const float verticalPadding = (std::max)(10.0f, lineHeight * 0.42f);
    const float tileHeight =
        (std::max)(72.0f,
                   verticalPadding * 2.0f +
                   lineHeight * 2.0f +
                   ImGui::GetStyle().ItemSpacing.y + 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(12.0f, verticalPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Surface2());
    ImGui::PushStyleColor(ImGuiCol_Border, Hairline());
    ImGui::BeginChild(id, ImVec2(0.0f, tileHeight),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 tileStart = ImGui::GetWindowPos();
    const ImVec2 tileSize = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRectFilled(
        tileStart, ImVec2(tileStart.x + 2.0f, tileStart.y + tileSize.y),
        ImGui::ColorConvertFloat4ToU32(valueColor), 1.0f);
    Muted("%s", label);
    ImGui::TextColored(valueColor, "%s", value);
    if (detail && detail[0]) {
        ImGui::SameLine();
        ImGui::TextColored(MutedColor(), "%s", detail);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace SettingsUi
