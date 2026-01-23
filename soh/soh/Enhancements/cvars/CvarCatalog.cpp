// File: CVarCatalog.cpp

#include "CVarCatalog.h"

#include <libultraship/bridge.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "functions.h"
#include "soh/OTRGlobals.h"

#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" {
#include "z64.h"
#include "variables.h"
#include "macros.h"
}

namespace {
std::vector<CVarCatalogEntry> sEntries;
std::mutex sMutex;
} // namespace

namespace CVarCatalog {

void Register(const char* cvarName, const char* label) {
    if (cvarName == nullptr || cvarName[0] == '\0') {
        return;
    }

    std::scoped_lock lock(sMutex);

    // De-dupe by name; upgrade label if we learn a better one later
    for (auto& e : sEntries) {
        if (e.name == cvarName) {
            if ((e.label.empty() || e.label == e.name) && label != nullptr && label[0] != '\0') {
                e.label = label;
            }
            return;
        }
    }

    CVarCatalogEntry e;
    e.name = cvarName;
    e.label = (label != nullptr && label[0] != '\0') ? label : cvarName;
    sEntries.push_back(std::move(e));
}

const std::vector<CVarCatalogEntry>& GetAll() {
    return sEntries;
}

// ============================================================================
// CVar Binds hotkeys (checkbox CVars only)
//
// Stored in CVars:
//   gSettings.CVarBinds.Count
//   gSettings.CVarBinds.Entries.Entry<i>.CVar   (string)
//   gSettings.CVarBinds.Entries.Entry<i>.Mask   (int32 packed; low 16 bits used)
//
// IMPORTANT:
// Do NOT use numeric-only path segments like ".0." (it can be treated as an array
// index by config unflattening and break string-key lookups after reload).
// ============================================================================

static constexpr int32_t CVAR_CVAR_BINDS_COUNT_DEFAULT = 0;
#define CVAR_CVAR_BINDS_COUNT_NAME "gSettings.CVarBinds.Count"

static std::string GetBindPrefix(int index) {
    return "gSettings.CVarBinds.Entries.Entry" + std::to_string(index);
}

static std::string GetBindCVarName(int index) {
    return GetBindPrefix(index) + ".CVar";
}

static std::string GetBindMaskName(int index) {
    return GetBindPrefix(index) + ".Mask";
}

static void TryToggleBoundCVar(const char* targetCvarName) {
    if (targetCvarName == nullptr || targetCvarName[0] == '\0') {
        return;
    }

    const int32_t cur = CVarGetInteger(targetCvarName, 0);
    CVarSetInteger(targetCvarName, cur ? 0 : 1);
}

static void OnGameStateMainStartCVarBindsHotkeys() {
    const int32_t count = CVarGetInteger(CVAR_CVAR_BINDS_COUNT_NAME, CVAR_CVAR_BINDS_COUNT_DEFAULT);
    if (count <= 0) {
        return;
    }

    // If two binds share the same combo, only fire once per frame for that mask.
    static std::unordered_set<uint16_t> sFiredThisFrame;
    sFiredThisFrame.clear();

    for (int i = 0; i < count; i++) {
        const std::string maskName = GetBindMaskName(i);
        const int32_t packed = CVarGetInteger(maskName.c_str(), 0);
        const uint16_t mask = static_cast<uint16_t>(packed & 0xFFFF);

        if (mask == 0) {
            continue;
        }

        if (sFiredThisFrame.contains(mask)) {
            continue;
        }

        if (CHECK_BTN_ANY(gGameState->input[0].press.button, mask) &&
            CHECK_BTN_ALL(gGameState->input[0].cur.button, mask)) {

            const std::string bindCvarName = GetBindCVarName(i);
            const char* target = CVarGetString(bindCvarName.c_str(), "");

            TryToggleBoundCVar(target);

            // Re-run any RegisterShipInitFunc handlers that listed this CVar as an update path.
            if (target != nullptr && target[0] != '\0') {
                ShipInit::Init(target);
            }

            sFiredThisFrame.insert(mask);
        }
    }
}

static void RegisterCVarBindsHotkeys() {
    COND_HOOK(OnGameStateMainStart, true, OnGameStateMainStartCVarBindsHotkeys);
}

static RegisterShipInitFunc initFuncCVarBindsHotkeys(RegisterCVarBindsHotkeys, { CVAR_CVAR_BINDS_COUNT_NAME });

} // namespace CVarCatalog
