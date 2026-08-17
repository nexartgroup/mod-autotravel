#ifndef MOD_AUTOTRAVEL_H
#define MOD_AUTOTRAVEL_H

#include "Common.h"
#include "ObjectGuid.h"
#include "MoveSplineInitArgs.h"

#include <string>
#include <unordered_map>

class Player;

enum ATState : uint8
{
    AT_IDLE = 0,
    AT_CALCULATE_PATH,
    AT_TRAVELING,
    AT_COMBAT_PAUSED,
    AT_MOUNTING,
    AT_ARRIVED,
    AT_FAILED
};

char const* ATStateName(ATState s);

struct ATConfig
{
    bool  enable            = true;
    float arrivalDistance   = 8.0f;
    bool  autoMount         = true;
    float mountMinDistance  = 150.0f;
    bool  pauseInCombat     = true;
    bool  resumeAfterCombat = true;
    uint32 combatGraceMs    = 1500;
    bool  stuckDetection    = true;
    uint32 stuckTimeoutMs   = 5000;
    float stuckMinDistance  = 3.0f;
    uint32 maxRepathAttempts = 8;
    bool  resumeAfterDeath  = false;
    bool  takeClientControl = true;
    uint32 chunkPoints      = 12;
    bool  allowTeleport     = true;
    float teleportMinDist   = 0.0f;
    uint32 teleportCooldown = 5;
    bool  debug             = false;
};

extern ATConfig ATConf;

struct ATSession
{
    ATState state       = AT_IDLE;

    uint32  mapId       = 0;
    float   destX       = 0.0f;
    float   destY       = 0.0f;
    float   destZ       = 0.0f;
    std::string destName;

    Movement::PointsArray path;
    size_t  idx         = 0;
    bool    pathIncomplete = false;

    bool    controlTaken   = false;
    bool    debug          = false;

    // arrival tolerance override (0 = use global)
    float   arrivalOverride = 0.0f;

    // stuck tracking
    float   lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;
    uint32  stuckTimer = 0;

    // misc timers
    uint32  repathAttempts = 0;
    uint32  combatTimer    = 0;
    uint32  statusTimer    = 0;
    uint32  mountTimer     = 0;
    bool    mountTried     = false;
};

class AutoTravelMgr
{
public:
    static AutoTravelMgr* instance();

    void LoadConfig();
    void Update(uint32 diff);

    bool Start(Player* player, uint32 uiMapId, float nx, float ny,
               bool hasCalib, float pnx, float pny, std::string const& name);
    void Teleport(Player* player, uint32 uiMapId, float nx, float ny,
                  bool hasCalib, float pnx, float pny, std::string const& name);
    void Resolve(Player* player, uint32 uiMapId, float nx, float ny,
                 bool hasCalib, float pnx, float pny);
    void Stop(Player* player, std::string const& reason, bool silent = false);
    void Repath(Player* player);
    void PrintStatus(Player* player);
    void SetOption(Player* player, std::string const& key, std::string const& value);
    void SetDebug(Player* player, bool on);

    bool IsActive(Player* player) const;

private:
    void UpdateSession(Player* player, ATSession& s, uint32 diff);

    bool CalculatePath(Player* player, ATSession& s);
    void LaunchChunk(Player* player, ATSession& s);
    void HaltMovement(Player* player, ATSession& s);
    void ReleaseControl(Player* player, ATSession& s);

    bool TryMount(Player* player, ATSession& s);
    uint32 PickGroundMount(Player* player) const;

    bool MapToWorld(Player* player, uint32 uiMapId, float nx, float ny,
                    bool hasCalib, float pnx, float pny,
                    float& outX, float& outY, std::string& err) const;

    bool ResolveWorld(Player* player, uint32 uiMapId, float nx, float ny,
                      bool hasCalib, float pnx, float pny,
                      float& x, float& y, float& z, uint32& mapId,
                      std::string& err) const;

    void PushStatus(Player* player, ATSession const& s);
    void Msg(Player* player, std::string const& text) const;
    void Dbg(Player* player, ATSession const& s, std::string const& text) const;

    float ArrivalDist(ATSession const& s) const
    {
        return s.arrivalOverride > 0.0f ? s.arrivalOverride : ATConf.arrivalDistance;
    }

    std::unordered_map<ObjectGuid, ATSession> _sessions;
    std::unordered_map<ObjectGuid, uint32> _tpCooldown;   // Unix-Zeit
    uint32 _tick = 0;
};

#define sAutoTravel AutoTravelMgr::instance()

#endif // MOD_AUTOTRAVEL_H
