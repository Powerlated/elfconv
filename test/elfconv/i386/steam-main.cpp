#define VERSION_SAFE_STEAM_API_INTERFACES
#include <steam/steam_api.h>
#include <steam/steam_api_internal.h>
#include <steam/isteamclient021.h>
#include <steam/isteamuser.h>
#include <steam/isteamuserstats.h>
#include <steam/isteamutils.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void require(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "steam fixture failed: %s\n", what);
    std::exit(1);
  }
}

class StatsReceiver {
 public:
  StatsReceiver() : callback(this, &StatsReceiver::on_stats) {}
  void on_stats(UserStatsReceived_t *event) {
    ++calls;
    app_id = CGameID(event->m_nGameID).AppID();
    result = event->m_eResult;
    user_valid = event->m_steamIDUser.IsValid();
  }

  int calls = 0;
  AppId_t app_id = 0;
  EResult result = k_EResultNone;
  bool user_valid = false;
  CCallback<StatsReceiver, UserStatsReceived_t> callback;
};

int main() {
  require(SteamAPI_InitSafe(), "SteamAPI_InitSafe");

  const HSteamUser user_handle = SteamAPI_GetHSteamUser();
  const HSteamPipe pipe = SteamAPI_GetHSteamPipe();
  require(user_handle != 0 && pipe != 0, "client handles");

  auto *client = static_cast<ISteamClient021 *>(
      SteamInternal_CreateInterface("SteamClient021"));
  require(client != nullptr, "SteamClient021 interface");

  ISteamUser *user = client->GetISteamUser(user_handle, pipe, "SteamUser023");
  ISteamUtils *utils = client->GetISteamUtils(pipe, "SteamUtils010");
  require(user != nullptr && utils != nullptr, "Portal interface versions");
  require(user->GetHSteamUser() == user_handle, "ISteamUser::GetHSteamUser");
  const CSteamID steam_id = user->GetSteamID();
  require(steam_id.IsValid(), "ISteamUser::GetSteamID");
  require(utils->GetAppID() == 400, "ISteamUtils::GetAppID");

  ISteamUser *found_user = static_cast<ISteamUser *>(
      SteamInternal_FindOrCreateUserInterface(user_handle, "SteamUser023"));
  ISteamUtils *found_utils = static_cast<ISteamUtils *>(
      SteamInternal_FindOrCreateUserInterface(0, "SteamUtils010"));
  require(found_user != nullptr && found_utils != nullptr &&
              found_utils->GetAppID() == 400,
          "SteamInternal_FindOrCreateUserInterface");

  ISteamUserStats *stats = client->GetISteamUserStats(
      user_handle, pipe, STEAMUSERSTATS_INTERFACE_VERSION);
  require(stats != nullptr, "ISteamUserStats interface");
  {
    StatsReceiver receiver;
    require(stats->RequestCurrentStats(), "ISteamUserStats::RequestCurrentStats");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!receiver.calls && std::chrono::steady_clock::now() < deadline) {
      SteamAPI_RunCallbacks();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(receiver.calls == 1, "UserStatsReceived_t callback delivered once");
    require(receiver.app_id == 400 && receiver.result == k_EResultOK &&
                receiver.user_valid,
            "UserStatsReceived_t payload");
    std::printf("steam:appid=%u user=%u callback=%d result=%d\n",
                utils->GetAppID(), steam_id.IsValid(), receiver.calls,
                receiver.result);
  }

  SteamAPI_Shutdown();
  std::puts("steam:shutdown");
  return 0;
}
