#ifndef MOD_AUTOTRAVEL_H
#define MOD_AUTOTRAVEL_H

#include "Common.h"
#include "ObjectGuid.h"
#include "MoveSplineInitArgs.h"
#include <string>
#include <cstddef>
#include <unordered_map>
#include <vector>

class Player;

enum ATState : uint8
{
    AT_IDLE = 0,
    AT_CALCULATE_PATH,
    AT_TRAVELING,
    AT_COMBAT_PAUSED,
    AT_MOUNTING,
    AT_WAIT_FLIGHT,
    AT_ARRIVED,
    AT_FAILED
};

enum ATLegFlags : uint8
{
    AT_LEG_NORMAL  = 0,
    AT_LEG_FLIGHT  = 1,
    AT_LEG_SPECIAL = 2,
};

struct ATNode
{
    uint32 id = 0;
    uint32 mapId = 0;
    float  x = 0.0f, y = 0.0f, z = 0.0f;
    std::string name;
};

struct ATNodeLink
{
    uint32 to = 0;
    uint8  type = 0;
    float  cost = 0.0f;
};

struct ATLeg
{
    uint32 uiMapId = 0;
    float  nx = 0.0f, ny = 0.0f;
    uint8  flags = AT_LEG_NORMAL;

    float  wx = 0.0f, wy = 0.0f, wz = 0.0f;
    bool   resolved = false;
    std::string name;
    uint8  linkType = 0;
    std::string nextName;
};

char const* ATStateName(ATState s);
char const* LinkTypeNameFor(uint8 t);

struct ATConfig
{
    bool  enable            = true;
    float arrivalDistance   = 8.0f;
    float legDistance       = 15.0f;
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
    bool  swim              = true;
    float swimSurfaceOffset = 1.2f;
    float minSwimDepth      = 2.0f;
    uint32 maxUnderwaterMs  = 45000;
    bool  rescueUnderMesh   = true;
    float underMeshDepth    = 2.5f;
    float aboveMeshHeight   = 12.0f;
    bool  useTravelNodes    = true;
    std::string nodeDb      = "acore_playerbots";
    float nodeSearchRadius  = 800.0f;
    float skipDetourFactor  = 1.25f;   // ab diesem Umwegfaktor Startknoten ueberspringen
    float nodeMinDistance   = 300.0f;
    bool  useSpecialLinks   = true;
    float specialLinkCost   = 400.0f;
    uint32 teleportSecurity = 2;       // 0=Spieler 1=Moderator 2=GM 3=Admin
    bool  allowTeleport     = true;
    float teleportMinDist   = 0.0f;
    uint32 teleportCooldown = 5;

    /*
     * Natural navigation.
     *
     * Distance remains the base cost. Terrain preferences are
     * additional costs, so a small hill is still preferable to
     * an enormous detour.
     */
    bool  naturalPathing          = true;

    float slopeStart              = 0.15f;
    float slopeStrong             = 0.25f;
    float slopeExtreme            = 0.35f;

    float slopePenalty            = 5.0f;
    float steepSlopePenalty       = 10.0f;
    float extremeSlopePenalty     = 30.0f;

    /*
     * Sustained elevation change.
     *
     * This detects "we are climbing a mountain" rather than
     * merely detecting individual small slopes.
     */
    float elevationWindow         = 40.0f;
    float elevationGainStart      = 8.0f;
    float elevationGainStrong     = 15.0f;
    float elevationGainExtreme    = 25.0f;

    float elevationPenalty        = 5.0f;
    float strongElevationPenalty  = 20.0f;
    float extremeElevationPenalty = 60.0f;

    /*
     * Natural turning.
     */
    float turnPenaltyStart        = 35.0f;
    float turnPenaltyStrong       = 70.0f;
    float turnPenaltyExtreme      = 110.0f;

    float turnPenalty             = 2.0f;
    float strongTurnPenalty       = 5.0f;
    float extremeTurnPenalty      = 12.0f;

    /*
     * Incomplete paths are only a fallback.
     */
    float incompletePathPenalty   = 10000.0f;

    /*
     * Contour probing.
     *
     * When the selected route looks like a mountain climb, the
     * bot searches for routes around it.
     *
     * Two probe points are used per contour candidate:
     *
     *       A
     *        \
     *         P1
     *          \
     *           P2
     *            \
     *             B
     *
     * Four candidates are tested:
     *
     *   left  / narrow
     *   left  / wide
     *   right / narrow
     *   right / wide
     *
     * = 8 intermediate probe points in total.
     */
    bool  contourProbing             = true;

    float contourTriggerElevation    = 15.0f;
    float contourTriggerSlope        = 0.20f;

    float contourNarrowOffset        = 100.0f;
    float contourWideOffset          = 180.0f;

    float contourFirstProgress       = 0.35f;
    float contourSecondProgress      = 0.65f;

    /*
     * A contour route is allowed to be longer than the direct route,
     * but only by this factor before terrain preference is considered.
     */
    float contourMaxDistanceFactor   = 2.5f;

    bool  debug                      = false;
    float terrainStep = 3.0f;
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

    std::vector<ATLeg> route;
    size_t  legIdx      = 0;
    bool    wasInFlight = false;

    Movement::PointsArray path;
    size_t  idx         = 0;
    bool    pathIncomplete = false;
    uint32  lastPathType   = 0;
    bool    controlTaken   = false;
    bool    debug          = false;

    float   arrivalOverride = 0.0f;
    uint32  graceOverride   = 0;

    uint32  underwaterTimer = 0;
    bool    swimming        = false;
    uint8   offMeshHits     = 0;
    float   lastOffMeshZ    = 0.0f;
    uint32  rescueCount     = 0;

    float   lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;
    uint32  stuckTimer = 0;

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
    void LoadMapAreas();
    void LoadTravelNodes();

    bool BuildNodeRoute(Player* player, float dx, float dy, float dz,
                        std::vector<ATLeg>& out, std::string& note) const;

    void NodeInfo(Player* player);
    size_t NodeCount() const;
    void Update(uint32 diff);

    bool Start(Player* player, uint32 uiMapId, float nx, float ny,
               bool hasCalib, float pnx, float pny, std::string const& name);

    void RouteAdd(Player* player, bool clearFirst, std::string const& packed);
    bool RouteStart(Player* player, std::string const& name);

    void Teleport(Player* player, uint32 uiMapId, float nx, float ny,
                  bool hasCalib, float pnx, float pny,
                  std::string const& name);

    void Resolve(Player* player, uint32 uiMapId, float nx, float ny,
                 bool hasCalib, float pnx, float pny);

    void LearnMapId(Player* player, uint32 clientMapId, float pnx, float pny);

    void Diagnose(Player* player, uint32 uiMapId, float nx, float ny,
                  bool hasCalib, float pnx, float pny);

    void Stop(Player* player, std::string const& reason, bool silent = false);
    void Repath(Player* player);
    void PrintStatus(Player* player);
    void SetOption(Player* player, std::string const& key,
                   std::string const& value);
    void SetDebug(Player* player, bool on);

    bool IsActive(Player* player) const;

private:
    void UpdateSession(Player* player, ATSession& s, uint32 diff);

    bool CalculatePath(Player* player, ATSession& s);
    bool BeginTravel(Player* player, ATSession& s);
    void ApplyNodeRouting(Player* player, ATSession& s);
    bool SetLegTarget(Player* player, ATSession& s);
    bool AdvanceLeg(Player* player, ATSession& s);

    bool TryPath(Player* player, float x, float y, float z, bool straight,
                 Movement::PointsArray& out, uint32& typeOut,
                 bool& incomplete) const;

    /*
     * Same PathGenerator, but with an explicit start position.
     *
     * This is what makes contour probing possible:
     *
     *   player -> probe1
     *   probe1 -> probe2
     *   probe2 -> destination
     */
    bool TryPathBetween(Player* player,
                        float startX, float startY, float startZ,
                        float destX, float destY, float destZ,
                        bool straight,
                        Movement::PointsArray& out,
                        uint32& typeOut,
                        bool& incomplete) const;

    float PathDistance(Movement::PointsArray const& path) const;

    float ScoreNaturalPath(Player* player,
                           Movement::PointsArray const& path,
                           bool incomplete) const;

    bool HasMountainClimb(Movement::PointsArray const& path) const;

    bool BuildContourCandidate(
        Player* player,
        ATSession const& s,
        float offset,
        bool left,
        Movement::PointsArray& out,
        uint32& typeOut,
        bool& incomplete,
        float& score) const;

    float BestGroundZ(Player* player, float x, float y) const;
    
    std::vector<float> FindGroundPlanes(
        Player* player,
        float x,
        float y,
        float probeZ) const;

    float SelectGroundPlane(
        std::vector<float> const& planes,
        float referenceZ,
        float targetZ,
        float horizontalDistance) const;

    bool WaterSurface(Player* player, float x, float y,
                      float probeZ, float& level) const;

    float TravelZ(Player* player, float x, float y,
                  float groundZ) const;

    void LaunchChunk(Player* player, ATSession& s);
    void HaltMovement(Player* player, ATSession& s);
    void ReleaseControl(Player* player, ATSession& s);

    bool TryMount(Player* player, ATSession& s);
    uint32 PickGroundMount(Player* player) const;

    bool MapToWorld(Player* player, uint32 uiMapId,
                    float nx, float ny,
                    bool hasCalib, float pnx, float pny,
                    float& outX, float& outY,
                    std::string& err) const;

    bool ResolveWorld(Player* player, uint32 uiMapId,
                      float nx, float ny,
                      bool hasCalib, float pnx, float pny,
                      float& x, float& y, float& z,
                      uint32& mapId,
                      std::string& err) const;

    void PushStatus(Player* player, ATSession const& s);
    void Msg(Player* player, std::string const& text) const;
    void Dbg(Player* player, ATSession const& s,
             std::string const& text) const;

    float ArrivalDist(ATSession const& s) const
    {
        return s.arrivalOverride > 0.0f
            ? s.arrivalOverride
            : ATConf.arrivalDistance;
    }

    std::unordered_map<ObjectGuid, ATSession> _sessions;
    std::unordered_map<ObjectGuid, uint32> _tpCooldown;
    std::unordered_map<ObjectGuid, std::vector<ATLeg>> _pendingRoutes;

    uint32 _tick = 0;
};

#define sAutoTravel AutoTravelMgr::instance()

#endif // MOD_AUTOTRAVEL_H