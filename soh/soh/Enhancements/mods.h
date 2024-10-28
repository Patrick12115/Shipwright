#include <stdint.h>

#ifndef MODS_H
#define MODS_H

#ifdef __cplusplus
extern "C" {
#endif

void UpdateDirtPathFixState(int32_t sceneNum);
void UpdateMirrorModeState(int32_t sceneNum);
void UpdateHurtContainerModeState(bool newState);
void PatchToTMedallions();
void PatchCompasses();
void UpdatePermanentHeartLossState();
void UpdateHyperEnemiesState();
void UpdateHyperBossesState();
void InitMods();
void UpdatePatchHand(); 
void UpdatePatchCustomEquipmentDlists();
void PatchOrUnpatch(const char* resource, const char* gfx, const char* dlist1, const char* dlist2, const char* dlist3,
                    const char* alternateDL);

#ifdef __cplusplus
}
#endif

#endif
