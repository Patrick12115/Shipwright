#pragma once

#include "item-tables/ItemTableTypes.h" // for GetItemEntry
#include "soh/Enhancements/randomizer/randomizerTypes.h"

// Number of advanced categories we want
#define SKIP_GI_ADVANCED_CATEGORY_COUNT 16

// CVar names (TimeSavers.SkipGetItemAnimationAdvanced.*)
extern const char* skipGIAdvancedCVarList[SKIP_GI_ADVANCED_CATEGORY_COUNT];

// Text shown in the menu ("Skip GI: Items", etc.)
extern const char* skipGIAdvancedNameList[SKIP_GI_ADVANCED_CATEGORY_COUNT];

#ifdef __cplusplus
// Advanced-mode logic: only used from C++ code
bool ShouldSkipGetItemAnimationAdvanced(const GetItemEntry& entry);

RandomizerCheck GetLastIceTrapCheck();

#endif
