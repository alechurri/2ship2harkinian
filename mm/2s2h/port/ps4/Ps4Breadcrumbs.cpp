// PS4: progress markers in the log. When the system ends the process without a report (out of
// memory, GPU fault), the last marker says how far the game got.

#ifdef __PS4__

#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

#include <ship/port/ps4/Ps4Platform.h>
#include <spdlog/spdlog.h>

namespace {
bool sSceneFrameLogged = true;

void RegisterPs4Breadcrumbs() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnFileSelectSaveLoad>(
        [](s16 fileNum, bool isOwlSave, SaveContext*) {
            SPDLOG_INFO("[PS4] file select: save {} loaded{}", fileNum, isOwlSave ? " (owl save)" : "");
        });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSaveLoad>(
        [](s16 fileNum) { SPDLOG_INFO("[PS4] save {} applied", fileNum); });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnSceneInit>([](s8 sceneId, s8 spawnNum) {
        SPDLOG_INFO("[PS4] scene {} init (spawn {})", (int)sceneId, (int)spawnNum);
        Ship::Ps4::LogMemoryStats("at scene init");
        sSceneFrameLogged = false;
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnRoomInit>(
        [](s8 sceneId, s8 roomNum) { SPDLOG_INFO("[PS4] room {} of scene {} init", (int)roomNum, (int)sceneId); });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateDrawFinish>([]() {
        if (!sSceneFrameLogged) {
            sSceneFrameLogged = true;
            SPDLOG_INFO("[PS4] first frame of the scene drawn");
        }
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayDestroy>(
        []() { SPDLOG_INFO("[PS4] play state destroyed"); });
}

RegisterShipInitFunc sInit(RegisterPs4Breadcrumbs, {});
} // namespace

#endif // __PS4__
