#include "CrowdControl.h"
#include "CrowdControlTypes.h"
#include <libultraship/bridge.h>
#include <libultraship/libultraship.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <regex>
#include <imgui.h>
#include <random>
#include <algorithm>
#include <cstdio>
#include <chrono>

#include "soh/OTRGlobals.h"
#include "soh/Notification/Notification.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/SohGui/SohGui.hpp"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
}

static uint64_t NowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static float sCCTimersDesiredWidth = 0.0f;

static void CrowdControl_RegisterHooks();

void CrowdControl::MainThreadTick() {
    PumpPendingRemovals();

    SyncChaosStartup();

    bool shouldRetryQueued = false;
    {
        std::scoped_lock<std::mutex> lock(activeEffectsMutex);
        for (auto* e : activeEffects) {
            if (!e) {
                continue;
            }
            if (e->runOnceWhenPossible) {
                shouldRetryQueued = true;
                break;
            }
            const bool chaosEnabled = (CVarGetInteger(CVAR_REMOTE_CROWD_CONTROL("ChaosEnabled"), 0) != 0);

            // NEW: if chaos is enabled and we have any active effects at all,
            // make sure ProcessActiveEffectsOnce() gets a 1Hz main-thread chance to run.
            if (chaosEnabled && !activeEffects.empty()) {
                shouldRetryQueued = true;
                break;
            }

            // Existing: when chaos is off, but there are paused timed effects,
            // we still need to retry them (at 1Hz) so they can start when possible.
            if (!chaosEnabled && e->timeRemaining > 0 && e->isPaused) {
                shouldRetryQueued = true;
                break;
            }
        }
    }

    if (shouldRetryQueued && gPlayState != nullptr) {
        // IMPORTANT: don't run this every frame when Chaos is off,
        // or started timed effects will "tick" 1000ms per frame.
        static uint64_t sLastRetryMs = 0;
        uint64_t now = NowMs();
        const uint64_t kRetryIntervalMs = 1000;

        if (now - sLastRetryMs >= kRetryIntervalMs) {
            sLastRetryMs = now;
            ProcessActiveEffectsOnce();
        }
    }
}

static void CrowdControl_OnGameFrameUpdate() {
    if (CrowdControl::Instance == nullptr) {
        return;
    }

    // Always tick so pending removals can be pumped even during transitions.
    CrowdControl::Instance->MainThreadTick();
}

static void CrowdControl_OnPlayDestroy() {
    if (CrowdControl::Instance == nullptr) {
        return;
    }

    // Safe to flush pending removals during teardown
    CrowdControl::Instance->MainThreadTick();
}

static void CrowdControl_RegisterHooks() {
    static HOOK_ID sGameFrameHookID = 0;
    static HOOK_ID sPlayDestroyHookID = 0;
    static bool sRegistered = false;

    // Register hooks whenever chaos OR remote CC is enabled.
    // (This keeps things consistent even if the menu is not visible.)
    const bool chaosEnabled = (CVarGetInteger(CVAR_REMOTE_CROWD_CONTROL("ChaosEnabled"), 0) != 0);
    const bool remoteEnabled = (CVarGetInteger(CVAR_REMOTE_CROWD_CONTROL("Enabled"), 0) != 0);
    const bool shouldBeRegistered =
        (chaosEnabled || remoteEnabled || (CrowdControl::Instance && CrowdControl::Instance->HasPendingWork()));

    if (shouldBeRegistered == sRegistered) {
        return;
    }

    GameInteractor::Instance->UnregisterGameHook<GameInteractor::OnGameFrameUpdate>(sGameFrameHookID);
    GameInteractor::Instance->UnregisterGameHook<GameInteractor::OnPlayDestroy>(sPlayDestroyHookID);
    sGameFrameHookID = 0;
    sPlayDestroyHookID = 0;
    sRegistered = false;

    if (!shouldBeRegistered) {
        return;
    }

    sRegistered = true;

    sGameFrameHookID =
        GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(CrowdControl_OnGameFrameUpdate);

    sPlayDestroyHookID =
        GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>(CrowdControl_OnPlayDestroy);
}

void CrowdControl::Enable() {
    Network::Enable(CVarGetString(CVAR_REMOTE_CROWD_CONTROL("Host"), "127.0.0.1"),
                    CVarGetInteger(CVAR_REMOTE_CROWD_CONTROL("Port"), 43384));

    // Ensure main-thread hook is installed when remote CC is enabled.
    CrowdControl_RegisterHooks();
}

void CrowdControl::OnConnected() {
    ccThreadProcess = std::thread(&CrowdControl::ProcessActiveEffects, this);

    // In case someone flips cvars outside the UI, keep hooks in sync.
    CrowdControl_RegisterHooks();
}

void CrowdControl::OnDisconnected() {
    ccThreadProcess.join();
}

bool CrowdControl::HasPendingWork() {
    // Used by hook registration to keep main-thread cleanup alive even when Chaos is toggled off.
    std::scoped_lock<std::mutex, std::mutex> lock(pendingRemovalsMutex, activeEffectsMutex);
    return !pendingRemovals.empty() || !activeEffects.empty();
}

void CrowdControl::OnIncomingJson(nlohmann::json payload) {
    Effect* incomingEffect = ParseMessage(payload);
    if (!incomingEffect) {
        return;
    }

    // If effect is not a timed effect, execute and return result.
    if (!incomingEffect->timeRemaining) {
        EffectResult result = CrowdControl::ExecuteEffect(incomingEffect);
        EmitMessage(incomingEffect->id, incomingEffect->timeRemaining, result);
        delete incomingEffect;
    } else {
        // If another timed effect is already active that conflicts with the incoming effect.
        bool isConflictingEffectActive = false;
        {
            std::scoped_lock<std::mutex> lock(activeEffectsMutex);
            for (Effect* effect : activeEffects) {
                if (effect != incomingEffect && effect->category == incomingEffect->category &&
                    effect->id < incomingEffect->id) {
                    isConflictingEffectActive = true;
                    EmitMessage(incomingEffect->id, incomingEffect->timeRemaining, EffectResult::Retry);
                    break;
                }
            }
        }

        if (isConflictingEffectActive) {
            delete incomingEffect;
            return;
        }

        // Check if effect can be applied, if it can't, let CC know.
        EffectResult result = CrowdControl::CanApplyEffect(incomingEffect);
        if (result == EffectResult::Retry || result == EffectResult::Failure) {
            EmitMessage(incomingEffect->id, incomingEffect->timeRemaining, result);
            delete incomingEffect;
            return;
        }

        {
            std::scoped_lock<std::mutex> lock(activeEffectsMutex);
            activeEffects.push_back(incomingEffect);
        }
    }
}

void CrowdControl::ProcessActiveEffectsOnce() {
    std::scoped_lock<std::mutex> lock(activeEffectsMutex);

    // When remote CC is connected, the background ProcessActiveEffects() thread owns NON-CHAOS timed effects.
    // ProcessActiveEffectsOnce() should only handle queued one-shot effects and Chaos-owned timed effects.
    const bool remoteThreadRunning = isConnected;
    const bool chaosEnabled = (CVarGetInteger(CVAR_REMOTE_CROWD_CONTROL("ChaosEnabled"), 0) != 0);

    // Defer resuming a started Chaos effect for exactly one tick after it becomes unblocked,
    // to avoid a 1-second "on -> off -> on" flicker when the ended remote effect's Remove()
    // arrives slightly late.
    static std::unordered_set<uint32_t> sDeferResumeOnce;

    auto it = activeEffects.begin();
    while (it != activeEffects.end()) {
        Effect* effect = *it;
        if (!effect) {
            it = activeEffects.erase(it);
            continue;
        }

        // If remote CC thread is running, do not interfere with non-Chaos timed effects at all.
        if (remoteThreadRunning && effect->timeRemaining > 0 && !effect->isChaos) {
            ++it;
            continue;
        }

        // If another older timed effect in the same category exists, keep this queued.
        if (effect->timeRemaining > 0) {
            bool blocked = false;
            for (Effect* other : activeEffects) {
                if (!other || other == effect) {
                    continue;
                }
                if (other->timeRemaining > 0 && other->category == effect->category && other->id < effect->id) {
                    blocked = true;
                    break;
                }
            }

            // If a previous effect in this category just ended and is waiting for main-thread removal,
            // do not start the next one yet. Otherwise the late removal can undo the newly-started effect.
            if (!blocked) {
                std::scoped_lock<std::mutex> rlock(pendingRemovalsMutex);
                for (auto& [r, cat] : pendingRemovals) {
                    if (cat == effect->category) {
                        blocked = true;
                        break;
                    }
                }
            }

            if (blocked) {
                // If it's blocked, clear any pending one-tick defer state so we don't "consume" it early.
                sDeferResumeOnce.erase(effect->id);

                if (!effect->isPaused) {
                    effect->isPaused = true;
                    EmitMessage(effect->id, effect->timeRemaining, EffectResult::Paused);
                }
                ++it;
                continue;
            }

            // Not blocked. If we are about to resume a previously-started timed effect while the remote
            // thread is running, defer exactly one tick so any late Remove() can land first.
            if (remoteThreadRunning) {
                const bool started = (effect->lastExecutionResult == EffectResult::Success);

                if (started && effect->isPaused) {
                    if (sDeferResumeOnce.find(effect->id) == sDeferResumeOnce.end()) {
                        sDeferResumeOnce.insert(effect->id);
                        ++it;
                        continue;
                    } else {
                        sDeferResumeOnce.erase(effect->id);
                    }
                } else {
                    // Keep the set clean for non-resume cases.
                    sDeferResumeOnce.erase(effect->id);
                }
            } else {
                // If remote thread isn't running, we don't need this guard.
                sDeferResumeOnce.erase(effect->id);
            }
        } else {
            // Non-timed effects shouldn't participate in this guard.
            sDeferResumeOnce.erase(effect->id);
        }

        // Queued non-timed effects: keep retrying until Success, then remove immediately.
        // No ticking, no pause/resume semantics, and crucially: no re-firing multiple times.
        if (effect->runOnceWhenPossible) {
            EffectResult result = CrowdControl::ExecuteEffect(effect);
            if (result == EffectResult::Success) {
                it = activeEffects.erase(it);
                delete effect;
                continue;
            }
            // Still not possible: keep it queued, try again next tick.
            ++it;
            continue;
        }

        // Timed effects below this point are Chaos-owned (or offline).
        if (effect->timeRemaining > 0) {
            const bool started = (effect->lastExecutionResult == EffectResult::Success);

            // If the timed effect has already started:
            if ((chaosEnabled || effect->isChaos) && started) {
                // IMPORTANT: keep Chaos-owned timed effects "alive" by executing them every tick,
                // the same way the remote CC thread does. If another effect's Remove() undid the
                // modifier, this re-applies it.
                EffectResult r = CrowdControl::ExecuteEffect(effect);

                if (r == EffectResult::Success) {
                    if (effect->isPaused) {
                        effect->isPaused = false;
                        EmitMessage(effect->id, effect->timeRemaining, EffectResult::Resumed);
                    }

                    // Only tick time when we successfully applied this tick.
                    effect->timeRemaining -= 1000;

                    if (effect->timeRemaining <= 0) {
                        it = activeEffects.erase(it);

                        if (auto* removable = dynamic_cast<RemovableGameInteractionEffect*>(effect->giEffect)) {
                            std::scoped_lock<std::mutex> rlock(pendingRemovalsMutex);
                            pendingRemovals.push_back({ removable, effect->category });
                        }

                        delete effect;
                        continue;
                    }

                    ++it;
                    continue;
                }

                if (r == EffectResult::Retry) {
                    // Can't apply right now; pause and DO NOT tick time.
                    if (!effect->isPaused) {
                        effect->isPaused = true;
                        EmitMessage(effect->id, effect->timeRemaining, EffectResult::Paused);
                    }
                    ++it;
                    continue;
                }

                // Failure: drop it.
                it = activeEffects.erase(it);
                delete effect;
                continue;
            }

            // Not started yet: attempt to start once.
            EffectResult result = CrowdControl::ExecuteEffect(effect);

            if (result == EffectResult::Success) {
                effect->lastExecutionResult = EffectResult::Success;

                // If we have a success after previously being paused, tell CC to resume timer.
                if (effect->isPaused) {
                    effect->isPaused = false;
                    EmitMessage(effect->id, effect->timeRemaining, EffectResult::Resumed);
                } else {
                    EmitMessage(effect->id, effect->timeRemaining, EffectResult::Success);
                }

                ++it;
                continue;
            }

            if (result == EffectResult::Failure) {
                it = activeEffects.erase(it);
                delete effect;
                continue;
            }

            // Retry: stay queued/paused.
            if (!effect->isPaused) {
                effect->isPaused = true;
                EmitMessage(effect->id, effect->timeRemaining, EffectResult::Paused);
            }
            ++it;
            continue;
        }

        // Nothing to do for zero-time effects that aren't runOnceWhenPossible.
        ++it;
    }
}

void CrowdControl::ProcessActiveEffects() {
    while (isEnabled) {
        // We only want to send events when status changes, on start we send Success.
        // If it fails at some point, we send Pause, and when it starts to succeed again we send Success.
        // CC uses this to pause the timer on the overlay.
        activeEffectsMutex.lock();
        auto it = activeEffects.begin();

        while (it != activeEffects.end()) {
            Effect* effect = *it;

            // NEW: Remote CC thread must NEVER drive Chaos-owned effects
            if (effect && effect->isChaos) {
                ++it;
                continue;
            }
            EffectResult result = CrowdControl::ExecuteEffect(effect);

            if (result == EffectResult::Success) {
                // If time remaining has reached 0, we have finished the effect.
                if (effect->timeRemaining <= 0) {
                    it = activeEffects.erase(it);

                    // IMPORTANT: don't call RemoveEffect(nullptr). Enqueue main-thread removal if removable.
                    if (auto* removable = dynamic_cast<RemovableGameInteractionEffect*>(effect->giEffect)) {
                        std::scoped_lock<std::mutex> rlock(pendingRemovalsMutex);
                        pendingRemovals.push_back({ removable, effect->category });
                    }

                    delete effect;
                } else {
                    // If we have a success after previously being paused, tell CC to resume timer.
                    if (effect->isPaused) {
                        effect->isPaused = false;
                        EmitMessage(effect->id, effect->timeRemaining, EffectResult::Resumed);
                        // If not paused before, subtract time from the timer and send a Success event if
                        // the result is different from the last time this was ran.
                        // Timed events are put on a thread that runs once per second.
                    } else {
                        effect->timeRemaining -= 1000;
                        if (result != effect->lastExecutionResult) {
                            effect->lastExecutionResult = result;
                            EmitMessage(effect->id, effect->timeRemaining, EffectResult::Success);
                        }
                    }
                    it++;
                }
            } else { // Timed effects only do Success or Retry
                if (!effect->isPaused && effect->timeRemaining > 0) {
                    effect->isPaused = true;
                    EmitMessage(effect->id, effect->timeRemaining, EffectResult::Paused);
                }
                it++;
            }
        }

        activeEffectsMutex.unlock();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    SPDLOG_TRACE("[CrowdControl] Ending Process thread...");
}

void CrowdControl::EmitMessage(uint32_t eventId, long timeRemaining, EffectResult status) {
    if (!isConnected) {
        return;
    }

    nlohmann::json payload;

    payload["id"] = eventId;
    payload["type"] = 0;
    payload["timeRemaining"] = timeRemaining;
    payload["status"] = status;

    SPDLOG_INFO("[CrowdControl] Sending payload:\n{}", payload.dump());

    SendJsonToRemote(payload);
}

CrowdControl::EffectResult CrowdControl::ExecuteEffect(Effect* effect) {
    GameInteractionEffectQueryResult giResult;
    if (effect->category == kEffectCatSpawnEnemy) {
        giResult = GameInteractor::RawAction::SpawnEnemyWithOffset(effect->spawnParams[0], effect->spawnParams[1],
                                                                   effect->viewerName);
    } else if (effect->category == kEffectCatSpawnActor) {
        giResult =
            GameInteractor::RawAction::SpawnActor(effect->spawnParams[0], effect->spawnParams[1], effect->viewerName);
    } else {
        giResult = GameInteractor::ApplyEffect(effect->giEffect);
    }

    return TranslateGiEnum(giResult);
}

// Checks if effect can be applied -- should not be used to check for spawn enemy effects.
CrowdControl::EffectResult CrowdControl::CanApplyEffect(Effect* effect) {
    // FIX: this was using || which makes it basically always true.
    assert(effect->category != kEffectCatSpawnEnemy && effect->category != kEffectCatSpawnActor);

    if (gPlayState == nullptr || GET_PLAYER(gPlayState) == nullptr) {
        return EffectResult::Retry;
    }

    GameInteractionEffectQueryResult giResult = GameInteractor::CanApplyEffect(effect->giEffect);
    return TranslateGiEnum(giResult);
}

CrowdControl::EffectResult CrowdControl::TranslateGiEnum(GameInteractionEffectQueryResult giResult) {
    // Translate GameInteractor result into CC's own enums.
    EffectResult result;
    if (giResult == GameInteractionEffectQueryResult::Possible) {
        result = EffectResult::Success;
    } else if (giResult == GameInteractionEffectQueryResult::TemporarilyNotPossible) {
        result = EffectResult::Retry;
    } else {
        result = EffectResult::Failure;
    }

    return result;
}

// Call this from MAIN THREAD once per frame (best), or at least whenever you know you're on main thread.
// Do NOT call from the chaos thread.
void CrowdControl::PumpPendingRemovals() {
    if (gPlayState == nullptr) {
        return;
    }

    std::vector<std::pair<RemovableGameInteractionEffect*, uint32_t>> local;
    {
        std::scoped_lock<std::mutex> lock(pendingRemovalsMutex);
        local.swap(pendingRemovals);
    }

    for (auto& [r, cat] : local) {
        if (r) {
            GameInteractor::RemoveEffect(r);
        }
    }
}

CrowdControl::Effect* CrowdControl::ParseMessage(nlohmann::json dataReceived) {
    if (!dataReceived.contains("id") || !dataReceived.contains("type")) {
        SPDLOG_ERROR("[CrowdControl] Invalid payload received:\n{}", dataReceived.dump());
        return nullptr;
    }

    SPDLOG_INFO("[CrowdControl] Received payload:\n{}", dataReceived.dump());

    if (!dataReceived.contains("code")) {
        // This seems to happen when the CC session ends
        SPDLOG_ERROR("[CrowdControl] Payload does not contain code, ignoring.");
        return nullptr;
    }

    Effect* effect = new Effect();
    effect->lastExecutionResult = EffectResult::Initiate;
    effect->id = dataReceived["id"];
    effect->viewerName = dataReceived["viewer"];
    auto parameters = dataReceived["parameters"];
    uint32_t receivedParameter = 0;
    auto effectName = dataReceived["code"].get<std::string>();

    // NEW: store code + default display name (can be upgraded to uiName later)
    effect->effectCode = effectName;
    effect->displayName = effectName;

    if (parameters.size() > 0) {
        receivedParameter = dataReceived["parameters"][0];
    }

    // -------------------------------------------------------------------------
    // Duration CVAR helper (seconds -> ms), NO clamp (except negative -> 0)
    // -------------------------------------------------------------------------
    auto GetDurationMs = [&](int defaultSeconds) -> long {
        const std::string key = std::string(CVAR_REMOTE_CROWD_CONTROL("ChaosEffectDuration.")) + effectName;
        int sec = CVarGetInteger(key.c_str(), defaultSeconds);
        if (sec < 0) {
            sec = 0;
        }
        return (long)sec * 1000L;
    };

    // Assign GameInteractionEffect + values to CC effect.
    // Categories are mostly used for checking for conflicting timed effects.
    switch (effectStringToEnum[effectName]) {

        // Spawn Enemies and Objects
        case kEffectSpawnCuccoStorm:
            effect->spawnParams[0] = ACTOR_EN_NIW;
            effect->category = kEffectCatSpawnActor;
            break;
        case kEffectSpawnLitBomb:
            effect->spawnParams[0] = ACTOR_EN_BOM;
            effect->category = kEffectCatSpawnActor;
            break;
        case kEffectSpawnExplosion:
            effect->spawnParams[0] = ACTOR_EN_BOM;
            effect->spawnParams[1] = 1;
            effect->category = kEffectCatSpawnActor;
            break;
        case kEffectSpawnArwing:
            effect->spawnParams[0] = ACTOR_EN_CLEAR_TAG;
            // Parameter for no cutscene Arwing
            effect->spawnParams[1] = 1;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnDarklink:
            effect->spawnParams[0] = ACTOR_EN_TORCH2;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnIronKnuckle:
            effect->spawnParams[0] = ACTOR_EN_IK;
            // Parameter for black standing Iron Knuckle
            effect->spawnParams[1] = 2;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnStalfos:
            effect->spawnParams[0] = ACTOR_EN_TEST;
            // Parameter for gravity-obeying Stalfos
            effect->spawnParams[1] = 2;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnFreezard:
            effect->spawnParams[0] = ACTOR_EN_FZ;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnLikeLike:
            effect->spawnParams[0] = ACTOR_EN_RR;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnGibdo:
            effect->spawnParams[0] = ACTOR_EN_RD;
            // Parameter for Gibdo
            effect->spawnParams[1] = 32766;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnKeese:
            effect->spawnParams[0] = ACTOR_EN_FIREFLY;
            // Parameter for normal keese
            effect->spawnParams[1] = 2;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnIceKeese:
            effect->spawnParams[0] = ACTOR_EN_FIREFLY;
            // Parameter for ice keese
            effect->spawnParams[1] = 4;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnFireKeese:
            effect->spawnParams[0] = ACTOR_EN_FIREFLY;
            // Parameter for fire keese
            effect->spawnParams[1] = 1;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnWolfos:
            effect->spawnParams[0] = ACTOR_EN_WF;
            effect->category = kEffectCatSpawnEnemy;
            break;
        case kEffectSpawnWallmaster:
            effect->spawnParams[0] = ACTOR_EN_WALLMAS;
            effect->category = kEffectCatSpawnEnemy;
            break;

        // Link Modifiers
        case kEffectTakeHalfDamage:
            effect->category = kEffectCatDamageTaken;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ModifyDefenseModifier();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = 2;
            break;
        case kEffectTakeDoubleDamage:
            effect->category = kEffectCatDamageTaken;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ModifyDefenseModifier();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = -2;
            break;
        case kEffectOneHitKo:
            effect->category = kEffectCatDamageTaken;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::OneHitKO();
            break;
        case kEffectInvincibility:
            effect->category = kEffectCatDamageTaken;
            effect->timeRemaining = GetDurationMs(15);
            effect->giEffect = new GameInteractionEffect::PlayerInvincibility();
            break;
        case kEffectIncreaseSpeed:
            effect->category = kEffectCatSpeed;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ModifyMovementSpeedMultiplier();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = 2;
            break;
        case kEffectDecreaseSpeed:
            effect->category = kEffectCatSpeed;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ModifyMovementSpeedMultiplier();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = -2;
            break;
        case kEffectLowGravity:
            effect->category = kEffectCatGravity;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ModifyGravity();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_GRAVITY_LEVEL_LIGHT;
            break;
        case kEffectHighGravity:
            effect->category = kEffectCatGravity;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ModifyGravity();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_GRAVITY_LEVEL_HEAVY;
            break;
        case kEffectForceIronBoots:
            effect->category = kEffectCatBoots;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ForceEquipBoots();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = EQUIP_VALUE_BOOTS_IRON;
            break;
        case kEffectForceHoverBoots:
            effect->category = kEffectCatBoots;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ForceEquipBoots();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] =
                EQUIP_VALUE_BOOTS_HOVER;
            break;
        case kEffectSlipperyFloor:
            effect->category = kEffectCatSlipperyFloor;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::SlipperyFloor();
            break;
        case kEffectNoLedgeGrabs:
            effect->category = kEffectCatNoLedgeGrabs;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::DisableLedgeGrabs();
            break;
        case kEffectRandomWind:
            effect->category = kEffectCatRandomWind;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::RandomWind();
            break;
        case kEffectRandomBonks:
            effect->category = kEffectCatRandomBonks;
            effect->timeRemaining = GetDurationMs(60);
            effect->giEffect = new GameInteractionEffect::RandomBonks();
            break;

        // Hurt or Heal Link
        case kEffectEmptyHeart:
            effect->giEffect = new GameInteractionEffect::ModifyHealth();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter * -1;
            break;
        case kEffectFillHeart:
            effect->giEffect = new GameInteractionEffect::ModifyHealth();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter;
            break;
        case kEffectKnockbackLinkWeak:
            effect->giEffect = new GameInteractionEffect::KnockbackPlayer();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = 1;
            break;
        case kEffectKnockbackLinkStrong:
            effect->giEffect = new GameInteractionEffect::KnockbackPlayer();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = 3;
            break;
        case kEffectKnockbackLinkMega:
            effect->giEffect = new GameInteractionEffect::KnockbackPlayer();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = 6;
            break;
        case kEffectBurnLink:
            effect->giEffect = new GameInteractionEffect::BurnPlayer();
            break;
        case kEffectFreezeLink:
            effect->giEffect = new GameInteractionEffect::FreezePlayer();
            break;
        case kEffectElectrocuteLink:
            effect->giEffect = new GameInteractionEffect::ElectrocutePlayer();
            break;
        case kEffectKillLink:
            effect->giEffect = new GameInteractionEffect::SetPlayerHealth();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = 0;
            break;

        // Give Items and Consumables
        case kEffectAddHeartContainer:
            effect->giEffect = new GameInteractionEffect::ModifyHeartContainers();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = 1;
            break;
        case kEffectFillMagic:
            effect->giEffect = new GameInteractionEffect::FillMagic();
            break;
        case kEffectAddRupees:
            effect->giEffect = new GameInteractionEffect::ModifyRupees();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter;
            break;
        case kEffectGiveDekuShield:
            effect->giEffect = new GameInteractionEffect::GiveOrTakeShield();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = ITEM_SHIELD_DEKU;
            break;
        case kEffectGiveHylianShield:
            effect->giEffect = new GameInteractionEffect::GiveOrTakeShield();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = ITEM_SHIELD_HYLIAN;
            break;
        case kEffectRefillSticks:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_STICK;
            break;
        case kEffectRefillNuts:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_NUT;
            break;
        case kEffectRefillBombs:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_BOMB;
            break;
        case kEffectRefillSeeds:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_SLINGSHOT;
            break;
        case kEffectRefillArrows:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_BOW;
            break;
        case kEffectRefillBombchus:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_BOMBCHU;
            break;

        // Take Items and Consumables
        case kEffectRemoveHeartContainer:
            effect->giEffect = new GameInteractionEffect::ModifyHeartContainers();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = -1;
            break;
        case kEffectEmptyMagic:
            effect->giEffect = new GameInteractionEffect::EmptyMagic();
            break;
        case kEffectRemoveRupees:
            effect->giEffect = new GameInteractionEffect::ModifyRupees();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter * -1;
            break;
        case kEffectTakeDekuShield:
            effect->giEffect = new GameInteractionEffect::GiveOrTakeShield();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = -ITEM_SHIELD_DEKU;
            break;
        case kEffectTakeHylianShield:
            effect->giEffect = new GameInteractionEffect::GiveOrTakeShield();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = -ITEM_SHIELD_HYLIAN;
            break;
        case kEffectTakeSticks:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter * -1;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_STICK;
            break;
        case kEffectTakeNuts:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter * -1;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_NUT;
            break;
        case kEffectTakeBombs:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter * -1;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_BOMB;
            break;
        case kEffectTakeSeeds:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter * -1;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_SLINGSHOT;
            break;
        case kEffectTakeArrows:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter * -1;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_BOW;
            break;
        case kEffectTakeBombchus:
            effect->giEffect = new GameInteractionEffect::AddOrTakeAmmo();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = receivedParameter * -1;
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[1] = ITEM_BOMBCHU;
            break;

        // Link Size Modifiers
        case kEffectGiantLink:
            effect->category = kEffectCatLinkSize;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ModifyLinkSize();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_LINK_SIZE_GIANT;
            break;
        case kEffectMinishLink:
            effect->category = kEffectCatLinkSize;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ModifyLinkSize();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_LINK_SIZE_MINISH;
            break;
        case kEffectPaperLink:
            effect->category = kEffectCatLinkSize;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ModifyLinkSize();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_LINK_SIZE_PAPER;
            break;
        case kEffectSquishedLink:
            effect->category = kEffectCatLinkSize;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::ModifyLinkSize();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_LINK_SIZE_SQUISHED;
            break;
        case kEffectInvisibleLink:
            effect->category = kEffectCatLinkSize;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::InvisibleLink();
            break;

        // Generic Effects
        case kEffectRandomBombTimer:
            effect->category = kEffectCatRandomBombFuseTimer;
            effect->timeRemaining = GetDurationMs(60);
            effect->giEffect = new GameInteractionEffect::RandomBombFuseTimer();
            break;
        case kEffectSetTimeToDawn:
            effect->giEffect = new GameInteractionEffect::SetTimeOfDay();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_TIMEOFDAY_DAWN;
            break;
        case kEffectSetTimeToDusk:
            effect->giEffect = new GameInteractionEffect::SetTimeOfDay();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_TIMEOFDAY_DUSK;
            break;

        // Visual Effects
        case kEffectNoUi:
            effect->category = kEffectCatUi;
            effect->timeRemaining = GetDurationMs(60);
            effect->giEffect = new GameInteractionEffect::NoUI();
            break;
        case kEffectRainstorm:
            effect->category = kEffectCatWeather;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::WeatherRainstorm();
            break;
        case kEffectDebugMode:
            effect->category = kEffectCatDebugMode;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::SetCollisionViewer();
            break;
        case kEffectRandomCosmetics:
            effect->giEffect = new GameInteractionEffect::RandomizeCosmetics();
            break;

        // Controls
        case kEffectNoZButton:
            effect->category = kEffectCatNoZ;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::DisableZTargeting();
            break;
        case kEffectReverseControls:
            effect->category = kEffectCatReverseControls;
            effect->timeRemaining = GetDurationMs(60);
            effect->giEffect = new GameInteractionEffect::ReverseControls();
            break;
        case kEffectPacifistMode:
            effect->category = kEffectCatPacifist;
            effect->timeRemaining = GetDurationMs(15);
            effect->giEffect = new GameInteractionEffect::PacifistMode();
            break;
        case kEffectPressRandomButtons:
            effect->category = kEffectCatRandomButtons;
            effect->timeRemaining = GetDurationMs(30);
            effect->giEffect = new GameInteractionEffect::PressRandomButton();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = 30;
            break;
        case kEffectClearCbuttons:
            effect->giEffect = new GameInteractionEffect::ClearAssignedButtons();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_BUTTONS_CBUTTONS;
            break;
        case kEffectClearDpad:
            effect->giEffect = new GameInteractionEffect::ClearAssignedButtons();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_BUTTONS_DPAD;
            break;

        // Teleport Player
        case kEffectTpLinksHouse:
            effect->giEffect = new GameInteractionEffect::TeleportPlayer();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_TP_DEST_LINKSHOUSE;
            break;
        case kEffectTpMinuet:
            effect->giEffect = new GameInteractionEffect::TeleportPlayer();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_TP_DEST_MINUET;
            break;
        case kEffectTpBolero:
            effect->giEffect = new GameInteractionEffect::TeleportPlayer();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_TP_DEST_BOLERO;
            break;
        case kEffectTpSerenade:
            effect->giEffect = new GameInteractionEffect::TeleportPlayer();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_TP_DEST_SERENADE;
            break;
        case kEffectTpRequiem:
            effect->giEffect = new GameInteractionEffect::TeleportPlayer();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_TP_DEST_REQUIEM;
            break;
        case kEffectTpNocturne:
            effect->giEffect = new GameInteractionEffect::TeleportPlayer();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_TP_DEST_NOCTURNE;
            break;
        case kEffectTpPrelude:
            effect->giEffect = new GameInteractionEffect::TeleportPlayer();
            dynamic_cast<ParameterizedGameInteractionEffect*>(effect->giEffect)->parameters[0] = GI_TP_DEST_PRELUDE;
            break;

        default:
            break;
    }

    return effect;
}

// Local (offline) chaos CVARs
#define CVAR_CC_CHAOS_ENABLED CVAR_REMOTE_CROWD_CONTROL("ChaosEnabled")
#define CVAR_CC_CHAOS_MIN_SECONDS CVAR_REMOTE_CROWD_CONTROL("ChaosMinSeconds")
#define CVAR_CC_CHAOS_MAX_SECONDS CVAR_REMOTE_CROWD_CONTROL("ChaosMaxSeconds")
#define CVAR_CC_CHAOS_NOTIFY CVAR_REMOTE_CROWD_CONTROL("ChaosShowNotifications")
// 0 = Auto, 1 = Up, 2 = Down
#define CVAR_CC_TIMERS_GROW_DIR CVAR_REMOTE_CROWD_CONTROL("TimersGrowDir")

// Per-effect keys:
//   Remote.CrowdControl.ChaosEffectEnabled.<effectCode>
//   Remote.CrowdControl.ChaosEffectWeight.<effectCode>
//   Remote.CrowdControl.ChaosEffectDuration.<effectCode>
static std::string CCChaosEffectEnabledKey(const std::string& effectCode) {
    return std::string(CVAR_REMOTE_CROWD_CONTROL("ChaosEffectEnabled.")) + effectCode;
}
static std::string CCChaosEffectWeightKey(const std::string& effectCode) {
    return std::string(CVAR_REMOTE_CROWD_CONTROL("ChaosEffectWeight.")) + effectCode;
}
static std::string CCChaosEffectDurationKey(const std::string& effectCode) {
    return std::string(CVAR_REMOTE_CROWD_CONTROL("ChaosEffectDuration.")) + effectCode;
}

// Read duration in seconds, do NOT clamp (except negative -> 0). Return ms.
static long CC_GetEffectDurationMsForCode(const std::string& effectCode, int defaultSeconds) {
    const std::string key = CCChaosEffectDurationKey(effectCode);
    int sec = CVarGetInteger(key.c_str(), defaultSeconds);
    if (sec < 0) {
        sec = 0;
    }
    return (long)sec * 1000L;
}

enum class ChaosUiGroup {
    Spawn,
    LinkModifiers,
    HurtHeal,
    GiveItems,
    TakeItems,
    LinkSize,
    Generic,
    Visual,
    Controls,
    Teleport,
};

static const char* ChaosGroupName(ChaosUiGroup g) {
    switch (g) {
        case ChaosUiGroup::Spawn:
            return "Spawn Enemies and Objects";
        case ChaosUiGroup::LinkModifiers:
            return "Link Modifiers";
        case ChaosUiGroup::HurtHeal:
            return "Hurt or Heal Link";
        case ChaosUiGroup::GiveItems:
            return "Give Items and Consumables";
        case ChaosUiGroup::TakeItems:
            return "Take Items and Consumables";
        case ChaosUiGroup::LinkSize:
            return "Link Size Modifiers";
        case ChaosUiGroup::Generic:
            return "Generic Effects";
        case ChaosUiGroup::Visual:
            return "Visual Effects";
        case ChaosUiGroup::Controls:
            return "Controls";
        case ChaosUiGroup::Teleport:
            return "Teleport Player";
        default:
            return "Unknown";
    }
}

struct ChaosEffectDef {
    const char* uiName;
    const char* code;
    ChaosUiGroup group;
    CCCatEnumValues category;
    int defaultWeight;
    bool defaultEnabled;
    bool hasParameter;
    int defaultParameter;
    int defaultDurationSeconds;
};

// Keep this list in sync with ParseMessage()'s supported effect codes.
static const std::vector<ChaosEffectDef> kChaosEffects = {
    // Spawn Enemies and Objects (non-timed)
    { "Cucco Storm", "spawn_cucco_storm", ChaosUiGroup::Spawn, kEffectCatSpawnActor, 50, true, false, 0, 0 },
    { "Lit Bomb", "spawn_lit_bomb", ChaosUiGroup::Spawn, kEffectCatSpawnActor, 60, true, false, 0, 0 },
    { "Explosion", "spawn_explosion", ChaosUiGroup::Spawn, kEffectCatSpawnActor, 35, true, false, 0, 0 },
    { "Arwing", "spawn_arwing", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 20, true, false, 0, 0 },
    { "Dark Link", "spawn_darklink", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 15, true, false, 0, 0 },
    { "Iron Knuckle", "spawn_iron_knuckle", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 10, true, false, 0, 0 },
    { "Stalfos", "spawn_stalfos", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 25, true, false, 0, 0 },
    { "Freezard", "spawn_freezard", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 25, true, false, 0, 0 },
    { "Like-Like", "spawn_like_like", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 25, true, false, 0, 0 },
    { "Gibdo", "spawn_gibdo", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 20, true, false, 0, 0 },
    { "Keese", "spawn_keese", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 25, true, false, 0, 0 },
    { "Ice Keese", "spawn_ice_keese", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 35, true, false, 0, 0 },
    { "Fire Keese", "spawn_fire_keese", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 35, true, false, 0, 0 },
    { "Wolfos", "spawn_wolfos", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 35, true, false, 0, 0 },
    { "Wallmaster", "spawn_wallmaster", ChaosUiGroup::Spawn, kEffectCatSpawnEnemy, 15, true, false, 0, 0 },

    // Link Modifiers (timed)
    { "Take Half Damage", "take_half_damage", ChaosUiGroup::LinkModifiers, kEffectCatDamageTaken, 20, true, false, 0,
      30 },
    { "Take Double Damage", "take_double_damage", ChaosUiGroup::LinkModifiers, kEffectCatDamageTaken, 25, true, false,
      0, 30 },
    { "One Hit KO", "one_hit_ko", ChaosUiGroup::LinkModifiers, kEffectCatDamageTaken, 8, true, false, 0, 30 },
    { "Invincibility", "invincibility", ChaosUiGroup::LinkModifiers, kEffectCatDamageTaken, 15, true, false, 0, 15 },
    { "Increase Speed", "increase_speed", ChaosUiGroup::LinkModifiers, kEffectCatSpeed, 20, true, false, 0, 30 },
    { "Decrease Speed", "decrease_speed", ChaosUiGroup::LinkModifiers, kEffectCatSpeed, 20, true, false, 0, 30 },
    { "Low Gravity", "low_gravity", ChaosUiGroup::LinkModifiers, kEffectCatGravity, 20, true, false, 0, 30 },
    { "High Gravity", "high_gravity", ChaosUiGroup::LinkModifiers, kEffectCatGravity, 20, true, false, 0, 30 },
    { "Force Iron Boots", "force_iron_boots", ChaosUiGroup::LinkModifiers, kEffectCatBoots, 15, true, false, 0, 30 },
    { "Force Hover Boots", "force_hover_boots", ChaosUiGroup::LinkModifiers, kEffectCatBoots, 15, true, false, 0, 30 },
    { "Slippery Floor", "slippery_floor", ChaosUiGroup::LinkModifiers, kEffectCatSlipperyFloor, 20, true, false, 0,
      30 },
    { "Disable Ledge Grabs", "no_ledge_grabs", ChaosUiGroup::LinkModifiers, kEffectCatNoLedgeGrabs, 15, true, false, 0,
      30 },
    { "Random Wind", "random_wind", ChaosUiGroup::LinkModifiers, kEffectCatRandomWind, 15, true, false, 0, 30 },
    { "Random Bonks", "random_bonks", ChaosUiGroup::LinkModifiers, kEffectCatRandomBonks, 15, true, false, 0, 60 },

    // Hurt or Heal Link (non-timed, some parameterized)
    { "Empty Heart", "empty_heart", ChaosUiGroup::HurtHeal, kEffectCatNone, 15, true, true, 1, 0 },
    { "Fill Heart", "fill_heart", ChaosUiGroup::HurtHeal, kEffectCatNone, 15, true, true, 1, 0 },
    { "Knockback Weak", "knockback_link_weak", ChaosUiGroup::HurtHeal, kEffectCatNone, 15, true, false, 0, 0 },
    { "Knockback Strong", "knockback_link_strong", ChaosUiGroup::HurtHeal, kEffectCatNone, 10, true, false, 0, 0 },
    { "Knockback Mega", "knockback_link_mega", ChaosUiGroup::HurtHeal, kEffectCatNone, 6, true, false, 0, 0 },
    { "Burn Link", "burn_link", ChaosUiGroup::HurtHeal, kEffectCatNone, 10, true, false, 0, 0 },
    { "Freeze Link", "freeze_link", ChaosUiGroup::HurtHeal, kEffectCatNone, 10, true, false, 0, 0 },
    { "Electrocute Link", "electrocute_link", ChaosUiGroup::HurtHeal, kEffectCatNone, 10, true, false, 0, 0 },
    { "Kill Link", "kill_link", ChaosUiGroup::HurtHeal, kEffectCatNone, 2, true, false, 0, 0 },

    // Give Items and Consumables (non-timed, some parameterized)
    { "Add Heart Container", "add_heart_container", ChaosUiGroup::GiveItems, kEffectCatNone, 3, true, false, 0, 0 },
    { "Fill Magic", "fill_magic", ChaosUiGroup::GiveItems, kEffectCatNone, 6, true, false, 0, 0 },
    { "Give Rupees", "add_rupees", ChaosUiGroup::GiveItems, kEffectCatNone, 10, true, true, 50, 0 },
    { "Give Deku Shield", "give_deku_shield", ChaosUiGroup::GiveItems, kEffectCatNone, 2, true, false, 0, 0 },
    { "Give Hylian Shield", "give_hylian_shield", ChaosUiGroup::GiveItems, kEffectCatNone, 2, true, false, 0, 0 },
    { "Refill Sticks", "refill_sticks", ChaosUiGroup::GiveItems, kEffectCatNone, 6, true, true, 5, 0 },
    { "Refill Nuts", "refill_nuts", ChaosUiGroup::GiveItems, kEffectCatNone, 6, true, true, 5, 0 },
    { "Refill Bombs", "refill_bombs", ChaosUiGroup::GiveItems, kEffectCatNone, 6, true, true, 5, 0 },
    { "Refill Seeds", "refill_seeds", ChaosUiGroup::GiveItems, kEffectCatNone, 6, true, true, 10, 0 },
    { "Refill Arrows", "refill_arrows", ChaosUiGroup::GiveItems, kEffectCatNone, 6, true, true, 10, 0 },
    { "Refill Bombchus", "refill_bombchus", ChaosUiGroup::GiveItems, kEffectCatNone, 6, true, true, 5, 0 },

    // Take Items and Consumables (non-timed, some parameterized)
    { "Remove Heart Container", "remove_heart_container", ChaosUiGroup::TakeItems, kEffectCatNone, 2, true, false, 0,
      0 },
    { "Empty Magic", "empty_magic", ChaosUiGroup::TakeItems, kEffectCatNone, 6, true, false, 0, 0 },
    { "Take Rupees", "remove_rupees", ChaosUiGroup::TakeItems, kEffectCatNone, 10, true, true, 50, 0 },
    { "Take Deku Shield", "take_deku_shield", ChaosUiGroup::TakeItems, kEffectCatNone, 2, true, false, 0, 0 },
    { "Take Hylian Shield", "take_hylian_shield", ChaosUiGroup::TakeItems, kEffectCatNone, 2, true, false, 0, 0 },
    { "Take Sticks", "take_sticks", ChaosUiGroup::TakeItems, kEffectCatNone, 6, true, true, 5, 0 },
    { "Take Nuts", "take_nuts", ChaosUiGroup::TakeItems, kEffectCatNone, 6, true, true, 5, 0 },
    { "Take Bombs", "take_bombs", ChaosUiGroup::TakeItems, kEffectCatNone, 6, true, true, 5, 0 },
    { "Take Seeds", "take_seeds", ChaosUiGroup::TakeItems, kEffectCatNone, 6, true, true, 10, 0 },
    { "Take Arrows", "take_arrows", ChaosUiGroup::TakeItems, kEffectCatNone, 6, true, true, 10, 0 },
    { "Take Bombchus", "take_bombchus", ChaosUiGroup::TakeItems, kEffectCatNone, 6, true, true, 5, 0 },

    // Link Size Modifiers (timed)
    { "Giant Link", "giant_link", ChaosUiGroup::LinkSize, kEffectCatLinkSize, 10, true, false, 0, 30 },
    { "Minish Link", "minish_link", ChaosUiGroup::LinkSize, kEffectCatLinkSize, 10, true, false, 0, 30 },
    { "Paper Link", "paper_link", ChaosUiGroup::LinkSize, kEffectCatLinkSize, 10, true, false, 0, 30 },
    { "Squished Link", "squished_link", ChaosUiGroup::LinkSize, kEffectCatLinkSize, 10, true, false, 0, 30 },
    { "Invisible Link", "invisible_link", ChaosUiGroup::LinkSize, kEffectCatLinkSize, 10, true, false, 0, 30 },

    // Generic Effects (one timed, others non-timed)
    { "Random Bomb Fuse Timer", "random_bomb_timer", ChaosUiGroup::Generic, kEffectCatRandomBombFuseTimer, 10, true,
      false, 0, 60 },
    { "Set Time: Dawn", "set_time_to_dawn", ChaosUiGroup::Generic, kEffectCatNone, 2, true, false, 0, 0 },
    { "Set Time: Dusk", "set_time_to_dusk", ChaosUiGroup::Generic, kEffectCatNone, 2, true, false, 0, 0 },

    // Visual Effects (some timed)
    { "No UI", "no_ui", ChaosUiGroup::Visual, kEffectCatUi, 8, true, false, 0, 60 },
    { "Rainstorm", "rainstorm", ChaosUiGroup::Visual, kEffectCatWeather, 10, true, false, 0, 30 },
    { "Debug Mode", "debug_mode", ChaosUiGroup::Visual, kEffectCatDebugMode, 2, true, false, 0, 30 },
    { "Randomize Cosmetics", "random_cosmetics", ChaosUiGroup::Visual, kEffectCatNone, 6, true, false, 0, 0 },

    // Controls (some timed)
    { "No Z Button", "no_z_button", ChaosUiGroup::Controls, kEffectCatNoZ, 10, true, false, 0, 30 },
    { "Reverse Controls", "reverse_controls", ChaosUiGroup::Controls, kEffectCatReverseControls, 10, true, false, 0,
      60 },
    { "Pacifist Mode", "pacifist_mode", ChaosUiGroup::Controls, kEffectCatPacifist, 8, true, false, 0, 15 },
    { "Press Random Buttons", "press_random_buttons", ChaosUiGroup::Controls, kEffectCatRandomButtons, 8, true, false,
      0, 30 },
    { "Clear C-Buttons", "clear_cbuttons", ChaosUiGroup::Controls, kEffectCatNone, 6, true, false, 0, 0 },
    { "Clear D-Pad", "clear_dpad", ChaosUiGroup::Controls, kEffectCatNone, 6, true, false, 0, 0 },

    // Teleport Player (non-timed)
    { "TP: Link's House", "tp_links_house", ChaosUiGroup::Teleport, kEffectCatNone, 3, true, false, 0, 0 },
    { "TP: Minuet", "tp_minuet", ChaosUiGroup::Teleport, kEffectCatNone, 2, true, false, 0, 0 },
    { "TP: Bolero", "tp_bolero", ChaosUiGroup::Teleport, kEffectCatNone, 2, true, false, 0, 0 },
    { "TP: Serenade", "tp_serenade", ChaosUiGroup::Teleport, kEffectCatNone, 2, true, false, 0, 0 },
    { "TP: Requiem", "tp_requiem", ChaosUiGroup::Teleport, kEffectCatNone, 2, true, false, 0, 0 },
    { "TP: Nocturne", "tp_nocturne", ChaosUiGroup::Teleport, kEffectCatNone, 2, true, false, 0, 0 },
    { "TP: Prelude", "tp_prelude", ChaosUiGroup::Teleport, kEffectCatNone, 2, true, false, 0, 0 },
};

static std::string CC_FormatMs(long ms) {
    if (ms < 0) {
        ms = 0;
    }
    long totalSec = ms / 1000;
    long m = totalSec / 60;
    long s = totalSec % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld:%02ld", m, s);
    return std::string(buf);
}

static const char* CC_LookupUiNameForCode(const std::string& code) {
    for (const auto& def : kChaosEffects) {
        if (code == def.code) {
            return def.uiName;
        }
    }
    return nullptr;
}

void CrowdControl::SyncChaosStartup() {
    const bool chaosEnabledNow = (CVarGetInteger(CVAR_REMOTE_CROWD_CONTROL("ChaosEnabled"), 0) != 0);

    // If chaos is not enabled, reset guard so it can apply again next time user enables it.
    if (!chaosEnabledNow) {
        chaosStartupApplied = false;
        return;
    }

    // Start the chaos thread even if the menu is never opened.
    EnsureChaosThreadStarted();

    // Apply the "edge trigger" work once per enable.
    if (!chaosStartupApplied) {
        chaosStartupApplied = true;

        // If you still want Remote CC networking enabled when Chaos is on:
        CVarSetInteger(CVAR_REMOTE_CROWD_CONTROL("Enabled"), 1);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();

        // Only call Enable once; Enable() is safe-ish but better not spam it.
        if (!isEnabled) {
            Enable();
        }

        // Keep hooks in sync immediately.
        CrowdControl_RegisterHooks();
    }
}

void CrowdControl::DrawEffectTimersWindowContents() {
    // Keep removable effects cleanup happening while this standalone window is open
    PumpPendingRemovals();

    if (ImGui::BeginPopupContextWindow("##CCTimersContext", ImGuiPopupFlags_MouseButtonRight)) {
        int mode = CVarGetInteger(CVAR_CC_TIMERS_GROW_DIR, 0);

        if (ImGui::MenuItem("Grow: Auto", nullptr, mode == 0)) {
            CVarSetInteger(CVAR_CC_TIMERS_GROW_DIR, 0);
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
        if (ImGui::MenuItem("Grow: Up", nullptr, mode == 1)) {
            CVarSetInteger(CVAR_CC_TIMERS_GROW_DIR, 1);
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
        if (ImGui::MenuItem("Grow: Down", nullptr, mode == 2)) {
            CVarSetInteger(CVAR_CC_TIMERS_GROW_DIR, 2);
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }

        ImGui::EndPopup();
    }

    struct Row {
        std::string name;
        std::string state;
        std::string timeText;
        long ms;
        uint32_t id;
    };

    std::vector<Row> rows;
    rows.reserve(16);

    {
        std::scoped_lock<std::mutex> lock(activeEffectsMutex);
        for (auto* e : activeEffects) {
            if (!e) {
                continue;
            }

            const std::string code = !e->effectCode.empty() ? e->effectCode : std::string();
            std::string name = !e->displayName.empty() ? e->displayName : code;

            // If displayName is just the code, try to upgrade to the Chaos UI name when possible.
            if (!code.empty() && (name.empty() || name == code)) {
                if (const char* ui = CC_LookupUiNameForCode(code)) {
                    name = ui;
                }
            }
            if (name.empty()) {
                name = "(Effect)";
            }

            // Timed effects
            if (e->timeRemaining > 0) {
                Row r;
                r.name = name;
                r.state = e->isPaused ? "Paused" : "Active";
                r.ms = e->timeRemaining;
                r.timeText = CC_FormatMs(r.ms);
                r.id = e->id;
                rows.push_back(std::move(r));
                continue;
            }

            // Queued one-shot effects
            if (e->runOnceWhenPossible) {
                Row r;
                r.name = name;
                r.state = "Queued";
                r.ms = 0;
                r.timeText = "--:--";
                r.id = e->id;
                rows.push_back(std::move(r));
                continue;
            }
        }
    }

    if (rows.empty()) {
        ImGui::TextUnformatted("No active effects.");
        return;
    }

    auto rankState = [](const std::string& s) {
        if (s == "Active")
            return 0;
        if (s == "Paused")
            return 1;
        return 2; // Queued
    };

    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        int ra = rankState(a.state);
        int rb = rankState(b.state);
        if (ra != rb)
            return ra < rb;
        if (a.name != b.name)
            return a.name < b.name;
        return a.id < b.id;
    });

    // Measure desired column widths (include headers too)
    float wName = ImGui::CalcTextSize("Effect").x;
    float wState = ImGui::CalcTextSize("State").x;
    float wTime = ImGui::CalcTextSize("Time").x;

    for (const auto& r : rows) {
        wName = std::max(wName, ImGui::CalcTextSize(r.name.c_str()).x);
        wState = std::max(wState, ImGui::CalcTextSize(r.state.c_str()).x);
        wTime = std::max(wTime, ImGui::CalcTextSize(r.timeText.c_str()).x);
    }

    ImGuiStyle& style = ImGui::GetStyle();

    // Add padding inside each table cell.
    const float cellPadX = style.CellPadding.x * 2.0f;

    float colNameW = wName + cellPadX + 10.0f;
    float colStateW = wState + cellPadX + 10.0f;
    float colTimeW = wTime + cellPadX + 10.0f;

    // Total desired window width:
    float desiredW = colNameW + colStateW + colTimeW + (style.WindowPadding.x * 2.0f) + 20.0f;
    desiredW = std::clamp(desiredW, 260.0f, 1200.0f);

    sCCTimersDesiredWidth = desiredW;

    const ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("##CCTimers", 3, flags)) {
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, colNameW);
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, colStateW);
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, colTimeW);

        for (const auto& r : rows) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.name.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(r.state.c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r.timeText.c_str());
        }

        ImGui::EndTable();
    }
}

void CrowdControlEffectTimersWindow::Draw() {
    if (CVarGetInteger(CVAR_WINDOW("CrowdControlTimers"), 0) == 0) {
        return;
    }
    if (!IsVisible() || gPlayState == nullptr) {
        return;
    }

    static bool sAnchorInit = false;
    static float sAnchorY = 0.0f;

    static int sLastMode = -999;    // 0 auto, 1 up, 2 down
    static int sLastGrowDir = -999; // effective 1 up, 2 down

    static ImVec2 sPrevPos = ImVec2(FLT_MAX, FLT_MAX);
    static bool sWasMoving = false;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(0, 0, 0, CVarGetFloat(CVAR_SETTING("Notifications.BgOpacity"), 0.5f)));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);

    float w = sCCTimersDesiredWidth > 0.0f ? sCCTimersDesiredWidth : 320.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, FLT_MAX));

    ImGui::Begin("CrowdControl Timers", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);

    ImGuiViewport* winVp = ImGui::GetWindowViewport();
    if (winVp == nullptr) {
        winVp = ImGui::GetMainViewport();
    }
    ImGuiViewport* mainVp = ImGui::GetMainViewport();
    const bool inGameViewport = (winVp == mainVp);
    ImGuiViewport* refVp = inGameViewport ? mainVp : winVp;

    ImVec2 prePos = ImGui::GetWindowPos();
    ImVec2 preSize = ImGui::GetWindowSize();

    const int mode = CVarGetInteger(CVAR_CC_TIMERS_GROW_DIR, 0); // 0 auto, 1 up, 2 down
    int growDir = mode;

    if (growDir == 0) {
        const float workTop = refVp->WorkPos.y;
        const float workBot = refVp->WorkPos.y + refVp->WorkSize.y;

        const float spaceAbove = prePos.y - workTop;
        const float spaceBelow = workBot - (prePos.y + preSize.y);

        growDir = (spaceAbove > spaceBelow) ? 1 : 2; // 1=Up, 2=Down
    }

    if (!sAnchorInit || mode != sLastMode || growDir != sLastGrowDir) {
        sAnchorInit = true;
        sLastMode = mode;
        sLastGrowDir = growDir;
        sAnchorY = (growDir == 1) ? (prePos.y + preSize.y) : prePos.y;
    }

    DrawElement();

    ImVec2 postPos = ImGui::GetWindowPos();
    ImVec2 postSize = ImGui::GetWindowSize();

    const ImGuiIO& io = ImGui::GetIO();
    const bool mouseDown = io.MouseDown[ImGuiMouseButton_Left];

    bool posChanged = false;
    if (sPrevPos.x != FLT_MAX) {
        posChanged = (postPos.x != sPrevPos.x) || (postPos.y != sPrevPos.y);
    }
    sPrevPos = postPos;

    const bool isMovingNow = mouseDown && posChanged;
    const bool justStartedMoving = isMovingNow && !sWasMoving;
    const bool justStoppedMoving = !mouseDown && sWasMoving;
    sWasMoving = isMovingNow || (mouseDown && sWasMoving);

    if (justStartedMoving) {
        sAnchorY = (growDir == 1) ? (postPos.y + postSize.y) : postPos.y;
    }

    if (mouseDown && sWasMoving) {
        const float grab = 32.0f;

        ImVec2 clamped = postPos;

        const float minX = refVp->WorkPos.x - postSize.x + grab;
        const float maxX = refVp->WorkPos.x + refVp->WorkSize.x - grab;
        clamped.x = std::clamp(clamped.x, minX, maxX);

        const float minY = refVp->WorkPos.y - postSize.y + grab;
        const float maxY = refVp->WorkPos.y + refVp->WorkSize.y - grab;
        clamped.y = std::clamp(clamped.y, minY, maxY);

        if (clamped.x != postPos.x || clamped.y != postPos.y) {
            ImGui::SetWindowPos(clamped, ImGuiCond_Always);
            postPos = clamped;
        }

        sAnchorY = (growDir == 1) ? (postPos.y + postSize.y) : postPos.y;

        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        return;
    }

    if (justStoppedMoving) {
        ImVec2 droppedPos = ImGui::GetWindowPos();
        ImVec2 droppedSize = ImGui::GetWindowSize();
        sAnchorY = (growDir == 1) ? (droppedPos.y + droppedSize.y) : droppedPos.y;
        postPos = droppedPos;
        postSize = droppedSize;
    }

    ImVec2 targetPos = postPos;
    if (growDir == 1) {
        targetPos.y = sAnchorY - postSize.y;
    } else {
        targetPos.y = sAnchorY;
    }

    const float minX = refVp->WorkPos.x;
    const float maxX = refVp->WorkPos.x + refVp->WorkSize.x - postSize.x;
    targetPos.x = std::clamp(targetPos.x, minX, maxX);

    const float minY = refVp->WorkPos.y;
    const float maxY = refVp->WorkPos.y + refVp->WorkSize.y - postSize.y;
    targetPos.y = std::clamp(targetPos.y, minY, maxY);

    if (targetPos.x != postPos.x || targetPos.y != postPos.y) {
        ImGui::SetWindowPos(targetPos, ImGuiCond_Always);
        postPos = targetPos;
    }

    sAnchorY = (growDir == 1) ? (postPos.y + postSize.y) : postPos.y;

    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void CrowdControl::EnsureChaosThreadStarted() {
    if (chaosThreadStarted.exchange(true)) {
        return;
    }
    chaosThreadExit = false;
    chaosThread = std::thread(&CrowdControl::ChaosLoop, this);
    chaosThread.detach();

    // Ensure main-thread hook exists as soon as chaos is used.
    CrowdControl_RegisterHooks();
}

bool CrowdControl::CanRunChaosNow() {
    // Avoid running on file select / menus.
    if (gPlayState == nullptr) {
        return false;
    }
    return true;
}

void CrowdControl::ChaosLoop() {
    std::random_device rd;
    std::mt19937 rng(rd());
    chaosRunning = true;

    while (!chaosThreadExit) {
        int minS = CVarGetInteger(CVAR_CC_CHAOS_MIN_SECONDS, 20);
        int maxS = CVarGetInteger(CVAR_CC_CHAOS_MAX_SECONDS, 60);
        if (minS < 1) {
            minS = 1;
        }
        if (maxS < minS) {
            maxS = minS;
        }

        std::uniform_int_distribution<int> delayDist(minS, maxS);
        const int delay = delayDist(rng);

        int elapsed = 0;
        while (elapsed < delay && !chaosThreadExit) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            bool hasQueuedWaiting = false;
            {
                std::scoped_lock<std::mutex> lock(activeEffectsMutex);
                for (Effect* e : activeEffects) {
                    if (!e)
                        continue;

                    if (e->runOnceWhenPossible) {
                        hasQueuedWaiting = true;
                        break;
                    }

                    if (e->timeRemaining > 0 && e->isPaused) {
                        hasQueuedWaiting = true;
                        break;
                    }
                }
            }

            (void)hasQueuedWaiting;
            elapsed++;
        }

        if (chaosThreadExit) {
            break;
        }

        if (!CVarGetInteger(CVAR_CC_CHAOS_ENABLED, 0)) {
            continue;
        }
        if (!CanRunChaosNow()) {
            continue;
        }

        int totalWeight = 0;
        for (const auto& def : kChaosEffects) {
            const std::string enabledKey = CCChaosEffectEnabledKey(def.code);
            const std::string weightKey = CCChaosEffectWeightKey(def.code);

            if (!CVarGetInteger(enabledKey.c_str(), def.defaultEnabled ? 1 : 0)) {
                continue;
            }
            const int w = CVarGetInteger(weightKey.c_str(), def.defaultWeight);
            if (w <= 0) {
                continue;
            }
            totalWeight += w;
        }

        if (totalWeight <= 0) {
            continue;
        }

        std::uniform_int_distribution<int> pickDist(1, totalWeight);
        int pick = pickDist(rng);

        const ChaosEffectDef* chosen = nullptr;
        for (const auto& def : kChaosEffects) {
            const std::string enabledKey = CCChaosEffectEnabledKey(def.code);
            const std::string weightKey = CCChaosEffectWeightKey(def.code);

            if (!CVarGetInteger(enabledKey.c_str(), def.defaultEnabled ? 1 : 0)) {
                continue;
            }
            const int w = CVarGetInteger(weightKey.c_str(), def.defaultWeight);
            if (w <= 0) {
                continue;
            }

            pick -= w;
            if (pick <= 0) {
                chosen = &def;
                break;
            }
        }

        if (chosen != nullptr) {
            TriggerLocalEffectByCode(chosen->code, true);

            if (CVarGetInteger(CVAR_CC_CHAOS_NOTIFY, 1)) {
                Notification::Emit({
                    .message = std::string("Chaos: ") + chosen->uiName,
                });
            }
        }
    }

    chaosRunning = false;
}

void CrowdControl::TriggerLocalEffectByCode(const char* effectCode, bool allowTimed) {
    static std::atomic<uint32_t> sLocalId{ 100000 };
    nlohmann::json payload;
    payload["id"] = sLocalId.fetch_add(1);
    payload["type"] = 0;
    payload["viewer"] = "Chaos";
    payload["code"] = effectCode;

    payload["parameters"] = nlohmann::json::array();
    for (const auto& def : kChaosEffects) {
        if (strcmp(def.code, effectCode) == 0) {
            if (def.hasParameter) {
                payload["parameters"].push_back(def.defaultParameter);
            }
            break;
        }
    }

    Effect* effect = ParseMessage(payload);
    if (!effect) {
        return;
    }

    effect->isChaos = true;

    for (const auto& def : kChaosEffects) {
        if (strcmp(def.code, effectCode) == 0) {
            effect->displayName = def.uiName;
            break;
        }
    }

    if (!effect->timeRemaining || !allowTimed) {
        EffectResult r = CrowdControl::ExecuteEffect(effect);
        if (r == EffectResult::Success) {
            delete effect;
            return;
        }
        if (r == EffectResult::Retry) {
            effect->runOnceWhenPossible = true;
            effect->timeRemaining = 0;
            effect->isPaused = false;
            effect->lastExecutionResult = EffectResult::Retry;

            std::scoped_lock<std::mutex> lock(activeEffectsMutex);
            activeEffects.push_back(effect);
            return;
        }
        delete effect;
        return;
    }

    {
        std::scoped_lock<std::mutex> lock(activeEffectsMutex);
        for (Effect* active : activeEffects) {
            if (active && active->timeRemaining > 0 && active->category == effect->category &&
                active->id < effect->id) {
                effect->isPaused = true;
                break;
            }
        }
    }

    if (effect->isPaused) {
        std::scoped_lock<std::mutex> lock(activeEffectsMutex);
        activeEffects.push_back(effect);
        return;
    }

    EffectResult can = CrowdControl::CanApplyEffect(effect);
    if (can == EffectResult::Failure) {
        delete effect;
        return;
    }

    if (can == EffectResult::Retry) {
        effect->isPaused = true;
        std::scoped_lock<std::mutex> lock(activeEffectsMutex);
        activeEffects.push_back(effect);
        return;
    }

    {
        std::scoped_lock<std::mutex> lock(activeEffectsMutex);
        activeEffects.push_back(effect);
    }
}

void CrowdControl::ClearTimedEffects() {
    std::vector<std::pair<RemovableGameInteractionEffect*, uint32_t>> toRemove;

    {
        std::scoped_lock<std::mutex> lock(activeEffectsMutex);

        auto it = activeEffects.begin();
        while (it != activeEffects.end()) {
            Effect* e = *it;
            if (!e) {
                it = activeEffects.erase(it);
                continue;
            }

            if (e->timeRemaining > 0) {
                if (e->giEffect) {
                    if (auto* removable = dynamic_cast<RemovableGameInteractionEffect*>(e->giEffect)) {
                        toRemove.push_back({ removable, e->category });
                    }
                }

                it = activeEffects.erase(it);
                delete e;
                continue;
            }

            ++it;
        }
    }

    if (!toRemove.empty()) {
        std::scoped_lock<std::mutex> rlock(pendingRemovalsMutex);
        for (auto& [r, cat] : toRemove) {
            pendingRemovals.push_back({ r, cat });
        }
    }

    PumpPendingRemovals();
}

void CrowdControl::DrawChaosWindowContents() {
    if (CVarGetInteger(CVAR_CC_CHAOS_ENABLED, 0) != 0) {
        EnsureChaosThreadStarted();
    }

    CrowdControl_RegisterHooks();
    PumpPendingRemovals();

    DrawChaosUi();
}

struct ChaosThemeScope {
    int colorCount = 0;

    ChaosThemeScope() {
        UIWidgets::PushStyleInput(THEME_COLOR);
        UIWidgets::PushStyleCheckbox(THEME_COLOR);
        UIWidgets::PushStyleButton(THEME_COLOR);

        const ImVec4 accent = UIWidgets::ColorValues.at(THEME_COLOR);

        // No transparency variants (alpha stays 1.0)
        ImVec4 accentSoft = accent;
        ImVec4 accentHover = accent;
        ImVec4 accentActive = accent;

        accentSoft.w = 1.0f;
        accentHover.w = 1.0f;
        accentActive.w = 1.0f;

        // Make the slider "track" darker than the grab (no alpha, just darker RGB)
        ImVec4 track = accent;
        track.x *= 1.0f;
        track.y *= 1.0f;
        track.z *= 1.0f;
        track.w = 1.0f;

        // Make the slider handle brighter than everything else
        ImVec4 grab = accent;
        grab.x = std::min(grab.x * 1.25f, 1.0f);
        grab.y = std::min(grab.y * 1.25f, 1.0f);
        grab.z = std::min(grab.z * 1.25f, 1.0f);
        grab.w = 1.0f;

        ImVec4 grabActive = accent;
        grabActive.x = std::min(grabActive.x * 1.45f, 1.0f);
        grabActive.y = std::min(grabActive.y * 1.45f, 1.0f);
        grabActive.z = std::min(grabActive.z * 1.45f, 1.0f);
        grabActive.w = 1.0f;

        auto push = [&](ImGuiCol idx, ImVec4 c) {
            ImGui::PushStyleColor(idx, c);
            colorCount++;
        };

        // White grab + checkmark, semi-transparent
        ImVec4 white075 = ImVec4(1.0f, 1.0f, 1.0f, 0.75f);
        ImVec4 white050 = ImVec4(1.0f, 1.0f, 1.0f, 0.50f);

        // Slider handle
        push(ImGuiCol_SliderGrab, white050);
        push(ImGuiCol_SliderGrabActive, white050);

        // Checkbox checkmark (not the box)
        push(ImGuiCol_CheckMark, white075);

        // Buttons (this fixes SmallButton +/- being grey)
        push(ImGuiCol_Button, accentSoft);
        push(ImGuiCol_ButtonHovered, accentHover);
        push(ImGuiCol_ButtonActive, accentActive);

        // CollapsingHeader / Table header colors (this fixes category boxes)
        push(ImGuiCol_Header, accentSoft);
        push(ImGuiCol_HeaderHovered, accentHover);
        push(ImGuiCol_HeaderActive, accentActive);

        // Slider track + grab (this fixes "bar is same color as slider box")
        push(ImGuiCol_FrameBg, track);
        push(ImGuiCol_FrameBgHovered, track);
        push(ImGuiCol_FrameBgActive, track);

        // Tabs
        push(ImGuiCol_Tab, accentSoft);
        push(ImGuiCol_TabHovered, accentHover);
        push(ImGuiCol_TabActive, accentActive);
        push(ImGuiCol_TabUnfocused, accentSoft);
        push(ImGuiCol_TabUnfocusedActive, accentHover);

        // Column resize bars / separators
        push(ImGuiCol_Separator, accentSoft);
        push(ImGuiCol_SeparatorHovered, accentHover);
        push(ImGuiCol_SeparatorActive, accentActive);
    }

    ~ChaosThemeScope() {
        if (colorCount > 0) {
            ImGui::PopStyleColor(colorCount);
        }

        UIWidgets::PopStyleButton();
        UIWidgets::PopStyleCheckbox();
        UIWidgets::PopStyleInput();
    }
};

static bool CC_DrawMinusSliderPlusInt(const char* id, int* value, int minV, int maxV, const char* format = nullptr) {
    bool changed = false;

    const float btnW = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    if (UIWidgets::Button("-", UIWidgets::ButtonOptions().Color(THEME_COLOR).Size(ImVec2(btnW, 0)))) {
        int nv = std::max(minV, *value - 1);
        changed |= (nv != *value);
        *value = nv;
    }

    ImGui::SameLine(0.0f, spacing);

    float avail = ImGui::GetContentRegionAvail().x;
    float sliderW = avail - btnW - spacing;
    sliderW = std::max(sliderW, 80.0f);

    ImGui::SetNextItemWidth(sliderW);

    ImGui::PushID(id);
    if (format) {
        changed |= ImGui::SliderInt("##slider", value, minV, maxV, format);
    } else {
        changed |= ImGui::SliderInt("##slider", value, minV, maxV);
    }
    ImGui::PopID();

    ImGui::SameLine(0.0f, spacing);

    if (UIWidgets::Button("+", UIWidgets::ButtonOptions().Color(THEME_COLOR).Size(ImVec2(btnW, 0)))) {
        int nv = std::min(maxV, *value + 1);
        changed |= (nv != *value);
        *value = nv;
    }

    return changed;
}

void CrowdControl::DrawChaosUi() {
    ChaosThemeScope theme; // Top controls
    bool enabled = CVarGetInteger(CVAR_CC_CHAOS_ENABLED, 0) != 0;
    if (ImGui::Checkbox("Enable Chaos Mode", &enabled)) {
        CVarSetInteger(CVAR_CC_CHAOS_ENABLED, enabled ? 1 : 0);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();

        CrowdControl_RegisterHooks();
    }

    static bool sWasChaosEnabled = false;
    bool chaosEnabledNow = CVarGetInteger(CVAR_CC_CHAOS_ENABLED, 0) != 0;

    if (chaosEnabledNow && !sWasChaosEnabled) {
        CVarSetInteger(CVAR_REMOTE_CROWD_CONTROL("Enabled"), 1);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        CrowdControl::Instance->Enable();
    }
    sWasChaosEnabled = chaosEnabledNow;

    bool showNotif = CVarGetInteger(CVAR_CC_CHAOS_NOTIFY, 1) != 0;
    if (ImGui::Checkbox("Show notifications", &showNotif)) {
        CVarSetInteger(CVAR_CC_CHAOS_NOTIFY, showNotif ? 1 : 0);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    // Timers window toggle
    bool showTimers = CVarGetInteger(CVAR_WINDOW("CrowdControlTimers"), 0) != 0;
    ImGui::SameLine();
    if (ImGui::Checkbox("Show timers window", &showTimers)) {
        CVarSetInteger(CVAR_WINDOW("CrowdControlTimers"), showTimers ? 1 : 0);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    int minS = CVarGetInteger(CVAR_CC_CHAOS_MIN_SECONDS, 20);
    int maxS = CVarGetInteger(CVAR_CC_CHAOS_MAX_SECONDS, 60);
    if (minS < 1)
        minS = 1;
    if (maxS < minS)
        maxS = minS;

    // Min delay
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Min Delay (s)");
    ImGui::SameLine();

    ImGui::PushID("ChaosMinDelayRow");
    if (CC_DrawMinusSliderPlusInt("minDelay", &minS, 1, 600)) {
        if (maxS < minS) {
            maxS = minS;
        }
    }
    ImGui::PopID();

    // Max delay
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Max Delay (s)");
    ImGui::SameLine();

    ImGui::PushID("ChaosMaxDelayRow");
    if (CC_DrawMinusSliderPlusInt("maxDelay", &maxS, 1, 600)) {
        if (maxS < minS) {
            maxS = minS;
        }
    }
    ImGui::PopID();

    if (CVarGetInteger(CVAR_CC_CHAOS_MIN_SECONDS, 20) != minS) {
        CVarSetInteger(CVAR_CC_CHAOS_MIN_SECONDS, minS);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    if (CVarGetInteger(CVAR_CC_CHAOS_MAX_SECONDS, 60) != maxS) {
        CVarSetInteger(CVAR_CC_CHAOS_MAX_SECONDS, maxS);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    ImGui::Spacing();

    // Reset Defaults: turns everything ON + resets weights + resets durations
    if (ImGui::Button("Reset Defaults")) {
        for (const auto& def : kChaosEffects) {
            const std::string enabledKey = CCChaosEffectEnabledKey(def.code);
            const std::string weightKey = CCChaosEffectWeightKey(def.code);
            const std::string durationKey = CCChaosEffectDurationKey(def.code);

            CVarSetInteger(enabledKey.c_str(), 1);
            CVarSetInteger(weightKey.c_str(), def.defaultWeight);

            if (def.defaultDurationSeconds > 0) {
                CVarSetInteger(durationKey.c_str(), def.defaultDurationSeconds);
            }
        }
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Active Effects")) {
        if (CrowdControl::Instance) {
            CrowdControl::Instance->ClearTimedEffects();
        }
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("ChaosTabs")) {

        if (ImGui::BeginTabItem("Effects")) {
            // Enable/Disable all (exact behavior: flip ChaosEffectEnabled.* for every effect)
            if (ImGui::Button("Enable All")) {
                for (const auto& def : kChaosEffects) {
                    CVarSetInteger(CCChaosEffectEnabledKey(def.code).c_str(), 1);
                }
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }
            ImGui::SameLine();
            if (ImGui::Button("Disable All")) {
                for (const auto& def : kChaosEffects) {
                    CVarSetInteger(CCChaosEffectEnabledKey(def.code).c_str(), 0);
                }
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }

            ImGui::Separator();

            // Expand/Collapse all (Effects)
            static int sSetAllGroupsOpenEffects = 0;
            if (ImGui::Button("Expand All##Effects")) {
                sSetAllGroupsOpenEffects = 1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Collapse All##Effects")) {
                sSetAllGroupsOpenEffects = 2;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset All Weights")) {
                for (const auto& def : kChaosEffects) {
                    CVarSetInteger(CCChaosEffectWeightKey(def.code).c_str(), def.defaultWeight);
                }
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }

            ImGui::Separator();

            auto drawGroupEffects = [&](ChaosUiGroup group) {
                if (sSetAllGroupsOpenEffects == 1) {
                    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                } else if (sSetAllGroupsOpenEffects == 2) {
                    ImGui::SetNextItemOpen(false, ImGuiCond_Always);
                }

                if (!ImGui::CollapsingHeader(ChaosGroupName(group), ImGuiTreeNodeFlags_DefaultOpen)) {
                    return;
                }

                if (ImGui::BeginTable("##ChaosTable", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28.0f);
                    ImGui::TableSetupColumn("Effect", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                    ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                    ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, 60.0f);

                    // Custom clickable header row
                    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

                    // "On" header (clickable)
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TableHeader("On");

                    ImRect onRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                    ImGui::SetCursorScreenPos(onRect.Min);
                    ImGui::InvisibleButton("##hdr_on", onRect.GetSize());

                    bool onHovered = ImGui::IsItemHovered();
                    bool onClicked = ImGui::IsItemClicked();

                    if (onHovered) {
                        ImGui::GetWindowDrawList()->AddRectFilled(onRect.Min, onRect.Max,
                                                                  ImGui::GetColorU32(ImGuiCol_HeaderHovered));
                    }

                    if (onClicked) {
                        bool anyDisabled = false;
                        for (const auto& def : kChaosEffects) {
                            if (def.group != group)
                                continue;

                            const std::string key = CCChaosEffectEnabledKey(def.code);
                            if (!CVarGetInteger(key.c_str(), def.defaultEnabled ? 1 : 0)) {
                                anyDisabled = true;
                                break;
                            }
                        }

                        const int newValue = anyDisabled ? 1 : 0;
                        for (const auto& def : kChaosEffects) {
                            if (def.group != group)
                                continue;

                            CVarSetInteger(CCChaosEffectEnabledKey(def.code).c_str(), newValue);
                        }

                        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                    }

                    // "Effect" header
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TableHeader("Effect");

                    // "Weight" header
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TableHeader("Weight");

                    // "Reset" header (clickable, weight-only)
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TableHeader("Reset");

                    ImRect resetRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                    ImGui::SetCursorScreenPos(resetRect.Min);
                    ImGui::InvisibleButton("##hdr_reset", resetRect.GetSize());

                    bool resetHovered = ImGui::IsItemHovered();
                    bool resetClicked = ImGui::IsItemClicked();

                    if (resetHovered) {
                        ImGui::GetWindowDrawList()->AddRectFilled(resetRect.Min, resetRect.Max,
                                                                  ImGui::GetColorU32(ImGuiCol_HeaderHovered));
                    }

                    if (resetClicked) {
                        for (const auto& def : kChaosEffects) {
                            if (def.group != group)
                                continue;

                            CVarSetInteger(CCChaosEffectWeightKey(def.code).c_str(), def.defaultWeight);
                        }

                        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                    }

                    // Rows
                    for (const auto& def : kChaosEffects) {
                        if (def.group != group) {
                            continue;
                        }

                        const std::string enabledKey = CCChaosEffectEnabledKey(def.code);
                        const std::string weightKey = CCChaosEffectWeightKey(def.code);
                        bool effEnabled = CVarGetInteger(enabledKey.c_str(), def.defaultEnabled ? 1 : 0) != 0;
                        int weight = CVarGetInteger(weightKey.c_str(), def.defaultWeight);

                        ImGui::TableNextRow();

                        // On
                        ImGui::TableSetColumnIndex(0);
                        std::string onId = std::string("##on_") + def.code;
                        if (ImGui::Checkbox(onId.c_str(), &effEnabled)) {
                            CVarSetInteger(enabledKey.c_str(), effEnabled ? 1 : 0);
                            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                        }

                        // Name
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(def.uiName);

                        // Weight
                        ImGui::TableSetColumnIndex(2);

                        ImGui::PushID(def.code);

                        CC_DrawMinusSliderPlusInt("weight", &weight, 0, 100);

                        ImGui::PopID();

                        if (CVarGetInteger(weightKey.c_str(), def.defaultWeight) != weight) {
                            CVarSetInteger(weightKey.c_str(), weight);
                            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                        }

                        // Reset (row): weight only
                        ImGui::TableSetColumnIndex(3);
                        std::string rId = std::string("Reset##") + def.code;
                        if (ImGui::Button(rId.c_str())) {
                            CVarSetInteger(weightKey.c_str(), def.defaultWeight);
                            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                        }
                    }

                    ImGui::EndTable();
                }
            };

            drawGroupEffects(ChaosUiGroup::Spawn);
            drawGroupEffects(ChaosUiGroup::LinkModifiers);
            drawGroupEffects(ChaosUiGroup::HurtHeal);
            drawGroupEffects(ChaosUiGroup::GiveItems);
            drawGroupEffects(ChaosUiGroup::TakeItems);
            drawGroupEffects(ChaosUiGroup::LinkSize);
            drawGroupEffects(ChaosUiGroup::Generic);
            drawGroupEffects(ChaosUiGroup::Visual);
            drawGroupEffects(ChaosUiGroup::Controls);
            drawGroupEffects(ChaosUiGroup::Teleport);

            sSetAllGroupsOpenEffects = 0;

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Durations")) {
            static int sSetAllGroupsOpenDur = 0;

            if (ImGui::Button("Expand All##Dur")) {
                sSetAllGroupsOpenDur = 1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Collapse All##Dur")) {
                sSetAllGroupsOpenDur = 2;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset All Durations")) {
                for (const auto& def : kChaosEffects) {
                    if (def.defaultDurationSeconds <= 0) {
                        continue;
                    }
                    CVarSetInteger(CCChaosEffectDurationKey(def.code).c_str(), def.defaultDurationSeconds);
                }
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }

            ImGui::Separator();

            auto drawGroupDur = [&](ChaosUiGroup group) {
                if (sSetAllGroupsOpenDur == 1) {
                    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                } else if (sSetAllGroupsOpenDur == 2) {
                    ImGui::SetNextItemOpen(false, ImGuiCond_Always);
                }

                if (!ImGui::CollapsingHeader(ChaosGroupName(group), ImGuiTreeNodeFlags_DefaultOpen)) {
                    return;
                }

                const ImGuiTableFlags durFlags =
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

                if (ImGui::BeginTable("##ChaosDurTable", 3, durFlags)) {
                    ImGui::TableSetupColumn("Effect", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                    ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                    ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, 64.0f);

                    for (const auto& def : kChaosEffects) {
                        if (def.group != group) {
                            continue;
                        }
                        if (def.defaultDurationSeconds <= 0) {
                            continue; // only timed effects
                        }

                        const std::string durationKey = CCChaosEffectDurationKey(def.code);
                        int sec = CVarGetInteger(durationKey.c_str(), def.defaultDurationSeconds);

                        ImGui::TableNextRow();

                        // Effect name
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(def.uiName);

                        // Duration controls: [-] [slider] [+]
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID(def.code);

                        int durSec = CVarGetInteger(durationKey.c_str(), def.defaultDurationSeconds);
                        if (durSec < 0) {
                            durSec = 0;
                        }

                        // UI range clamp (slider is 0..120)
                        int uiSec = std::clamp(durSec, 0, 120);

                        if (CC_DrawMinusSliderPlusInt("dur", &uiSec, 0, 120, "%ds")) {
                            durSec = uiSec;
                        }

                        ImGui::PopID();

                        if (CVarGetInteger(durationKey.c_str(), def.defaultDurationSeconds) != durSec) {
                            CVarSetInteger(durationKey.c_str(), durSec);
                            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                        }

                        // Reset button
                        ImGui::TableSetColumnIndex(2);
                        std::string defId = std::string("Reset##") + def.code;
                        if (ImGui::Button(defId.c_str())) {
                            CVarSetInteger(durationKey.c_str(), def.defaultDurationSeconds);
                            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                        }
                    }

                    ImGui::EndTable();
                }
            };

            auto groupHasTimed = [&](ChaosUiGroup g) -> bool {
                for (const auto& def : kChaosEffects) {
                    if (def.group == g && def.defaultDurationSeconds > 0) {
                        return true;
                    }
                }
                return false;
            };

            for (ChaosUiGroup g : {
                     ChaosUiGroup::Spawn,
                     ChaosUiGroup::LinkModifiers,
                     ChaosUiGroup::HurtHeal,
                     ChaosUiGroup::GiveItems,
                     ChaosUiGroup::TakeItems,
                     ChaosUiGroup::LinkSize,
                     ChaosUiGroup::Generic,
                     ChaosUiGroup::Visual,
                     ChaosUiGroup::Controls,
                     ChaosUiGroup::Teleport,
                 }) {
                if (groupHasTimed(g)) {
                    drawGroupDur(g);
                }
            }

            sSetAllGroupsOpenDur = 0;

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}
