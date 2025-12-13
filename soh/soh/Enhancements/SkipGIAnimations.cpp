#include "SkipGIAnimations.h"

#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "soh/Enhancements/randomizer/3drando/random.hpp"
#include "soh/Enhancements/randomizer/context.h"
#include "soh/Enhancements/enhancementTypes.h"
#include "soh/OTRGlobals.h"
#include "soh/cvar_prefixes.h"
#include "soh/ResourceManagerHelpers.h"
#include "soh/Enhancements/item-tables/ItemTableManager.h"

// --------------------------------------------------------
// CVar wiring
// --------------------------------------------------------

#define CVAR_SKIP_GI_SIMPLE CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimation")

// Indices into the advanced category arrays
// Order must match the user's requested order.
enum SkipGIAdvancedCategoryIndex {
    SKIPGI_CAT_MAJOR_ITEMS = 0,
    SKIPGI_CAT_LESSER_ITEMS,
    SKIPGI_CAT_TRIFORCE,
    SKIPGI_CAT_GREG_RUPEE,
    SKIPGI_CAT_SONGS,
    SKIPGI_CAT_DUNGEON_REWARDS,
    SKIPGI_CAT_BOSS_SOULS,
    SKIPGI_CAT_BOSS_KEYS,
    SKIPGI_CAT_DUNGEON_KEYS,
    SKIPGI_CAT_OVERWORLD_KEYS,
    SKIPGI_CAT_SKULLTULA_TOKENS,
    SKIPGI_CAT_HEART_CONTAINERS,
    SKIPGI_CAT_HEART_PIECES,
    SKIPGI_CAT_MAPS_COMPASSES,
    SKIPGI_CAT_CONSUMABLES,
    SKIPGI_CAT_ICE_TRAPS,
};

// NOTE: You must update SKIP_GI_ADVANCED_CATEGORY_COUNT to 16 in SkipGIAnimations.h

// Keep this in sync with the enum above
const char* skipGIAdvancedCVarList[SKIP_GI_ADVANCED_CATEGORY_COUNT] = {
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.MajorItems"),      // 0
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.LesserItems"),     // 1
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.Triforce"),        // 2
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.GregRupee"),       // 3
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.Songs"),           // 4
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.DungeonRewards"),  // 5
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.BossSouls"),       // 6
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.BossKeys"),        // 7
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.DungeonKeys"),     // 8
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.OverworldKeys"),   // 9
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.SkulltulaTokens"), // 10
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.HeartContainers"), // 11
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.HeartPieces"),     // 12
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.MapsCompasses"),   // 13
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.Consumables"),     // 14
    CVAR_RANDOMIZER_ENHANCEMENT("TimeSavers.SkipGetItemAnimationAdvanced.IceTraps"),        // 15
};

const char* skipGIAdvancedNameList[SKIP_GI_ADVANCED_CATEGORY_COUNT] = {
    "Major Items",          // 0
    "Lesser Items",         // 1
    "Triforce Pieces",      // 2
    "Greg the Green Rupee", // 3
    "Songs",                // 4
    "Dungeon Rewards",      // 5
    "Boss Souls",           // 6
    "Boss Keys",            // 7
    "Dungeon Keys",         // 8
    "Overworld Keys",       // 9
    "Skulltula Tokens",     // 10
    "Heart Containers",     // 11
    "Heart Pieces",         // 12
    "Maps & Compasses",     // 13
    "Consumables",          // 14
    "All Ice Traps",        // 15
};

// --------------------------------------------------------
// Helpers
// --------------------------------------------------------

static GetItemEntry GetVisualEntry(const GetItemEntry& entry) {
    // drawItemId is a uint16_t in GetItemEntry, but ItemTableManager expects GetItemID.
    // In practice, vanilla draw ids map to the vanilla GetItem table.
    return ItemTableManager::Instance->RetrieveItemEntry(entry.drawModIndex, (GetItemID)entry.drawItemId);
}

static bool IsRandomizerEntry(const GetItemEntry& entry) {
    return entry.modIndex == MOD_RANDOMIZER;
}

// ---------- Specific RG-based check helpers ----------

static bool IsFirstMajorProgressive(const GetItemEntry& entry) {
    if (!IsRandomizerEntry(entry)) {
        return false;
    }

    switch (entry.getItemId) {
        case RG_PROGRESSIVE_BOMB_BAG:
            return CUR_UPG_VALUE(UPG_BOMB_BAG) == 0;
        case RG_PROGRESSIVE_BOW:
            return INV_CONTENT(ITEM_BOW) == ITEM_NONE;
        case RG_PROGRESSIVE_SLINGSHOT:
            return INV_CONTENT(ITEM_SLINGSHOT) == ITEM_NONE;
        case RG_PROGRESSIVE_NUT_UPGRADE:
            return CUR_UPG_VALUE(UPG_NUTS) == 0;
        case RG_PROGRESSIVE_STICK_UPGRADE:
            return CUR_UPG_VALUE(UPG_STICKS) == 0;
        case RG_PROGRESSIVE_MAGIC_METER:
            return gSaveContext.magicLevel == 0;
        case RG_PROGRESSIVE_OCARINA:
            return INV_CONTENT(ITEM_OCARINA_FAIRY) == ITEM_NONE;
        case RG_PROGRESSIVE_BOMBCHU_BAG:
            return INV_CONTENT(ITEM_BOMBCHU) == ITEM_NONE;
        default:
            return false;
    }
}

static bool IsMajorItem(const GetItemEntry& entry) {
    if (!IsRandomizerEntry(entry)) {
        return false;
    }

    // Special rule: selected progressives are major for the first copy only.
    switch (entry.getItemId) {
        case RG_PROGRESSIVE_BOMB_BAG:
        case RG_PROGRESSIVE_BOW:
        case RG_PROGRESSIVE_SLINGSHOT:
        case RG_PROGRESSIVE_NUT_UPGRADE:
        case RG_PROGRESSIVE_STICK_UPGRADE:
        case RG_PROGRESSIVE_MAGIC_METER:
        case RG_PROGRESSIVE_OCARINA:
        case RG_PROGRESSIVE_BOMBCHU_BAG:
            return IsFirstMajorProgressive(entry);
        default:
            break;
    }

    switch (entry.getItemId) {
        // Swords
        case RG_KOKIRI_SWORD:
        case RG_MASTER_SWORD:
        case RG_GIANTS_KNIFE:
        case RG_BIGGORON_SWORD:

        // Shields
        case RG_MIRROR_SHIELD:

        // Boots
        case RG_IRON_BOOTS:
        case RG_HOVER_BOOTS:

        // Utility / combat items
        case RG_BOOMERANG:
        case RG_LENS_OF_TRUTH:
        case RG_MEGATON_HAMMER:

        // Spells
        case RG_DINS_FIRE:
        case RG_FARORES_WIND:
        case RG_NAYRUS_LOVE:

        // Elemental arrows
        case RG_FIRE_ARROWS:
        case RG_ICE_ARROWS:
        case RG_LIGHT_ARROWS:

        // Card / beans / defense
        case RG_GERUDO_MEMBERSHIP_CARD:
        case RG_MAGIC_BEAN_PACK:

        // Child/adult trade sequence
        case RG_WEIRD_EGG:
        case RG_ZELDAS_LETTER:
        case RG_POCKET_EGG:
        case RG_COJIRO:
        case RG_ODD_MUSHROOM:
        case RG_ODD_POTION:
        case RG_POACHERS_SAW:
        case RG_BROKEN_SWORD:
        case RG_PRESCRIPTION:
        case RG_EYEBALL_FROG:
        case RG_EYEDROPS:
        case RG_CLAIM_CHECK:

        // Progressive equipment that is *always* major
        case RG_PROGRESSIVE_HOOKSHOT:
        case RG_PROGRESSIVE_STRENGTH:
        case RG_PROGRESSIVE_WALLET:
        case RG_PROGRESSIVE_SCALE:
        case RG_PROGRESSIVE_GORONSWORD:

        // Magic (single/double)
        case RG_MAGIC_SINGLE:
        case RG_MAGIC_DOUBLE:

        // Bottles + contents
        case RG_EMPTY_BOTTLE:
        case RG_BOTTLE_WITH_MILK:
        case RG_BOTTLE_WITH_RED_POTION:
        case RG_BOTTLE_WITH_GREEN_POTION:
        case RG_BOTTLE_WITH_BLUE_POTION:
        case RG_BOTTLE_WITH_FAIRY:
        case RG_BOTTLE_WITH_FISH:
        case RG_BOTTLE_WITH_BLUE_FIRE:
        case RG_BOTTLE_WITH_BUGS:
        case RG_BOTTLE_WITH_POE:
        case RG_RUTOS_LETTER:
        case RG_BOTTLE_WITH_BIG_POE:

        // Ocarinas
        case RG_FAIRY_OCARINA:
        case RG_OCARINA_OF_TIME:

        // Bomb bags
        case RG_BOMB_BAG:
        case RG_BIG_BOMB_BAG:
        case RG_BIGGEST_BOMB_BAG:

        // Bows / quivers
        case RG_FAIRY_BOW:
        case RG_BIG_QUIVER:
        case RG_BIGGEST_QUIVER:

        // Slingshot / bullet bags
        case RG_FAIRY_SLINGSHOT:
        case RG_BIG_BULLET_BAG:
        case RG_BIGGEST_BULLET_BAG:

        // Strength items
        case RG_GORONS_BRACELET:
        case RG_SILVER_GAUNTLETS:
        case RG_GOLDEN_GAUNTLETS:

        // Scales
        case RG_SILVER_SCALE:
        case RG_GOLDEN_SCALE:
        case RG_BRONZE_SCALE:

        // Wallets
        case RG_ADULT_WALLET:
        case RG_GIANT_WALLET:
        case RG_TYCOON_WALLET:
        case RG_CHILD_WALLET:

        // Nut/stick capacity items
        case RG_DEKU_NUT_CAPACITY_30:
        case RG_DEKU_NUT_CAPACITY_40:
        case RG_DEKU_STICK_CAPACITY_20:
        case RG_DEKU_STICK_CAPACITY_30:

        // Extra bags
        case RG_DEKU_STICK_BAG:
        case RG_DEKU_NUT_BAG:

        // Hookshots
        case RG_HOOKSHOT:
        case RG_LONGSHOT:

        // Misc progression
        case RG_SKELETON_KEY:
        case RG_FISHING_POLE:
        case RG_ARCHIPELAGO_ITEM_PROGRESSIVE:

        // Ocarina button “items”
        case RG_OCARINA_A_BUTTON:
        case RG_OCARINA_C_UP_BUTTON:
        case RG_OCARINA_C_DOWN_BUTTON:
        case RG_OCARINA_C_LEFT_BUTTON:
        case RG_OCARINA_C_RIGHT_BUTTON:

        // Glitch abilities
        case RG_ABILITY_ISG:
        case RG_ABILITY_OI:
        case RG_ABILITY_QPA:
        case RG_ABILITY_HESS:
        case RG_ABILITY_SUPERSLIDE:
        case RG_ABILITY_HOVER:
        case RG_ABILITY_EQUIP_SWAP:
        case RG_ABILITY_GROUND_JUMP:
        case RG_ABILITY_WEIRDSHOT:
            return true;

        default:
            return false;
    }
}

static bool IsLesserItem(const GetItemEntry& entry) {
    if (!IsRandomizerEntry(entry)) {
        return false;
    }

    // Special rule: selected progressives become Lesser after the first copy.
    switch (entry.getItemId) {
        case RG_PROGRESSIVE_BOMB_BAG:
        case RG_PROGRESSIVE_BOW:
        case RG_PROGRESSIVE_SLINGSHOT:
        case RG_PROGRESSIVE_NUT_UPGRADE:
        case RG_PROGRESSIVE_STICK_UPGRADE:
        case RG_PROGRESSIVE_MAGIC_METER:
        case RG_PROGRESSIVE_OCARINA:
        case RG_PROGRESSIVE_BOMBCHU_BAG:
            return !IsFirstMajorProgressive(entry);
        default:
            break;
    }

    switch (entry.getItemId) {
        case RG_GORON_TUNIC:
        case RG_ZORA_TUNIC:
        case RG_DEKU_SHIELD:
        case RG_HYLIAN_SHIELD:
        case RG_STONE_OF_AGONY:
        case RG_DOUBLE_DEFENSE:
        case RG_MAGIC_BEAN:

        // Infinite upgrade variants
        case RG_QUIVER_INF:
        case RG_BOMB_BAG_INF:
        case RG_BULLET_BAG_INF:
        case RG_STICK_UPGRADE_INF:
        case RG_NUT_UPGRADE_INF:
        case RG_MAGIC_INF:
        case RG_BOMBCHU_INF:
        case RG_WALLET_INF:

        case RG_ARCHIPELAGO_ITEM_USEFUL:
            return true;

        default:
            return false;
    }
}

static bool IsTriforce(const GetItemEntry& entry) {
    return IsRandomizerEntry(entry) && entry.getItemId == RG_TRIFORCE_PIECE;
}

static bool IsGregRupee(const GetItemEntry& entry) {
    return IsRandomizerEntry(entry) && entry.getItemId == RG_GREG_RUPEE;
}

static bool IsSong(const GetItemEntry& entry) {
    if (!IsRandomizerEntry(entry)) {
        return false;
    }
    switch (entry.getItemId) {
        case RG_ZELDAS_LULLABY:
        case RG_EPONAS_SONG:
        case RG_SARIAS_SONG:
        case RG_SUNS_SONG:
        case RG_SONG_OF_TIME:
        case RG_SONG_OF_STORMS:
        case RG_MINUET_OF_FOREST:
        case RG_BOLERO_OF_FIRE:
        case RG_SERENADE_OF_WATER:
        case RG_REQUIEM_OF_SPIRIT:
        case RG_NOCTURNE_OF_SHADOW:
        case RG_PRELUDE_OF_LIGHT:
            return true;
        default:
            return false;
    }
}

static bool IsDungeonReward(const GetItemEntry& entry) {
    if (!IsRandomizerEntry(entry)) {
        return false;
    }
    switch (entry.getItemId) {
        case RG_KOKIRI_EMERALD:
        case RG_GORON_RUBY:
        case RG_ZORA_SAPPHIRE:
        case RG_FOREST_MEDALLION:
        case RG_FIRE_MEDALLION:
        case RG_WATER_MEDALLION:
        case RG_SPIRIT_MEDALLION:
        case RG_SHADOW_MEDALLION:
        case RG_LIGHT_MEDALLION:
            return true;
        default:
            return false;
    }
}

static bool IsBossSoul(const GetItemEntry& entry) {
    return IsRandomizerEntry(entry) && (entry.getItemId >= RG_GOHMA_SOUL && entry.getItemId <= RG_GANON_SOUL);
}

static bool IsBossKey(const GetItemEntry& entry) {
    return IsRandomizerEntry(entry) &&
           (entry.getItemId >= RG_FOREST_TEMPLE_BOSS_KEY && entry.getItemId <= RG_GANONS_CASTLE_BOSS_KEY);
}

static bool IsDungeonKey(const GetItemEntry& entry) {
    return IsRandomizerEntry(entry) &&
           (entry.getItemId >= RG_FOREST_TEMPLE_SMALL_KEY && entry.getItemId <= RG_TREASURE_GAME_KEY_RING);
}

static bool IsOverworldKey(const GetItemEntry& entry) {
    return IsRandomizerEntry(entry) &&
           (entry.getItemId >= RG_GUARD_HOUSE_KEY && entry.getItemId <= RG_FISHING_HOLE_KEY);
}

static bool IsSkulltulaToken(const GetItemEntry& entry) {
    return IsRandomizerEntry(entry) && entry.getItemId == RG_GOLD_SKULLTULA_TOKEN;
}

static bool IsHeartContainer(const GetItemEntry& entry) {
    return IsRandomizerEntry(entry) && entry.getItemId == RG_HEART_CONTAINER;
}

static bool IsHeartPiece(const GetItemEntry& entry) {
    if (!IsRandomizerEntry(entry)) {
        return false;
    }
    switch (entry.getItemId) {
        case RG_PIECE_OF_HEART:
        case RG_TREASURE_GAME_HEART:
            return true;
        default:
            return false;
    }
}

static bool IsMapOrCompass(const GetItemEntry& entry) {
    if (!IsRandomizerEntry(entry)) {
        return false;
    }
    // Maps
    if (entry.getItemId >= RG_DEKU_TREE_MAP && entry.getItemId <= RG_ICE_CAVERN_MAP) {
        return true;
    }
    // Compasses
    if (entry.getItemId >= RG_DEKU_TREE_COMPASS && entry.getItemId <= RG_ICE_CAVERN_COMPASS) {
        return true;
    }
    return false;
}

// Consumables: rupees, refills, shop trash etc.
// For randomizer entries we treat ITEM_CATEGORY_JUNK as consumables.
static bool IsConsumable(const GetItemEntry& entry) {
    return entry.getItemCategory == ITEM_CATEGORY_JUNK || entry.getItemId == RG_HUGE_RUPEE ||
           entry.getItemId == RG_ARCHIPELAGO_ITEM_JUNK;
}

static bool IsIceTrap(const GetItemEntry& entry) {
    return IsRandomizerEntry(entry) && entry.getItemId == RG_ICE_TRAP;
}

// --------------------------------------------------------
// Category classifier (one and only place that decides
// which bucket an entry belongs to).
// --------------------------------------------------------

static SkipGIAdvancedCategoryIndex ClassifyAdvancedCategory(const GetItemEntry& entry) {
    // Vanilla handling
    if (!IsRandomizerEntry(entry)) {
        // Treat vanilla junk as consumables
        if (IsConsumable(entry)) {
            return SKIPGI_CAT_CONSUMABLES;
        }
        // Everything else defaults to major
        return SKIPGI_CAT_MAJOR_ITEMS;
    }

    if (IsMajorItem(entry))
        return SKIPGI_CAT_MAJOR_ITEMS;
    if (IsLesserItem(entry))
        return SKIPGI_CAT_LESSER_ITEMS;
    if (IsTriforce(entry))
        return SKIPGI_CAT_TRIFORCE;
    if (IsGregRupee(entry))
        return SKIPGI_CAT_GREG_RUPEE; // NEW
    if (IsSong(entry))
        return SKIPGI_CAT_SONGS;
    if (IsDungeonReward(entry))
        return SKIPGI_CAT_DUNGEON_REWARDS;
    if (IsBossSoul(entry))
        return SKIPGI_CAT_BOSS_SOULS;
    if (IsBossKey(entry))
        return SKIPGI_CAT_BOSS_KEYS;
    if (IsDungeonKey(entry))
        return SKIPGI_CAT_DUNGEON_KEYS;
    if (IsOverworldKey(entry))
        return SKIPGI_CAT_OVERWORLD_KEYS;
    if (IsSkulltulaToken(entry))
        return SKIPGI_CAT_SKULLTULA_TOKENS;
    if (IsHeartContainer(entry))
        return SKIPGI_CAT_HEART_CONTAINERS;
    if (IsHeartPiece(entry))
        return SKIPGI_CAT_HEART_PIECES;
    if (IsMapOrCompass(entry))
        return SKIPGI_CAT_MAPS_COMPASSES;
    if (IsConsumable(entry))
        return SKIPGI_CAT_CONSUMABLES;
    if (IsIceTrap(entry))
        return SKIPGI_CAT_ICE_TRAPS;

    return SKIPGI_CAT_MAJOR_ITEMS;
}

// --------------------------------------------------------
// Advanced logic (used when Skip mode == SGIA_ADVANCED)
// --------------------------------------------------------

bool ShouldSkipGetItemAnimationAdvanced(const GetItemEntry& entry) {

    // Ice traps ALWAYS skip
    if (IsIceTrap(entry)) {
        return true;
    }

    // If the base feature is off completely, never skip anything.
    if (!CVarGetInteger(CVAR_SKIP_GI_SIMPLE, 0)) {
        return false;
    }

    const SkipGIAdvancedCategoryIndex cat = ClassifyAdvancedCategory(entry);

    if (cat < 0 || cat >= SKIP_GI_ADVANCED_CATEGORY_COUNT) {
        return false;
    }

    const char* cvarName = skipGIAdvancedCVarList[cat];
    if (!cvarName) {
        return false;
    }

    return CVarGetInteger(cvarName, 0) != 0;
}
