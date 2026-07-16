#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "imgui.h"

namespace SettingsUi {

inline ImVec4 Accent()  { return ImVec4(0.05f, 0.61f, 0.92f, 1.00f); }
inline ImVec4 Violet()  { return ImVec4(0.18f, 0.70f, 0.78f, 1.00f); }
inline ImVec4 Success() { return ImVec4(0.25f, 0.76f, 0.48f, 1.00f); }
inline ImVec4 Warning() { return ImVec4(0.98f, 0.63f, 0.08f, 1.00f); }
inline ImVec4 TextColor() { return ImVec4(0.91f, 0.92f, 0.95f, 1.00f); }
inline ImVec4 MutedColor() { return ImVec4(0.52f, 0.55f, 0.62f, 1.00f); }
inline ImVec4 Surface0() { return ImVec4(0.074f, 0.076f, 0.092f, 1.00f); }
inline ImVec4 Surface1() { return ImVec4(0.094f, 0.096f, 0.114f, 1.00f); }
inline ImVec4 Surface2() { return ImVec4(0.118f, 0.120f, 0.142f, 1.00f); }
inline ImVec4 Hairline() { return ImVec4(0.19f, 0.20f, 0.24f, 0.90f); }

inline void PushTheme()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(9.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.0f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.058f, 0.060f, 0.073f, 0.998f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,        Surface0());
    ImGui::PushStyleColor(ImGuiCol_PopupBg,        ImVec4(0.105f, 0.108f, 0.128f, 0.998f));
    ImGui::PushStyleColor(ImGuiCol_Border,         Hairline());
    ImGui::PushStyleColor(ImGuiCol_BorderShadow,   ImVec4(0.0f, 0.0f, 0.0f, 0.18f));
    ImGui::PushStyleColor(ImGuiCol_Text,           TextColor());
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,   ImVec4(0.39f, 0.41f, 0.47f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.126f, 0.130f, 0.153f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.154f, 0.161f, 0.190f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.180f, 0.190f, 0.224f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,      Accent());
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,     Accent());
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Violet());
    ImGui::PushStyleColor(ImGuiCol_Button,         ImVec4(0.137f, 0.141f, 0.165f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.174f, 0.183f, 0.216f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.095f, 0.314f, 0.447f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Header,         ImVec4(0.128f, 0.147f, 0.177f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.151f, 0.188f, 0.226f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.075f, 0.310f, 0.450f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Tab,            ImVec4(0.091f, 0.094f, 0.112f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered,     ImVec4(0.125f, 0.154f, 0.185f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabSelected,    ImVec4(0.112f, 0.149f, 0.179f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, Accent());
    ImGui::PushStyleColor(ImGuiCol_Separator,      Hairline());
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.075f, 0.077f, 0.090f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.094f, 0.096f, 0.113f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,    ImVec4(0.060f, 0.062f, 0.075f, 0.82f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,  ImVec4(0.21f, 0.22f, 0.26f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, Accent());
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, Violet());
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.04f, 0.45f, 0.72f, 0.42f));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,  ImVec4(0.118f, 0.121f, 0.143f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg,     ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt,  ImVec4(0.145f, 0.148f, 0.170f, 0.34f));
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Surface1());
    ImGui::PushStyleColor(ImGuiCol_Border, Hairline());
    const bool visible = ImGui::BeginChild(
        id, ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
            ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (visible) {
        const ImVec2 child = ImGui::GetWindowPos();
        const float headerWidth = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(child.x + 1.0f, child.y + 1.0f),
            ImVec2(child.x + 34.0f, child.y + 3.0f),
            ImGui::ColorConvertFloat4ToU32(Accent()));
        ImGui::TextColored(TextColor(), "%s", title);
        if (subtitle && subtitle[0]) {
            const float inlineWidth = ImGui::CalcTextSize(title).x +
                ImGui::CalcTextSize(subtitle).x + ImGui::GetStyle().ItemSpacing.x + 18.0f;
            if (inlineWidth <= headerWidth) {
                ImGui::SameLine(0.0f, 12.0f);
                ImGui::TextColored(MutedColor(), "%s", subtitle);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, MutedColor());
                ImGui::TextWrapped("%s", subtitle);
                ImGui::PopStyleColor();
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        const ImVec2 lineStart = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            lineStart,
            ImVec2(lineStart.x + ImGui::GetContentRegionAvail().x, lineStart.y),
            ImGui::ColorConvertFloat4ToU32(Hairline()));
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
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
    const bool descriptionInline = description && description[0] &&
        ImGui::CalcTextSize(title).x + ImGui::CalcTextSize(description).x + 19.0f <=
            ImGui::GetContentRegionAvail().x;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        start, ImVec2(start.x + 2.0f, start.y + ImGui::GetTextLineHeight()),
        ImGui::ColorConvertFloat4ToU32(Accent()));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 9.0f);
    ImGui::TextColored(TextColor(), "%s", title);
    if (description && description[0]) {
        if (descriptionInline)
            ImGui::SameLine(0.0f, 10.0f);
        else
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 9.0f);
        ImGui::TextColored(MutedColor(), "%s", description);
    }
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
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.04f, 0.55f, 0.84f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.08f, 0.65f, 0.96f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.03f, 0.44f, 0.69f, 1.00f));
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
    const float verticalPadding = (std::max)(8.0f, lineHeight * 0.34f);
    const float availableWidth = (std::max)(
        0.0f, ImGui::GetContentRegionAvail().x - 20.0f);
    const float combinedWidth = ImGui::CalcTextSize(value).x +
        ((detail && detail[0]) ? ImGui::CalcTextSize(detail).x + 8.0f : 0.0f);
    const bool detailOwnLine = detail && detail[0] && combinedWidth > availableWidth;
    const int textLines = detailOwnLine ? 3 : 2;
    const float tileHeight =
        (std::max)(64.0f,
                   verticalPadding * 2.0f +
                   lineHeight * static_cast<float>(textLines) +
                   ImGui::GetStyle().ItemSpacing.y * static_cast<float>(textLines - 1) + 2.0f);
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
    ImGui::GetWindowDrawList()->AddRectFilled(
        tileStart, ImVec2(tileStart.x + 26.0f, tileStart.y + 2.0f),
        ImGui::ColorConvertFloat4ToU32(valueColor), 1.0f);
    Muted("%s", label);
    ImGui::TextColored(valueColor, "%s", value);
    if (detail && detail[0]) {
        if (!detailOwnLine)
            ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextColored(MutedColor(), "%s", detail);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace SettingsUi
