#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

static constexpr int SCE_SHARE_PLAY_ERROR_INVALID_ARGS = static_cast<int>(0x810E0001);
static constexpr std::int32_t SCE_USER_SERVICE_USER_ID_INVALID = -1;

extern "C" {

int APS5_VABI sceSharePlayInitialize(void* heap, size_t heapSize) {
    (void)heap;
    if (heapSize == 0) APS5_INVALID_ARG_EX;
    return 0;
}

int APS5_VABI sceSharePlayTerminate(void) {
    return 0;
}

int APS5_VABI sceSharePlayGetCurrentConnectionInfoA(SharePlayConnectionInfoA* info) {
    if (info == nullptr) return SCE_SHARE_PLAY_ERROR_INVALID_ARGS;
    *info = {};
    info->hostUserId = SCE_USER_SERVICE_USER_ID_INVALID;
    info->visitorUserId = SCE_USER_SERVICE_USER_ID_INVALID;
    return 0;
}

int APS5_VABI sceSharePlayProhibitController(int prohibited) {
    if (prohibited != 0 && prohibited != 1) APS5_INVALID_ARG_EX;
    return 0;
}

int APS5_VABI sceSharePlayCrashDaemon(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayGetCurrentConnectionInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayGetCurrentInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayGetEvent(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayNotifyDialogOpen(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayNotifyForceCloseForCdlg(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayResumeScreenForCdlg(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayServerLock(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayServerUnLock(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlaySetMode(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlaySetProhibition(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlaySetProhibitionModeWithAppId(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayStartStandby(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayStartStreaming(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayStopStandby(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSharePlayStopStreaming(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
