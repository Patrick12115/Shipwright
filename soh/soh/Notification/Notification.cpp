
#include "Notification.h"
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
}

namespace Notification {

static uint32_t nextId = 0;
static std::vector<Options> notifications = {};

void Window::Draw() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) {
        return;
    }

    const float margin = 30.0f;
    const float padding = 10.0f;
    const float boxPaddingX = 12.0f;
    const float boxPaddingY = 10.0f;
    const float rounding = 6.0f;
    const float iconSize = 32.0f;
    const float iconTextSpacing = 8.0f;

    int position = CVarGetInteger(CVAR_SETTING("Notifications.Position"), 3);
    if (position == 4) { // Hidden
        return;
    }

    // Base anchor position (same as your code)
    ImVec2 basePosition;
    switch (position) {
        case 0:
            basePosition = ImVec2(vp->Pos.x + margin, vp->Pos.y + margin);
            break; // TL
        case 1:
            basePosition = ImVec2(vp->Pos.x + vp->Size.x - margin, vp->Pos.y + margin);
            break; // TR
        case 2:
            basePosition = ImVec2(vp->Pos.x + margin, vp->Pos.y + vp->Size.y - margin);
            break; // BL
        case 3:
            basePosition = ImVec2(vp->Pos.x + vp->Size.x - margin, vp->Pos.y + vp->Size.y - margin);
            break; // BR
        default:
            return;
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList(vp);

    // You were using SetWindowFontScale; for draw lists, use font size explicitly.
    const float scale = CVarGetFloat(CVAR_SETTING("Notifications.Size"), 1.8f);
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * scale;

    // Stack newest at the “edge” (similar to your inverseIndex logic)
    float stackOffsetY = 0.0f;

    // Draw from newest -> oldest so stacking feels natural
    for (int i = (int)notifications.size() - 1; i >= 0; --i) {
        const Options& n = notifications[i];

        float alpha = 1.0f;
        if (n.remainingTime < 4.0f) {
            alpha = (n.remainingTime - 1.0f) / 3.0f;
            if (alpha < 0.0f)
                alpha = 0.0f;
            if (alpha > 1.0f)
                alpha = 1.0f;
        }

        // Text sizes (prefix/message/suffix)
        const bool hasIcon = (n.itemIcon != nullptr);
        const bool hasPrefix = !n.prefix.empty();
        const bool hasSuffix = !n.suffix.empty();

        // Measure text using the scaled font size
        ImVec2 prefixSz(0, 0), msgSz(0, 0), suffixSz(0, 0);

        if (hasPrefix)
            prefixSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, n.prefix.c_str());
        msgSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, n.message.c_str());
        if (hasSuffix)
            suffixSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, n.suffix.c_str());

        float contentW = 0.0f;
        float contentH = 0.0f;

        if (hasIcon) {
            contentW += iconSize + iconTextSpacing;
            contentH = (iconSize > contentH) ? iconSize : contentH;
        }

        contentW += prefixSz.x + msgSz.x + suffixSz.x;
        // Height is max of text line height and icon
        const float textH = (prefixSz.y > msgSz.y ? prefixSz.y : msgSz.y);
        const float textH2 = (suffixSz.y > textH ? suffixSz.y : textH);
        contentH = (textH2 > contentH) ? textH2 : contentH;

        const ImVec2 boxSize(contentW + boxPaddingX * 2.0f, contentH + boxPaddingY * 2.0f);

        // Compute box position based on corner + stacking
        ImVec2 boxPos = basePosition;

        const bool rightAligned = (position == 1 || position == 3);
        const bool bottomAligned = (position == 2 || position == 3);

        if (rightAligned) {
            boxPos.x -= boxSize.x;
        }
        if (bottomAligned) {
            boxPos.y -= boxSize.y;
            boxPos.y -= stackOffsetY;
        } else {
            boxPos.y += stackOffsetY;
        }

        ImVec2 boxPosMax(boxPos.x + boxSize.x, boxPos.y + boxSize.y);

        // Notification.cpp
        float bgOpacity = CVarGetFloat(CVAR_SETTING("Notifications.BgOpacity"), 0.5f);
        bgOpacity = ImClamp(bgOpacity, 0.0f, 1.0f);

        ImU32 bgCol = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, bgOpacity * alpha));
        dl->AddRectFilled(boxPos, boxPosMax, bgCol, rounding);

        // Draw contents
        ImVec2 cursor = ImVec2(boxPos.x + boxPaddingX, boxPos.y + boxPaddingY);

        if (hasIcon) {
            ImTextureID tex = Ship::Context::GetInstance()->GetWindow()->GetGui()->GetTextureByName(n.itemIcon);
            ImU32 iconTint = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha));
            dl->AddImage(tex, cursor, ImVec2(cursor.x + iconSize, cursor.y + iconSize), ImVec2(0, 0), ImVec2(1, 1),
                         iconTint);
            cursor.x += iconSize + iconTextSpacing;
        }

        // Apply alpha to colors
        auto withAlpha = [&](ImVec4 c) {
            c.w *= alpha;
            return c;
        };

        if (hasPrefix) {
            dl->AddText(font, fontSize, cursor, ImGui::GetColorU32(withAlpha(n.prefixColor)), n.prefix.c_str());
            cursor.x += prefixSz.x;
        }

        dl->AddText(font, fontSize, cursor, ImGui::GetColorU32(withAlpha(n.messageColor)), n.message.c_str());
        cursor.x += msgSz.x;

        if (hasSuffix) {
            dl->AddText(font, fontSize, cursor, ImGui::GetColorU32(withAlpha(n.suffixColor)), n.suffix.c_str());
        }

        // Advance stacking
        stackOffsetY += boxSize.y + padding;
    }
}

void Window::UpdateElement() {
    for (int index = 0; index < notifications.size(); ++index) {
        auto& notification = notifications[index];

        // decrement remainingTime
        notification.remainingTime -= ImGui::GetIO().DeltaTime;

        // remove notification if it has expired
        if (notification.remainingTime <= 0) {
            notifications.erase(notifications.begin() + index);
            --index;
        }
    }
}

void Emit(Options notification) {
    notification.id = nextId++;
    if (notification.remainingTime == 0.0f) {
        notification.remainingTime = CVarGetFloat(CVAR_SETTING("Notifications.Duration"), 10.0f);
    }
    notifications.push_back(notification);
    if (!notification.mute) {
        Audio_PlaySoundGeneral(NA_SE_SY_METRONOME, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                               &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
    }
}

} // namespace Notification
