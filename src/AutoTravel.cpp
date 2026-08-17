#include "AutoTravel.h"

#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "GridDefines.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "World.h"

#include <cmath>
#include <cstdio>
#include <ctime>

ATConfig ATConf;

char const* ATStateName(ATState s)
{
    switch (s)
    {
        case AT_IDLE:           return "IDLE";
        case AT_CALCULATE_PATH: return "REPATHING";
        case AT_TRAVELING:      return "TRAVELING";
        case AT_COMBAT_PAUSED:  return "PAUSED - COMBAT";
        case AT_MOUNTING:       return "MOUNTING";
        case AT_ARRIVED:        return "ARRIVED";
        case AT_FAILED:         return "FAILED";
    }
    return "UNKNOWN";
}

AutoTravelMgr* AutoTravelMgr::instance()
{
    static AutoTravelMgr inst;
    return &inst;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void AutoTravelMgr::LoadConfig()
{
    ATConf.enable            = sConfigMgr->GetOption<bool>  ("AutoTravel.Enable", true);
    ATConf.arrivalDistance   = sConfigMgr->GetOption<float> ("AutoTravel.ArrivalDistance", 8.0f);
    ATConf.autoMount         = sConfigMgr->GetOption<bool>  ("AutoTravel.AutoMount", true);
    ATConf.mountMinDistance  = sConfigMgr->GetOption<float> ("AutoTravel.MountMinDistance", 150.0f);
    ATConf.pauseInCombat     = sConfigMgr->GetOption<bool>  ("AutoTravel.PauseInCombat", true);
    ATConf.resumeAfterCombat = sConfigMgr->GetOption<bool>  ("AutoTravel.ResumeAfterCombat", true);
    ATConf.combatGraceMs     = sConfigMgr->GetOption<uint32>("AutoTravel.CombatGraceMs", 1500);
    ATConf.stuckDetection    = sConfigMgr->GetOption<bool>  ("AutoTravel.StuckDetection", true);
    ATConf.stuckTimeoutMs    = sConfigMgr->GetOption<uint32>("AutoTravel.StuckTimeoutMs", 5000);
    ATConf.stuckMinDistance  = sConfigMgr->GetOption<float> ("AutoTravel.StuckMinDistance", 3.0f);
    ATConf.maxRepathAttempts = sConfigMgr->GetOption<uint32>("AutoTravel.MaxRepathAttempts", 8);
    ATConf.resumeAfterDeath  = sConfigMgr->GetOption<bool>  ("AutoTravel.ResumeAfterDeath", false);
    ATConf.takeClientControl = sConfigMgr->GetOption<bool>  ("AutoTravel.TakeClientControl", true);
    ATConf.chunkPoints       = sConfigMgr->GetOption<uint32>("AutoTravel.ChunkPoints", 12);
    ATConf.allowTeleport     = sConfigMgr->GetOption<bool>  ("AutoTravel.AllowTeleport", true);
    ATConf.teleportMinDist   = sConfigMgr->GetOption<float> ("AutoTravel.TeleportMinDistance", 0.0f);
    ATConf.teleportCooldown  = sConfigMgr->GetOption<uint32>("AutoTravel.TeleportCooldownSec", 5);
    ATConf.debug             = sConfigMgr->GetOption<bool>  ("AutoTravel.Debug", false);

    if (ATConf.chunkPoints < 2)  ATConf.chunkPoints = 2;
    if (ATConf.chunkPoints > 60) ATConf.chunkPoints = 60;
}

// ---------------------------------------------------------------------------
// Messaging (Protokoll zum Addon laeuft ueber Systemnachrichten mit [AT]-Prefix)
// ---------------------------------------------------------------------------

void AutoTravelMgr::Msg(Player* player, std::string const& text) const
{
    if (!player || !player->GetSession())
        return;
    ChatHandler(player->GetSession()).SendSysMessage(("[AT]M|" + text).c_str());
}

void AutoTravelMgr::Dbg(Player* player, ATSession const& s, std::string const& text) const
{
    if (!ATConf.debug && !s.debug)
        return;
    if (!player || !player->GetSession())
        return;
    ChatHandler(player->GetSession()).SendSysMessage(("[AT]D|" + text).c_str());
}

void AutoTravelMgr::PushStatus(Player* player, ATSession const& s)
{
    if (!player || !player->GetSession())
        return;

    float dist = std::sqrt(
        (player->GetPositionX() - s.destX) * (player->GetPositionX() - s.destX) +
        (player->GetPositionY() - s.destY) * (player->GetPositionY() - s.destY));

    char buf[512];
    std::snprintf(buf, sizeof(buf), "[AT]S|%s|%.0f|%s|%u|%u|%u",
                  ATStateName(s.state),
                  dist,
                  s.destName.empty() ? "-" : s.destName.c_str(),
                  player->IsMounted() ? 1u : 0u,
                  uint32(s.path.size()),
                  s.repathAttempts);

    ChatHandler(player->GetSession()).SendSysMessage(buf);
}

// ---------------------------------------------------------------------------
// Kartenkoordinaten -> Weltkoordinaten (WorldMapArea.dbc)
// ---------------------------------------------------------------------------

bool AutoTravelMgr::MapToWorld(Player* player, uint32 uiMapId, float nx, float ny,
                               bool hasCalib, float pnx, float pny,
                               float& outX, float& outY, std::string& err) const
{
    WorldMapAreaEntry const* e = sWorldMapAreaStore.LookupEntry(uiMapId);
    if (!e)
    {
        err = "Unbekannte WorldMapArea-ID " + std::to_string(uiMapId) + ".";
        return false;
    }

    if (e->map_id != player->GetMapId())
    {
        err = "Das Ziel liegt auf einer anderen Karte (Map " + std::to_string(e->map_id) +
              "). Kontinentwechsel wird nicht unterstuetzt.";
        return false;
    }

    // Standardtransformation:
    //   horizontale Kartenachse  -> Welt-Y   (y1 = links,  y2 = rechts)
    //   vertikale  Kartenachse   -> Welt-X   (x1 = oben,   x2 = unten)
    auto variantA = [&](float mx, float my, float& wx, float& wy)
    {
        wy = e->y1 + mx * (e->y2 - e->y1);
        wx = e->x1 + my * (e->x2 - e->x1);
    };
    // Fallback fuer abweichende DBC-Feldreihenfolge in exotischen Forks.
    auto variantB = [&](float mx, float my, float& wx, float& wy)
    {
        wx = e->y1 + mx * (e->y2 - e->y1);
        wy = e->x1 + my * (e->x2 - e->x1);
    };

    if (hasCalib && pnx > 0.0f && pny > 0.0f)
    {
        float ax, ay, bx, by;
        variantA(pnx, pny, ax, ay);
        variantB(pnx, pny, bx, by);

        float ea = std::sqrt(std::pow(ax - player->GetPositionX(), 2) +
                             std::pow(ay - player->GetPositionY(), 2));
        float eb = std::sqrt(std::pow(bx - player->GetPositionX(), 2) +
                             std::pow(by - player->GetPositionY(), 2));

        if (ea > 400.0f && eb > 400.0f)
        {
            char b[256];
            std::snprintf(b, sizeof(b),
                "Koordinaten-Kalibrierung fehlgeschlagen (Abweichung %.0f/%.0f yd). "
                "Bitte die Weltkarte auf die Zielzone stellen und erneut starten.", ea, eb);
            err = b;
            return false;
        }

        if (eb < ea)
            variantB(nx, ny, outX, outY);
        else
            variantA(nx, ny, outX, outY);
        return true;
    }

    variantA(nx, ny, outX, outY);
    return true;
}


// ---------------------------------------------------------------------------
// Zielaufloesung: Kartenkoordinaten -> Weltkoordinaten inklusive Bodenhoehe
// ---------------------------------------------------------------------------

bool AutoTravelMgr::ResolveWorld(Player* player, uint32 uiMapId, float nx, float ny,
                                 bool hasCalib, float pnx, float pny,
                                 float& x, float& y, float& z, uint32& mapId,
                                 std::string& err) const
{
    if (nx < 0.0f || nx > 1.0f || ny < 0.0f || ny > 1.0f)
    {
        err = "Ungueltige Zielkoordinaten vom Addon erhalten.";
        return false;
    }

    if (!MapToWorld(player, uiMapId, nx, ny, hasCalib, pnx, pny, x, y, err))
        return false;

    mapId = player->GetMapId();

    Map* map = player->GetMap();
    z = map->GetHeight(player->GetPhaseMask(), x, y, MAX_HEIGHT);
    if (z <= INVALID_HEIGHT)
        z = map->GetHeight(player->GetPhaseMask(), x, y, player->GetPositionZ() + 100.0f);
    if (z <= INVALID_HEIGHT)
    {
        err = "Fuer diese Position ist keine Hoehendaten verfuegbar (fehlende vmaps?).";
        return false;
    }
    return true;
}

void AutoTravelMgr::Resolve(Player* player, uint32 uiMapId, float nx, float ny,
                            bool hasCalib, float pnx, float pny)
{
    float x, y, z;
    uint32 mapId;
    std::string err;
    if (!ResolveWorld(player, uiMapId, nx, ny, hasCalib, pnx, pny, x, y, z, mapId, err))
    {
        Msg(player, err);
        return;
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf), "[AT]W|%u|%.3f|%.3f|%.3f", mapId, x, y, z);
    if (player->GetSession())
        ChatHandler(player->GetSession()).SendSysMessage(buf);
}

// ---------------------------------------------------------------------------
// Teleport (bewusst getrennt vom Reisebetrieb)
// ---------------------------------------------------------------------------

void AutoTravelMgr::Teleport(Player* player, uint32 uiMapId, float nx, float ny,
                             bool hasCalib, float pnx, float pny, std::string const& name)
{
    if (!ATConf.allowTeleport)
    {
        Msg(player, "Teleport ist auf diesem Server deaktiviert.");
        return;
    }
    if (player->IsInCombat())
    {
        Msg(player, "Im Kampf ist kein Teleport moeglich.");
        return;
    }
    if (!player->IsAlive())
    {
        Msg(player, "Du bist tot.");
        return;
    }
    if (player->IsInFlight() || player->GetVehicle() || player->IsBeingTeleported())
    {
        Msg(player, "Jetzt gerade nicht moeglich (Flug/Fahrzeug/Teleport).");
        return;
    }
    if (player->GetMap()->IsBattlegroundOrArena() || player->InBattleground())
    {
        Msg(player, "In Schlachtfeldern und Arenen nicht erlaubt.");
        return;
    }

    uint32 nowSec = uint32(time(nullptr));
    auto cd = _tpCooldown.find(player->GetGUID());
    if (cd != _tpCooldown.end() && nowSec < cd->second)
    {
        char b[96];
        std::snprintf(b, sizeof(b), "Noch %u Sekunden Abklingzeit.", cd->second - nowSec);
        Msg(player, b);
        return;
    }

    float x, y, z;
    uint32 mapId;
    std::string err;
    if (!ResolveWorld(player, uiMapId, nx, ny, hasCalib, pnx, pny, x, y, z, mapId, err))
    {
        Msg(player, err);
        return;
    }

    float dist = player->GetExactDist2d(x, y);
    if (ATConf.teleportMinDist > 0.0f && dist < ATConf.teleportMinDist)
    {
        char b[128];
        std::snprintf(b, sizeof(b), "Das Ziel ist nur %.0f yd entfernt - lauf hin.", dist);
        Msg(player, b);
        return;
    }

    // Laufende Reise sauber beenden
    auto it = _sessions.find(player->GetGUID());
    if (it != _sessions.end() && it->second.state != AT_IDLE)
    {
        HaltMovement(player, it->second);
        it->second.state = AT_IDLE;
        _sessions.erase(it);
    }

    _tpCooldown[player->GetGUID()] = nowSec + ATConf.teleportCooldown;

    player->TeleportTo(mapId, x, y, z + 0.5f, player->GetOrientation());

    char buf[192];
    std::snprintf(buf, sizeof(buf), "Teleport zu %s (%.1f / %.1f / %.1f), %.0f yd.",
                  name.empty() ? "Ziel" : name.c_str(), x, y, z, dist);
    Msg(player, buf);
}

// ---------------------------------------------------------------------------
// Start / Stop / Repath
// ---------------------------------------------------------------------------

bool AutoTravelMgr::IsActive(Player* player) const
{
    if (!player)
        return false;
    auto it = _sessions.find(player->GetGUID());
    return it != _sessions.end() && it->second.state != AT_IDLE;
}

bool AutoTravelMgr::Start(Player* player, uint32 uiMapId, float nx, float ny,
                          bool hasCalib, float pnx, float pny, std::string const& name)
{
    if (!ATConf.enable)
    {
        Msg(player, "AutoTravel ist auf diesem Server deaktiviert.");
        return false;
    }

    if (!sWorld->getBoolConfig(CONFIG_ENABLE_MMAPS))
    {
        Msg(player, "Serverseitiges Pathfinding (mmaps) ist deaktiviert. AutoTravel kann nicht arbeiten.");
        return false;
    }

    if (!player->IsAlive())
    {
        Msg(player, "Du bist tot.");
        return false;
    }

    if (player->IsInFlight() || player->GetVehicle() || player->IsBeingTeleported())
    {
        Msg(player, "AutoTravel kann jetzt nicht gestartet werden (Flug/Fahrzeug/Teleport).");
        return false;
    }

    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    uint32 tmpMap = 0;
    std::string err;
    if (!ResolveWorld(player, uiMapId, nx, ny, hasCalib, pnx, pny, wx, wy, wz, tmpMap, err))
    {
        Msg(player, err);
        return false;
    }

    ATSession& s = _sessions[player->GetGUID()];

    bool wasDebug = s.debug;
    s = ATSession();
    s.debug     = wasDebug;
    s.mapId     = player->GetMapId();
    s.destX     = wx;
    s.destY     = wy;
    s.destZ     = wz;
    s.destName  = name.empty() ? "Ziel" : name;
    s.state     = AT_CALCULATE_PATH;
    s.lastX     = player->GetPositionX();
    s.lastY     = player->GetPositionY();
    s.lastZ     = player->GetPositionZ();

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Ziel uebernommen: %s (Map %u | X %.2f Y %.2f Z %.2f)",
                  s.destName.c_str(), s.mapId, s.destX, s.destY, s.destZ);
    Dbg(player, s, buf);

    Msg(player, "Reise gestartet: " + s.destName);
    PushStatus(player, s);
    return true;
}

void AutoTravelMgr::Stop(Player* player, std::string const& reason, bool silent)
{
    auto it = _sessions.find(player->GetGUID());
    if (it == _sessions.end())
    {
        if (!silent)
            Msg(player, "AutoTravel ist nicht aktiv.");
        return;
    }

    ATSession& s = it->second;
    HaltMovement(player, s);
    s.state = AT_IDLE;
    PushStatus(player, s);
    if (!silent)
        Msg(player, reason.empty() ? "Reise gestoppt." : reason);
    _sessions.erase(it);
}

void AutoTravelMgr::Repath(Player* player)
{
    auto it = _sessions.find(player->GetGUID());
    if (it == _sessions.end())
    {
        Msg(player, "AutoTravel ist nicht aktiv.");
        return;
    }

    ATSession& s = it->second;
    if (s.state == AT_COMBAT_PAUSED)
    {
        Msg(player, "Repathing wird nach dem Kampf ausgefuehrt.");
        return;
    }

    HaltMovement(player, s);
    s.path.clear();
    s.idx = 0;
    s.state = AT_CALCULATE_PATH;
    Msg(player, "Pfad wird neu berechnet.");
    PushStatus(player, s);
}

void AutoTravelMgr::PrintStatus(Player* player)
{
    auto it = _sessions.find(player->GetGUID());
    if (it == _sessions.end())
    {
        Msg(player, "Status: IDLE");
        return;
    }
    ATSession const& s = it->second;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Status: %s | Ziel: %s | Pfadpunkte: %u/%u | Versuche: %u",
                  ATStateName(s.state), s.destName.c_str(),
                  uint32(s.idx), uint32(s.path.size()), s.repathAttempts);
    Msg(player, buf);
    PushStatus(player, const_cast<ATSession&>(s));
}

void AutoTravelMgr::SetDebug(Player* player, bool on)
{
    _sessions[player->GetGUID()].debug = on;
    Msg(player, on ? "Debug-Modus aktiv." : "Debug-Modus aus.");
    if (_sessions[player->GetGUID()].state == AT_IDLE && !on)
        _sessions.erase(player->GetGUID());
}

void AutoTravelMgr::SetOption(Player* player, std::string const& key, std::string const& value)
{
    ATSession& s = _sessions[player->GetGUID()];
    if (key == "arrival")
    {
        float v = float(atof(value.c_str()));
        if (v < 2.0f || v > 100.0f)
        {
            Msg(player, "Arrival Distance muss zwischen 2 und 100 liegen.");
            return;
        }
        s.arrivalOverride = v;
        Msg(player, "Arrival Distance = " + value);
    }
    else
        Msg(player, "Unbekannte Option: " + key);

    if (s.state == AT_IDLE && s.arrivalOverride <= 0.0f && !s.debug)
        _sessions.erase(player->GetGUID());
}

// ---------------------------------------------------------------------------
// Bewegung
// ---------------------------------------------------------------------------

void AutoTravelMgr::ReleaseControl(Player* player, ATSession& s)
{
    if (s.controlTaken)
    {
        player->SetClientControl(player, true);
        s.controlTaken = false;
    }
}

void AutoTravelMgr::HaltMovement(Player* player, ATSession& s)
{
    player->StopMoving();
    ReleaseControl(player, s);
}

void AutoTravelMgr::LaunchChunk(Player* player, ATSession& s)
{
    if (s.idx >= s.path.size())
        return;

    Movement::PointsArray chunk;
    chunk.push_back(G3D::Vector3(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()));

    uint32 n = 0;
    while (s.idx < s.path.size() && n < ATConf.chunkPoints)
    {
        chunk.push_back(s.path[s.idx]);
        ++s.idx;
        ++n;
    }

    if (chunk.size() < 2)
        return;

    if (ATConf.takeClientControl && !s.controlTaken)
    {
        player->SetClientControl(player, false);
        s.controlTaken = true;
    }

    Movement::MoveSplineInit init(player);
    init.MovebyPath(chunk);
    init.SetWalk(false);
    init.Launch();

    char buf[128];
    std::snprintf(buf, sizeof(buf), "Movement gestartet: %u Punkte (Index %u/%u)",
                  uint32(chunk.size()), uint32(s.idx), uint32(s.path.size()));
    Dbg(player, s, buf);
}

// ---------------------------------------------------------------------------
// Pathfinding (echtes AzerothCore NavMesh via PathGenerator)
// ---------------------------------------------------------------------------

bool AutoTravelMgr::CalculatePath(Player* player, ATSession& s)
{
    PathGenerator gen(player);
    gen.SetUseStraightPath(false);

    bool built = gen.CalculatePath(s.destX, s.destY, s.destZ, false);
    PathType type = gen.GetPathType();

    char buf[256];
    std::snprintf(buf, sizeof(buf), "PathGenerator: built=%u type=0x%X points=%u",
                  built ? 1u : 0u, uint32(type), uint32(gen.GetPath().size()));
    Dbg(player, s, buf);

    // PATHFIND_NOPATH liefert nur eine Geradeaus-Notloesung -> nicht akzeptieren.
    if (!built || (type & PATHFIND_NOPATH) || gen.GetPath().size() < 2)
        return false;

    // Reiner Shortcut ohne NavMesh nur ueber sehr kurze Distanz zulassen.
    if ((type & PATHFIND_SHORTCUT) && !(type & PATHFIND_NORMAL))
    {
        float d = player->GetExactDist2d(s.destX, s.destY);
        if (d > 40.0f)
        {
            Dbg(player, s, "Shortcut ueber grosse Distanz verworfen.");
            return false;
        }
    }

    s.path = gen.GetPath();
    s.idx  = 1;   // Punkt 0 ist die aktuelle Position
    s.pathIncomplete = (type & PATHFIND_INCOMPLETE) != 0;

    return true;
}

// ---------------------------------------------------------------------------
// Mount
// ---------------------------------------------------------------------------

uint32 AutoTravelMgr::PickGroundMount(Player* player) const
{
    uint32 best = 0;
    int32 bestSpeed = -1;

    for (auto const& pair : player->GetSpellMap())
    {
        if (pair.second->State == PLAYERSPELL_REMOVED || !pair.second->Active)
            continue;

        SpellInfo const* si = sSpellMgr->GetSpellInfo(pair.first);
        if (!si)
            continue;

        bool mounted = false;
        bool flying  = false;
        int32 speed  = 0;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            switch (si->Effects[i].ApplyAuraName)
            {
                case SPELL_AURA_MOUNTED:
                    mounted = true;
                    break;
                case SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED:
                    speed = std::max<int32>(speed, si->Effects[i].BasePoints);
                    break;
                case SPELL_AURA_MOD_FLIGHT_SPEED_MOUNTED:
                    flying = true;
                    break;
                default:
                    break;
            }
        }

        if (!mounted || flying)
            continue;

        if (speed > bestSpeed)
        {
            bestSpeed = speed;
            best = si->Id;
        }
    }

    return best;
}

bool AutoTravelMgr::TryMount(Player* player, ATSession& s)
{
    if (!ATConf.autoMount || player->IsMounted() || player->IsInCombat())
        return false;
    if (!player->IsOutdoors() || player->IsInWater() || player->IsInFlight())
        return false;
    if (player->GetMap()->IsBattlegroundOrArena() || player->GetMap()->IsDungeon())
        return false;
    if (player->GetExactDist2d(s.destX, s.destY) < ATConf.mountMinDistance)
        return false;

    uint32 spellId = PickGroundMount(player);
    if (!spellId)
    {
        Dbg(player, s, "Kein geeignetes Bodenmount gefunden.");
        return false;
    }

    HaltMovement(player, s);

    // Bewusst NICHT triggered: alle normalen Prueflungen (Zone, Reitkunst,
    // Kampf, Wasser, Innenraum) greifen. Schlaegt es fehl, laufen wir zu Fuss.
    SpellCastResult res = player->CastSpell(player, spellId, false);
    if (res != SPELL_CAST_OK)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Mount-Cast abgelehnt (SpellCastResult %u) -> zu Fuss.", uint32(res));
        Dbg(player, s, buf);
        return false;
    }

    s.state = AT_MOUNTING;
    s.mountTimer = 0;
    Dbg(player, s, "Mount wird gewirkt.");
    return true;
}

// ---------------------------------------------------------------------------
// Haupt-Tick
// ---------------------------------------------------------------------------

void AutoTravelMgr::Update(uint32 diff)
{
    if (!ATConf.enable)
        return;

    _tick += diff;
    if (_tick < 200)
        return;

    uint32 d = _tick;
    _tick = 0;

    for (auto it = _sessions.begin(); it != _sessions.end(); )
    {
        Player* player = ObjectAccessor::FindPlayer(it->first);
        if (!player || !player->IsInWorld())
        {
            it = _sessions.erase(it);
            continue;
        }

        if (it->second.state == AT_IDLE)
        {
            // reine Options-/Debug-Session, nichts zu tun
            ++it;
            continue;
        }

        UpdateSession(player, it->second, d);

        if (it->second.state == AT_IDLE)
            it = _sessions.erase(it);
        else
            ++it;
    }
}

void AutoTravelMgr::UpdateSession(Player* player, ATSession& s, uint32 diff)
{
    // --- harte Abbruchbedingungen -------------------------------------------
    if (player->GetMapId() != s.mapId || player->IsBeingTeleported())
    {
        HaltMovement(player, s);
        Msg(player, "Karte gewechselt - Reise abgebrochen.");
        s.state = AT_IDLE;
        return;
    }

    if (!player->IsAlive())
    {
        HaltMovement(player, s);
        if (ATConf.resumeAfterDeath)
        {
            if (s.state != AT_COMBAT_PAUSED)
            {
                s.state = AT_COMBAT_PAUSED;   // wartet auf Wiederbelebung
                Msg(player, "Du bist gestorben. Ziel bleibt gespeichert.");
                PushStatus(player, s);
            }
            return;
        }
        Msg(player, "Du bist gestorben - Reise beendet.");
        s.state = AT_IDLE;
        return;
    }

    if (player->IsInFlight() || player->GetVehicle())
    {
        HaltMovement(player, s);
        return;
    }

    // --- Kampf ---------------------------------------------------------------
    if (ATConf.pauseInCombat && player->IsInCombat())
    {
        if (s.state != AT_COMBAT_PAUSED)
        {
            HaltMovement(player, s);   // gibt die Kontrolle sofort zurueck
            s.state = AT_COMBAT_PAUSED;
            s.combatTimer = 0;
            s.path.clear();
            s.idx = 0;
            Msg(player, "Kampf erkannt - Reise pausiert.");
            Dbg(player, s, "Ziel bleibt gespeichert: " + s.destName);
            PushStatus(player, s);
        }
        else
            s.combatTimer = 0;
        return;
    }

    // --- Statusausgabe -------------------------------------------------------
    s.statusTimer += diff;
    if (s.statusTimer >= 1000)
    {
        s.statusTimer = 0;
        PushStatus(player, s);
    }

    // --- Zielankunft ---------------------------------------------------------
    float dist = player->GetExactDist2d(s.destX, s.destY);
    if (dist <= ArrivalDist(s) && s.state != AT_MOUNTING)
    {
        HaltMovement(player, s);
        s.state = AT_ARRIVED;
        PushStatus(player, s);
        Msg(player, "Ziel erreicht - " + s.destName + ".");
        s.state = AT_IDLE;
        return;
    }

    switch (s.state)
    {
        // -------------------------------------------------------------------
        case AT_COMBAT_PAUSED:
        {
            if (!ATConf.resumeAfterCombat)
            {
                Msg(player, "Kampf beendet - Fortsetzung ist deaktiviert.");
                s.state = AT_IDLE;
                return;
            }
            s.combatTimer += diff;
            if (s.combatTimer < ATConf.combatGraceMs)
                return;

            Msg(player, "Kampf beendet - berechne neuen Pfad von der aktuellen Position.");
            s.state = AT_CALCULATE_PATH;
            s.repathAttempts = 0;
            s.mountTried = false;
            PushStatus(player, s);
            return;
        }

        // -------------------------------------------------------------------
        case AT_MOUNTING:
        {
            s.mountTimer += diff;
            if (player->IsMounted())
            {
                Dbg(player, s, "Mount aktiv.");
                s.state = AT_CALCULATE_PATH;
                return;
            }
            if (s.mountTimer > 6000 || !player->HasUnitState(UNIT_STATE_CASTING))
            {
                if (!player->IsMounted())
                {
                    Dbg(player, s, "Mounten fehlgeschlagen - weiter zu Fuss.");
                    s.state = AT_CALCULATE_PATH;
                }
            }
            return;
        }

        // -------------------------------------------------------------------
        case AT_CALCULATE_PATH:
        {
            if (ATConf.autoMount && !s.mountTried && !player->IsMounted())
            {
                s.mountTried = true;
                if (TryMount(player, s))
                    return;
            }

            if (!CalculatePath(player, s))
            {
                ++s.repathAttempts;
                if (s.repathAttempts >= ATConf.maxRepathAttempts)
                {
                    HaltMovement(player, s);
                    Msg(player, "Kein begehbarer Weg zum Ziel gefunden. Navigation gestoppt.");
                    s.state = AT_FAILED;
                    PushStatus(player, s);
                    s.state = AT_IDLE;
                    return;
                }
                char buf[160];
                std::snprintf(buf, sizeof(buf), "Pfadberechnung fehlgeschlagen (Versuch %u/%u).",
                              s.repathAttempts, ATConf.maxRepathAttempts);
                Msg(player, buf);
                return;   // naechster Tick versucht es erneut
            }

            s.repathAttempts = 0;
            s.stuckTimer = 0;
            s.lastX = player->GetPositionX();
            s.lastY = player->GetPositionY();
            s.lastZ = player->GetPositionZ();

            char buf[128];
            std::snprintf(buf, sizeof(buf), "Pfad gefunden. Nodes: %u%s",
                          uint32(s.path.size()), s.pathIncomplete ? " (Teilpfad)" : "");
            Dbg(player, s, buf);

            s.state = AT_TRAVELING;
            LaunchChunk(player, s);
            PushStatus(player, s);
            return;
        }

        // -------------------------------------------------------------------
        case AT_TRAVELING:
        {
            // Stuck-Erkennung
            if (ATConf.stuckDetection)
            {
                s.stuckTimer += diff;
                if (s.stuckTimer >= ATConf.stuckTimeoutMs)
                {
                    float moved = std::sqrt(std::pow(player->GetPositionX() - s.lastX, 2) +
                                            std::pow(player->GetPositionY() - s.lastY, 2) +
                                            std::pow(player->GetPositionZ() - s.lastZ, 2));
                    s.stuckTimer = 0;
                    s.lastX = player->GetPositionX();
                    s.lastY = player->GetPositionY();
                    s.lastZ = player->GetPositionZ();

                    if (moved < ATConf.stuckMinDistance)
                    {
                        ++s.repathAttempts;
                        char buf[160];
                        std::snprintf(buf, sizeof(buf),
                                      "STUCK erkannt (%.1f yd in %u ms). Repath %u/%u.",
                                      moved, ATConf.stuckTimeoutMs,
                                      s.repathAttempts, ATConf.maxRepathAttempts);
                        Msg(player, buf);

                        if (s.repathAttempts >= ATConf.maxRepathAttempts)
                        {
                            HaltMovement(player, s);
                            Msg(player, "Kein Fortschritt moeglich. Navigation gestoppt.");
                            s.state = AT_FAILED;
                            PushStatus(player, s);
                            s.state = AT_IDLE;
                            return;
                        }

                        HaltMovement(player, s);
                        s.path.clear();
                        s.idx = 0;
                        s.state = AT_CALCULATE_PATH;
                        return;
                    }
                }
            }

            // Spline noch aktiv?
            if (!player->movespline->Finalized())
                return;

            if (s.idx < s.path.size())
            {
                LaunchChunk(player, s);
                return;
            }

            // Teilpfad zu Ende gelaufen -> von hier aus neu rechnen
            s.path.clear();
            s.idx = 0;
            s.state = AT_CALCULATE_PATH;
            Dbg(player, s, "Teilstrecke beendet - neue Pfadberechnung.");
            return;
        }

        default:
            s.state = AT_IDLE;
            return;
    }
}
