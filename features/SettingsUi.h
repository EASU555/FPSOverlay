#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "imgui.h"

namespace SettingsUi {

inline ImVec4 Accent()  { return ImVec4(0.30f, 0.64f, 1.00f, 1.00f); }
inline ImVec4 Violet()  { return ImVec4(0.52f, 0.40f, 1.00f, 1.00f); }
inline ImVec4 Success() { return ImVec4(0.27f, 0.84f, 0.64f, 1.00f); }
inline ImVec4 Warning() { return ImVec4(1.00f, 0.67f, 0.31f, 1.00f); }
inline ImVec4 MutedColor() { return ImVec4(0.53f, 0.59f, 0.70f, 1.00f); }

inline void PushTheme()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 7.0f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.027f, 0.035f, 0.070f, 0.995f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,        ImVec4(0.044f, 0.056f, 0.098f, 0.980f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,        ImVec4(0.040f, 0.050f, 0.090f, 0.998f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.20f, 0.25f, 0.36f, 0.72f));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow,   ImVec4(0.02f, 0.03f, 0.08f, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_Text,           ImVec4(0.91f, 0.94f, 0.99f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,   ImVec4(0.47f, 0.52f, 0.62f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.063f, 0.080f, 0.132f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.086f, 0.119f, 0.190f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.105f, 0.145f, 0.225f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,      Accent());
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,     Accent());
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Violet());
    ImGui::PushStyleColor(ImGuiCol_Button,         ImVec4(0.067f, 0.091f, 0.151f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.095f, 0.151f, 0.240f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.205f, 0.155f, 0.420f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Header,         ImVec4(0.075f, 0.110f, 0.180f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.105f, 0.175f, 0.275f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.215f, 0.165f, 0.430f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Tab,            ImVec4(0.055f, 0.070f, 0.118f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered,     ImVec4(0.090f, 0.150f, 0.240f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabSelected,    ImVec4(0.105f, 0.155f, 0.255f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, Accent());
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.19f, 0.24f, 0.35f, 0.78f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.025f, 0.034f, 0.068f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.045f, 0.060f, 0.110f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,    ImVec4(0.020f, 0.028f, 0.058f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,  ImVec4(0.15f, 0.20f, 0.31f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, Accent());
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, Violet());
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.22f, 0.48f, 0.86f, 0.42f));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,  ImVec4(0.075f, 0.105f, 0.170f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,  Accent());
    ImGui::PushStyleColor(ImGuiCol_PlotHistogramHovered, Violet());
    ImGui::PushStyleColor(ImGuiCol_NavCursor,      ImVec4(0.30f, 0.64f, 1.00f, 0.90f));
}

inline void PopTheme()
{
    ImGui::PopStyleColor(35);
    ImGui::PopStyleVar(6);
}

inline bool BeginCard(const char* id, const char* title, const char* subtitle = nullptr)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(13.0f, 11.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.050f, 0.064f, 0.108f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.17f, 0.21f, 0.31f, 0.82f));
    const bool visible = ImGui::BeginChild(
        id, ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
            ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (visible) {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(cursor.x, cursor.y + 5.0f),
            ImVec2(cursor.x + 5.0f, cursor.y + 10.0f),
            IM_COL32(77, 164, 255, 255), 3.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 13.0f);
        ImGui::TextColored(ImVec4(0.91f, 0.94f, 0.99f, 1.00f), "%s", title);
        if (subtitle && subtitle[0]) {
            const float subtitleWidth = ImGui::CalcTextSize(subtitle).x;
            if (ImGui::GetContentRegionAvail().x > subtitleWidth + 12.0f) {
                ImGui::SameLine();
            } else {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 13.0f);
            }
            ImGui::TextColored(MutedColor(), "%s", subtitle);
        }
        ImGui::Separator();
        ImGui::Spacing();
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
    ImGui::TextColored(Accent(), "%s", title);
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
    ImGui::TextColored(MutedColor(), "%s", label);
    ImGui::SameLine();
    ImGui::TextColored(good ? Success() : Warning(), "%s", value);
}

inline bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f))
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.43f, 0.86f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.53f, 0.98f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.38f, 0.30f, 0.82f, 1.00f));
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
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.047f, 0.060f, 0.104f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.16f, 0.20f, 0.30f, 0.82f));
    ImGui::BeginChild(id, ImVec2(0.0f, tileHeight),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
