#pragma once

#include "TextEditor.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"

#include <string>
#include <vector>
#include <string>
#include <chrono>

inline ImColor active_color(57, 255, 20, 255);

inline ImColor active_color_half(57, 255, 20, 60);

// Paleta del tema: tonos VERDES (nada de negro). Antes estos colores eran casi negros
// (10..20) y por eso los widgets (ComboHQ / KeybindHQ / paneles) se veian con "bordes negros".
inline ImColor default_color(26, 44, 28, 255);

inline ImColor text_color[2] = { ImColor(232, 255, 228, 255), ImColor(168, 214, 162, 255) };

inline ImColor background_color(22, 38, 24, 255);

inline ImColor second_color(30, 50, 32, 255);

inline ImColor stroke_color(57, 255, 20, 0);

inline ImColor stroke_color_active(57, 255, 20, 90);

inline ImColor child_color[2] = { ImColor(34, 56, 36, 255), ImColor(28, 48, 30, 255) };

inline ImColor scroll_bg_col(30, 50, 32, 255);

inline ImColor winbg_color(22, 38, 24, 255);

inline ImColor menu_title_color(57, 255, 20, 255);
inline ImColor menu_button_color(57, 255, 20, 255);
inline ImColor menu_tabs_color(24, 42, 26, 230);

inline ImFont* icon_font;

inline ImFont* logo_font;

inline ImFont* tabs_font;

inline ImFont* small_font;

inline ImFont* default_font;

inline ImFont* small_icon_font;

inline ImFont* title_font;

inline ImFont* arrow_icons;

inline ImVec2 frame_size(270, 16);

inline float anim_speed = 0.05f;

inline const TextEditor::Palette rE_palette = { {
                0xff7f7f7f,
                ImColor(187, 134, 192),
                ImColor(181, 206, 155),
                ImColor(206, 145, 120),
                ImColor(206, 145, 120),
                0xffffffff,
                0xff408080,
                0xffaaaaaa,
                ImColor(220, 205, 121),
                0xffc040a0,
                ImColor(106, 153, 62),
                ImColor(106, 153, 62),
                child_color[1],
                0xffe0e0e0,
                0x80a06020,
                0x800020ff,
                0x40f08000,
                ImColor(1.f, 1.f, 1.f, 0.2f),
                0x40000000,
                0x40808080,
                0x40a0a0a0,
            } };

inline float ImDegToRad(float degrees)
{
    static const float deg_to_rad = 0.01745329251994329576923690768489f;
    return degrees * deg_to_rad;
}

inline ImColor GetColorWithAlpha(ImColor color, float alpha)
{
    return ImColor(color.Value.x, color.Value.y, color.Value.z, alpha);
}

inline ImVec2 center_text(ImVec2 min, ImVec2 max, const char* text)
{
    return min + (max - min) / 2 - ImGui::CalcTextSize(text) / 2;
}

inline bool ColorPickerHQ(const char* label, float col[4])
{
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.03f, 0.10f, 0.03f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.06f, 0.18f, 0.05f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.09f, 0.26f, 0.08f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.04f, 0.12f, 0.04f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.07f, 0.20f, 0.06f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.30f, 0.08f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.224f, 1.f, 0.078f, 0.3f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    bool ret = ImGui::ColorEdit4(label, col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_AlphaPreview);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(7);
    return ret;
}

inline int rotation_start_index;
inline void ImRotateStart()
{
    rotation_start_index = ImGui::GetWindowDrawList()->VtxBuffer.Size;
}

inline ImVec2 ImRotationCenter()
{
    ImVec2 l(FLT_MAX, FLT_MAX), u(-FLT_MAX, -FLT_MAX);

    const auto& buf = ImGui::GetWindowDrawList()->VtxBuffer;
    for (int i = rotation_start_index; i < buf.Size; i++)
        l = ImMin(l, buf[i].pos), u = ImMax(u, buf[i].pos);

    return ImVec2((l.x + u.x) / 2, (l.y + u.y) / 2);
}

inline ImVec4 ImColorToImVec4(const ImColor& color)
{
    return ImVec4(color.Value.x, color.Value.y, color.Value.z, color.Value.w);
}

inline void Pickerbox(std::string label, bool* v, float col[4])
{
    std::string picker_name = "##picker" + label;

    ImGui::SetCursorPos(ImGui::GetCursorPos() - ImVec2(30, 0));
    ImGui::ColorEdit4(picker_name.c_str(), (float*)col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

    ImGui::SetCursorPos(ImGui::GetCursorPos() - ImVec2(0, 53));
    ImGui::Checkbox(label.c_str(), v);
}

inline void Keybindbox(std::string label, bool* v, int* key)
{
    std::string picker_name = "##keybind" + label;

    ImGui::SetCursorPos(ImGui::GetCursorPos() - ImVec2(30, 0));
    ImGui::Keybind(picker_name.c_str(), key);
    ImGui::SetCursorPos(ImGui::GetCursorPos() - ImVec2(0, 43));
    ImGui::Checkbox(label.c_str(), v);
}

inline void ImRotateEnd(float rad, ImVec2 center = ImRotationCenter())
{
    float s = (float)sin(rad), c = (float)cos(rad);
    center = ImRotate(center, s, c) - center;

    auto& buf = ImGui::GetWindowDrawList()->VtxBuffer;
    for (int i = rotation_start_index; i < buf.Size; i++)
        buf[i].pos = ImRotate(buf[i].pos, s, c) - center;
}

inline void rect_glow(ImDrawList* draw, ImVec2 start, ImVec2 end, ImColor col, float rounding, float intensity) {
    while (true) {
        if (col.Value.w < 0.0019f)
            break;

        draw->AddRectFilled(start, end, (ImU32)col, rounding);

        col.Value.w -= col.Value.w / intensity;
        start = ImVec2(start.x - 1, start.y - 1);
        end = ImVec2(end.x + 1, end.y + 1);
    }
}
