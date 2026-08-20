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
    AT_PLAYER_PAUSED,
    AT_ARRIVED,
    AT_FAILED
};

enum ATLegFlags : uint8
{
    AT_LEG_NORMAL  = 0,
    AT_LEG_FLIGHT  = 1,    // Carbonite-Abschnitt "F": Flugmeister
    AT_LEG_SPECIAL = 2,    // TravelNode-Verbindung, die nicht gelaufen wird
};

// Reiseknoten aus der Playerbot-Datenbank (playerbots_travelnode).
// Die Koordinaten sind bereits Weltkoordinaten - keine Umrechnung noetig.
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

// Ein Stuetzpunkt der Carbonite-Route. Zwischen zwei Stuetzpunkten sucht das
// NavMesh den tatsaechlichen Weg -- Carbonite gibt nur die grobe Abfolge vor
// (Zonenuebergang, Torbogen, Bruecke, Flugpunkt, Ziel).
struct ATLeg
{
    uint32 uiMapId = 0;
    float  nx = 0.0f, ny = 0.0f;
    uint8  flags = AT_LEG_NORMAL;

    float  wx = 0.0f, wy = 0.0f, wz = 0.0f;
    bool   resolved = false;
    std::string name;

    uint8  linkType = 0;       // Art der Verbindung zum NAECHSTEN Punkt
    std::string nextName;      // Name des naechsten Punktes
};

char const* ATStateName(ATState s);
char const* LinkTypeNameFor(uint8 t);

struct ATConfig
{
    bool  enable            = true;
    float arrivalDistance   = 8.0f;
    float legDistance       = 15.0f;   // Radius fuer Zwischenstuetzpunkte
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
    bool  takeClientControl = false;   // Standard: der Spieler behaelt die Steuerung
    bool  steerDetect       = true;    // eigene Steuereingaben erkennen
    uint32 steerPauseMs     = 8000;    // danach uebernimmt die Reise wieder
    uint32 chunkPoints      = 12;
    bool  swim              = true;
    float swimSurfaceOffset = 1.2f;    // wie tief unter der Oberflaeche
    float minSwimDepth      = 2.0f;    // darunter ist es eine Pfuetze
    uint32 maxUnderwaterMs  = 45000;   // Notbremse, falls doch getaucht wird
    bool  rescueUnderMesh   = true;
    float underMeshDepth    = 2.5f;    // ab so viel unter dem Boden gilt es als durchgefallen
    float aboveMeshHeight   = 6.0f;    // ab so viel darueber gilt er als haengend/fallend
    bool  useTravelNodes    = true;
    std::string nodeDb      = "acore_playerbots";
    float nodeSearchRadius  = 800.0f;
    float nodeMinDistance   = 300.0f;
    bool  useSpecialLinks   = true;
    float specialLinkCost   = 400.0f;
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
    float   destX       = 0.0f;    // aktuelles Etappenziel
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

    // arrival tolerance override (0 = use global)
    float   arrivalOverride = 0.0f;
    uint32  graceOverride   = 0;      // ms, 0 = Konfigurationswert
    int8    controlOverride = -1;     // -1 = Konfiguration, 0 = aus, 1 = an
    bool    playerPaused    = false;  // ausdruecklich angefordert (.at pause 1)
    uint32  inputWaitMs     = 0;      // Wartezeit pro Charakter, 0 = Konfiguration
    bool    pausedBySteer   = false;  // ausgeloest durch eigene Steuereingabe
    uint32  steerIdle       = 0;      // wie lange schon keine Eingabe mehr
    uint32  launchGuard     = 0;      // kurz nach dem Start keine Erkennung

    uint32  underwaterTimer = 0;
    bool    swimming        = false;
    uint8   offMeshHits     = 0;
    float   lastOffMeshZ    = 0.0f;
    uint32  rescueCount     = 0;

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
    void LoadMapAreas();
    void LoadTravelNodes();

    // Route ueber den Playerbot-Knotengraphen. false = kein Weg gefunden.
    bool BuildNodeRoute(Player* player, float dx, float dy, float dz,
                        std::vector<ATLeg>& out, std::string& note) const;
    void NodeInfo(Player* player);
    size_t NodeCount() const;
    void Update(uint32 diff);

    bool Start(Player* player, uint32 uiMapId, float nx, float ny,
               bool hasCalib, float pnx, float pny, std::string const& name);

    // Routenaufbau: erst RouteAdd (ggf. mehrfach), dann RouteStart.
    void RouteAdd(Player* player, bool clearFirst, std::string const& packed);
    bool RouteStart(Player* player, std::string const& name);
    void Teleport(Player* player, uint32 uiMapId, float nx, float ny,
                  bool hasCalib, float pnx, float pny, std::string const& name);
    void Resolve(Player* player, uint32 uiMapId, float nx, float ny,
                 bool hasCalib, float pnx, float pny);

    // Lernt die Zuordnung Client-Karten-ID -> WorldMapArea-ID anhand der
    // eigenen Position. Funktioniert auch, wenn das Ziel woanders liegt.
    void LearnMapId(Player* player, uint32 clientMapId, float pnx, float pny);

    // Ausfuehrliche Diagnose zu einem Ziel, ohne loszulaufen.
    void Diagnose(Player* player, uint32 uiMapId, float nx, float ny,
                  bool hasCalib, float pnx, float pny);
    void Stop(Player* player, std::string const& reason, bool silent = false);
    void Repath(Player* player);
    void PrintStatus(Player* player);
    void SetOption(Player* player, std::string const& key, std::string const& value);
    void SetPlayerPause(Player* player, bool on, bool bySteer = false);
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
                 Movement::PointsArray& out, uint32& typeOut, bool& incomplete) const;

    // Beste plausible Oberflaeche an x/y -- beruecksichtigt auch Gebaeude,
    // Bruecken und Stadtboeden, nicht nur das Rohgelaende.
    float BestGroundZ(Player* player, float x, float y) const;

    // Wasseroberflaeche an x/y. Rueckgabe false = kein nennenswertes Wasser.
    bool  WaterSurface(Player* player, float x, float y, float probeZ, float& level) const;

    // Hoehe, auf der sich der Charakter dort bewegen soll: Boden, oder knapp
    // unter der Wasseroberflaeche, wenn dort geschwommen wird.
    float TravelZ(Player* player, float x, float y, float groundZ) const;
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
    std::unordered_map<ObjectGuid, std::vector<ATLeg>> _pendingRoutes;
    uint32 _tick = 0;
};

#define sAutoTravel AutoTravelMgr::instance()

#endif // MOD_AUTOTRAVEL_H
