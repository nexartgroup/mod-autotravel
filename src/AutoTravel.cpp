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
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>

ATConfig ATConf;

char const* LinkTypeNameFor(uint8 t)
{
    switch (t)
    {
        case 1:  return "Laufweg";
        case 2:  return "Portal";
        case 3:  return "Transport";
        case 4:  return "Flugroute";
        default: return "Sonderverbindung";
    }
}

char const* ATStateName(ATState s)
{
    switch (s)
    {
        case AT_IDLE:           return "IDLE";
        case AT_CALCULATE_PATH: return "REPATHING";
        case AT_TRAVELING:      return "TRAVELING";
        case AT_COMBAT_PAUSED:  return "PAUSED - COMBAT";
        case AT_MOUNTING:       return "MOUNTING";
        case AT_WAIT_FLIGHT:    return "WARTE AUF FLUG";
        case AT_ARRIVED:        return "ARRIVED";
        case AT_FAILED:         return "FAILED";
    }
    return "UNKNOWN";
}

namespace
{
    std::string PathTypeName(uint32 t)
    {
        std::string out;
        if (t & PATHFIND_NORMAL)         out += "NORMAL ";
        if (t & PATHFIND_SHORTCUT)       out += "SHORTCUT ";
        if (t & PATHFIND_INCOMPLETE)     out += "INCOMPLETE ";
        if (t & PATHFIND_NOPATH)         out += "NOPATH ";
        if (t & PATHFIND_NOT_USING_PATH) out += "NOT_USING_PATH ";
        if (t & PATHFIND_SHORT)          out += "SHORT ";
        if (out.empty()) out = "BLANK";
        return out;
    }
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
    ATConf.legDistance       = sConfigMgr->GetOption<float> ("AutoTravel.LegDistance", 15.0f);
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
    ATConf.contourProbing = sConfigMgr->GetOption<bool>("AutoTravel.ContourProbing", true);
    ATConf.contourTriggerElevation = sConfigMgr->GetOption<float>("AutoTravel.ContourTriggerElevation", 15.0f);
    ATConf.contourTriggerSlope = sConfigMgr->GetOption<float>("AutoTravel.ContourTriggerSlope", 0.20f);
    ATConf.contourNarrowOffset = sConfigMgr->GetOption<float>("AutoTravel.ContourNarrowOffset", 100.0f);
    ATConf.contourWideOffset = sConfigMgr->GetOption<float>("AutoTravel.ContourWideOffset", 180.0f);
    ATConf.contourFirstProgress = sConfigMgr->GetOption<float>("AutoTravel.ContourFirstProgress", 0.35f);
    ATConf.contourSecondProgress = sConfigMgr->GetOption<float>("AutoTravel.ContourSecondProgress", 0.65f);
    ATConf.contourMaxDistanceFactor = sConfigMgr->GetOption<float>("AutoTravel.ContourMaxDistanceFactor", 2.5f);
    ATConf.naturalPathing    = sConfigMgr->GetOption<bool>("AutoTravel.NaturalPathing", true);
    ATConf.slopeStart        = sConfigMgr->GetOption<float>("AutoTravel.SlopeStart", 0.15f);
    ATConf.slopeStrong       = sConfigMgr->GetOption<float>("AutoTravel.SlopeStrong", 0.25f);
    ATConf.slopeExtreme      = sConfigMgr->GetOption<float>("AutoTravel.SlopeExtreme", 0.35f);
    ATConf.slopePenalty      = sConfigMgr->GetOption<float>("AutoTravel.SlopePenalty", 5.0f);
    ATConf.steepSlopePenalty = sConfigMgr->GetOption<float>("AutoTravel.SteepSlopePenalty", 10.0f);
    ATConf.extremeSlopePenalty = sConfigMgr->GetOption<float>("AutoTravel.ExtremeSlopePenalty", 30.0f);
    ATConf.elevationWindow = sConfigMgr->GetOption<float>("AutoTravel.ElevationWindow", 40.0f);
    ATConf.elevationGainStart = sConfigMgr->GetOption<float>("AutoTravel.ElevationGainStart", 4.0f);
    ATConf.elevationGainStrong = sConfigMgr->GetOption<float>("AutoTravel.ElevationGainStrong", 10.0f);
    ATConf.elevationGainExtreme = sConfigMgr->GetOption<float>("AutoTravel.ElevationGainExtreme", 20.0f);
    ATConf.elevationPenalty = sConfigMgr->GetOption<float>("AutoTravel.ElevationPenalty", 3.0f);
    ATConf.strongElevationPenalty = sConfigMgr->GetOption<float>("AutoTravel.StrongElevationPenalty", 8.0f);
    ATConf.extremeElevationPenalty = sConfigMgr->GetOption<float>("AutoTravel.ExtremeElevationPenalty", 20.0f);
    ATConf.turnPenaltyStart = sConfigMgr->GetOption<float>("AutoTravel.TurnPenaltyStart", 35.0f);
    ATConf.turnPenaltyStrong = sConfigMgr->GetOption<float>("AutoTravel.TurnPenaltyStrong", 70.0f);
    ATConf.turnPenaltyExtreme = sConfigMgr->GetOption<float>("AutoTravel.TurnPenaltyExtreme", 110.0f);
    ATConf.turnPenalty = sConfigMgr->GetOption<float>("AutoTravel.TurnPenalty", 2.0f);
    ATConf.strongTurnPenalty = sConfigMgr->GetOption<float>("AutoTravel.StrongTurnPenalty", 5.0f);
    ATConf.extremeTurnPenalty = sConfigMgr->GetOption<float>("AutoTravel.ExtremeTurnPenalty", 12.0f);
    ATConf.incompletePathPenalty = sConfigMgr->GetOption<float>("AutoTravel.IncompletePathPenalty", 10000.0f);
    ATConf.swim              = sConfigMgr->GetOption<bool>  ("AutoTravel.Swim", true);
    ATConf.swimSurfaceOffset = sConfigMgr->GetOption<float> ("AutoTravel.SwimSurfaceOffset", 1.2f);
    ATConf.minSwimDepth      = sConfigMgr->GetOption<float> ("AutoTravel.MinSwimDepth", 2.0f);
    ATConf.maxUnderwaterMs   = sConfigMgr->GetOption<uint32>("AutoTravel.MaxUnderwaterMs", 45000);
    ATConf.rescueUnderMesh   = sConfigMgr->GetOption<bool>  ("AutoTravel.RescueUnderMesh", true);
    ATConf.rescueSearchRange = sConfigMgr->GetOption<float> ("AutoTravel.RescueSearchRange", 14.0f);
    ATConf.underMeshDepth    = sConfigMgr->GetOption<float> ("AutoTravel.UnderMeshDepth", 2.5f);
    ATConf.aboveMeshHeight   = sConfigMgr->GetOption<float> ("AutoTravel.AboveMeshHeight", 6.0f);
    ATConf.useTravelNodes    = sConfigMgr->GetOption<bool>  ("AutoTravel.UseTravelNodes", true);
    ATConf.nodeDb            = sConfigMgr->GetOption<std::string>("AutoTravel.NodeDatabase", "acore_playerbots");
    ATConf.nodeSearchRadius  = sConfigMgr->GetOption<float> ("AutoTravel.NodeSearchRadius", 800.0f);
    ATConf.skipDetourFactor  = sConfigMgr->GetOption<float>("AutoTravel.SkipDetourFactor", 1.25f);
    if (ATConf.skipDetourFactor < 1.0f) ATConf.skipDetourFactor = 1.0f;
    ATConf.nodeMinDistance   = sConfigMgr->GetOption<float> ("AutoTravel.NodeMinDistance", 300.0f);
    ATConf.useSpecialLinks   = sConfigMgr->GetOption<bool>  ("AutoTravel.UseSpecialLinks", true);
    ATConf.specialLinkCost   = sConfigMgr->GetOption<float> ("AutoTravel.SpecialLinkCost", 400.0f);
    ATConf.teleportSecurity  = sConfigMgr->GetOption<uint32>("AutoTravel.TeleportSecurity", 2);
    if (ATConf.teleportSecurity > 3) ATConf.teleportSecurity = 3;
    ATConf.allowTeleport     = sConfigMgr->GetOption<bool>  ("AutoTravel.AllowTeleport", true);
    ATConf.teleportMinDist   = sConfigMgr->GetOption<float> ("AutoTravel.TeleportMinDistance", 0.0f);
    ATConf.teleportCooldown  = sConfigMgr->GetOption<uint32>("AutoTravel.TeleportCooldownSec", 5);
    ATConf.debug             = sConfigMgr->GetOption<bool>  ("AutoTravel.Debug", false);
    ATConf.terrainStep       = sConfigMgr->GetOption<float>("AutoTravel.TerrainStep", 3.0f);

    if (ATConf.arrivalDistance < 1.0f) ATConf.arrivalDistance = 1.0f;
    if (ATConf.legDistance     < 1.0f) ATConf.legDistance     = 1.0f;
    if (ATConf.chunkPoints < 2)  ATConf.chunkPoints = 2;
    if (ATConf.chunkPoints > 60) ATConf.chunkPoints = 60;

    LoadMapAreas();
    LoadTravelNodes();
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
// WorldMapArea.dbc -- eigener Loader
// ---------------------------------------------------------------------------
// AzerothCore legt fuer WorldMapArea.dbc keinen DBCStorage an (es gibt kein
// sWorldMapAreaStore), obwohl die Datei vom Extractor erzeugt wird. Fuer die
// Umrechnung Kartenposition -> Weltkoordinaten wird sie aber gebraucht.
//
// Deshalb liest das Modul die Datei beim Start selbst. Das DBC-Format ist
// trivial und seit Vanilla unveraendert:
//
//   char  magic[4] = "WDBC"
//   uint32 recordCount, fieldCount, recordSize, stringBlockSize
//   danach recordCount * recordSize Bytes
//
// WorldMapArea.dbc (3.3.5a), die ersten acht Felder:
//   0 ID   1 mapID   2 areaID   3 areaName(Stringoffset)
//   4 locLeft   5 locRight   6 locTop   7 locBottom      (alle float)
//
// Nur diese acht werden gelesen. Ob dahinter zehn oder elf Felder stehen, ist
// egal -- der Offset der ersten acht ist stabil.

namespace
{
    struct ATMapArea
    {
        uint32 mapId = 0;
        uint32 areaId = 0;
        float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
    };

    std::unordered_map<uint32, ATMapArea> sATMapAreas;
    bool sATMapAreasLoaded = false;

    bool LoadWorldMapAreaFile(std::string const& path, std::string& note)
    {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in.is_open())
            return false;

        char magic[4];
        in.read(magic, 4);
        if (!in || magic[0] != 'W' || magic[1] != 'D' || magic[2] != 'B' || magic[3] != 'C')
        {
            note = path + " ist keine DBC-Datei.";
            return false;
        }

        uint32 recordCount = 0, fieldCount = 0, recordSize = 0, stringSize = 0;
        in.read(reinterpret_cast<char*>(&recordCount), 4);
        in.read(reinterpret_cast<char*>(&fieldCount), 4);
        in.read(reinterpret_cast<char*>(&recordSize), 4);
        in.read(reinterpret_cast<char*>(&stringSize), 4);
        if (!in || fieldCount < 8 || recordSize < fieldCount * 4 || recordCount == 0)
        {
            note = path + ": unerwarteter Aufbau (Felder " + std::to_string(fieldCount) + ").";
            return false;
        }

        std::vector<char> buf(size_t(recordCount) * recordSize);
        in.read(buf.data(), std::streamsize(buf.size()));
        if (!in)
        {
            note = path + ": Datei unvollstaendig.";
            return false;
        }

        for (uint32 i = 0; i < recordCount; ++i)
        {
            char const* rec = buf.data() + size_t(i) * recordSize;
            uint32 u[8];
            float  f[8];
            std::memcpy(u, rec, sizeof(u));
            std::memcpy(f, rec, sizeof(f));

            ATMapArea a;
            a.mapId  = u[1];
            a.areaId = u[2];
            a.left   = f[4];
            a.right  = f[5];
            a.top    = f[6];
            a.bottom = f[7];

            // Kontinent- und Zoneneintraege ohne Ausdehnung sind unbrauchbar.
            if (a.left == a.right || a.top == a.bottom)
                continue;

            sATMapAreas[u[0]] = a;
        }

        note = path + ": " + std::to_string(sATMapAreas.size()) + " Karten geladen.";
        return !sATMapAreas.empty();
    }
}

void AutoTravelMgr::LoadMapAreas()
{
    sATMapAreas.clear();
    sATMapAreasLoaded = false;

    std::string dataDir = sConfigMgr->GetOption<std::string>("DataDir", ".");
    if (!dataDir.empty() && dataDir[dataDir.size() - 1] != '/' && dataDir[dataDir.size() - 1] != '\\')
        dataDir += "/";

    std::string const candidates[] =
    {
        dataDir + "dbc/WorldMapArea.dbc",
        dataDir + "dbc/enUS/WorldMapArea.dbc",
        dataDir + "dbc/enGB/WorldMapArea.dbc",
        dataDir + "dbc/deDE/WorldMapArea.dbc",
        "dbc/WorldMapArea.dbc",
    };

    std::string note;
    for (auto const& p : candidates)
    {
        if (LoadWorldMapAreaFile(p, note))
        {
            sATMapAreasLoaded = true;
            LOG_INFO("server.loading", "mod-autotravel: {}", note);
            return;
        }
    }

    LOG_ERROR("server.loading",
              "mod-autotravel: WorldMapArea.dbc nicht gefunden oder unlesbar (DataDir='{}'). "
              "Zielumrechnung ist deaktiviert. {}", dataDir, note);
}

// ---------------------------------------------------------------------------
// Kartenkoordinaten -> Weltkoordinaten
// ---------------------------------------------------------------------------
// Die vom Client gelieferte Karten-ID (GetCurrentMapAreaID) ist NICHT
// zwangslaeufig die ID aus WorldMapArea.dbc. Statt eine Konvention zu
// unterstellen, wird sie ueberprueft und noetigenfalls korrigiert:
//
// Schickt das Addon zusaetzlich die eigene normalisierte Position mit
// (hasCalib), dann kennt der Server fuer denselben Punkt beide Darstellungen.
// Er probiert alle Kartenausschnitte der aktuellen Map durch und nimmt den,
// der die bekannte echte Position reproduziert. Das ist eindeutig: nur der
// richtige Ausschnitt trifft auf wenige Yards genau.
//
// Die so gefundene Zuordnung Client-ID -> DBC-ID wird gemerkt und spaeter
// auch dann benutzt, wenn keine Gegenprobe moeglich ist (Ziel in einer
// anderen Zone als der Spieler).

namespace
{
    std::unordered_map<uint32, uint32> sATIdFix;      // Client-ID -> DBC-ID
    int32 sATIdDelta = 0;
    bool  sATIdDeltaKnown = false;

    void ApplyArea(ATMapArea const& e, bool swapped, float mx, float my, float& wx, float& wy)
    {
        if (!swapped)
        {
            // waagerechte Kartenachse -> Welt-Y, senkrechte -> Welt-X
            wy = e.left + mx * (e.right - e.left);
            wx = e.top  + my * (e.bottom - e.top);
        }
        else
        {
            wx = e.left + mx * (e.right - e.left);
            wy = e.top  + my * (e.bottom - e.top);
        }
    }
}

void AutoTravelMgr::LearnMapId(Player* player, uint32 clientMapId, float pnx, float pny)
{
    if (!sATMapAreasLoaded || !clientMapId || pnx <= 0.0f || pny <= 0.0f)
        return;
    if (sATIdFix.find(clientMapId) != sATIdFix.end())
        return;                                   // schon bekannt

    float bestErr = 1.0e9f;
    uint32 bestId = 0;

    for (auto const& kv : sATMapAreas)
    {
        if (kv.second.mapId != player->GetMapId())
            continue;
        for (int sw = 0; sw < 2; ++sw)
        {
            float wx, wy;
            ApplyArea(kv.second, sw != 0, pnx, pny, wx, wy);
            float e2 = std::sqrt(std::pow(wx - player->GetPositionX(), 2) +
                                 std::pow(wy - player->GetPositionY(), 2));
            if (e2 < bestErr) { bestErr = e2; bestId = kv.first; }
        }
    }

    if (bestId && bestErr <= 250.0f)
    {
        sATIdFix[clientMapId] = bestId;
        sATIdDelta = int32(bestId) - int32(clientMapId);
        sATIdDeltaKnown = true;
        if (bestId != clientMapId)
            LOG_INFO("module", "mod-autotravel: Karten-ID {} des Clients entspricht "
                               "WorldMapArea {} (Abweichung {:.1f} yd).",
                     clientMapId, bestId, bestErr);
    }
}

bool AutoTravelMgr::MapToWorld(Player* player, uint32 uiMapId, float nx, float ny,
                               bool hasCalib, float pnx, float pny,
                               float& outX, float& outY, std::string& err) const
{
    if (!sATMapAreasLoaded)
    {
        err = "Serverseitig konnte WorldMapArea.dbc nicht geladen werden - siehe Serverlog.";
        return false;
    }

    uint32 chosen = 0;
    bool   swapped = false;

    // --- Fall 1: Gegenprobe moeglich -> richtigen Ausschnitt suchen ---------
    if (hasCalib && pnx > 0.0f && pny > 0.0f)
    {
        float bestErr = 1.0e9f;
        uint32 bestId = 0;
        bool   bestSwapped = false;

        for (auto const& kv : sATMapAreas)
        {
            if (kv.second.mapId != player->GetMapId())
                continue;

            for (int sw = 0; sw < 2; ++sw)
            {
                float wx, wy;
                ApplyArea(kv.second, sw != 0, pnx, pny, wx, wy);
                float e2 = std::sqrt(std::pow(wx - player->GetPositionX(), 2) +
                                     std::pow(wy - player->GetPositionY(), 2));
                if (e2 < bestErr)
                {
                    bestErr = e2;
                    bestId = kv.first;
                    bestSwapped = (sw != 0);
                }
            }
        }

        if (!bestId || bestErr > 250.0f)
        {
            char b[256];
            std::snprintf(b, sizeof(b),
                "Kein passender Kartenausschnitt gefunden (bester Treffer ID %u, %.0f yd daneben). "
                "Ziel wird nicht angefahren. Pruefe die erkannte Zone mit 'at target'.", bestId, bestErr);
            err = b;
            return false;
        }

        if (bestId != uiMapId)
        {
            if (sATIdFix.find(uiMapId) == sATIdFix.end())
                LOG_INFO("module", "mod-autotravel: Karten-ID {} des Clients entspricht "
                                   "WorldMapArea {} (Abweichung {:.1f} yd).",
                         uiMapId, bestId, bestErr);
            sATIdFix[uiMapId] = bestId;
            sATIdDelta = int32(bestId) - int32(uiMapId);
            sATIdDeltaKnown = true;
        }
        else
            sATIdFix[uiMapId] = bestId;

        chosen = bestId;
        swapped = bestSwapped;
    }
    // --- Fall 2: keine Gegenprobe -> gelernte Zuordnung benutzen ------------
    else
    {
        uint32 tryId = uiMapId;

        auto fix = sATIdFix.find(uiMapId);
        if (fix != sATIdFix.end())
            tryId = fix->second;
        else if (sATIdDeltaKnown)
        {
            uint32 shifted = uint32(int32(uiMapId) + sATIdDelta);
            if (sATMapAreas.find(shifted) != sATMapAreas.end())
                tryId = shifted;
        }

        auto it = sATMapAreas.find(tryId);
        if (it == sATMapAreas.end())
        {
            err = "Unbekannte Karten-ID " + std::to_string(uiMapId) + ".";
            return false;
        }
        if (it->second.mapId != player->GetMapId())
        {
            err = "Das Ziel liegt auf einer anderen Karte (Map " +
                  std::to_string(it->second.mapId) +
                  "). Kontinentwechsel wird beim Laufen nicht unterstuetzt.";
            return false;
        }
        chosen = tryId;
    }

    ATMapArea const& e = sATMapAreas.find(chosen)->second;
    ApplyArea(e, swapped, nx, ny, outX, outY);

    // Gegenprobe ueber die Zone: der aufgeloeste Punkt muss in der Zone
    // liegen, die zu diesem Kartenausschnitt gehoert. Damit faellt eine um
    // eins verschobene Karten-ID auch dann auf, wenn der Spieler ganz woanders
    // steht -- genau der Fall bei Zwischenstuetzpunkten der Route.
    if (!hasCalib && e.areaId)
    {
        float probeZ = BestGroundZ(player, outX, outY);
        if (probeZ > INVALID_HEIGHT)
        {
            uint32 zoneAt = player->GetMap()->GetZoneId(player->GetPhaseMask(), outX, outY, probeZ);
            if (zoneAt && zoneAt != e.areaId)
            {
                // Nachbar-IDs probieren
                for (int32 d = -1; d <= 1; d += 2)
                {
                    auto alt = sATMapAreas.find(uint32(int32(chosen) + d));
                    if (alt == sATMapAreas.end() || alt->second.mapId != player->GetMapId())
                        continue;
                    float ax, ay;
                    ApplyArea(alt->second, swapped, nx, ny, ax, ay);
                    float az = BestGroundZ(player, ax, ay);
                    if (az <= INVALID_HEIGHT)
                        continue;
                    if (player->GetMap()->GetZoneId(player->GetPhaseMask(), ax, ay, az) == alt->second.areaId)
                    {
                        sATIdFix[uiMapId] = alt->first;
                        sATIdDelta = int32(alt->first) - int32(uiMapId);
                        sATIdDeltaKnown = true;
                        LOG_INFO("module", "mod-autotravel: Karten-ID {} ueber Zonenpruefung "
                                           "auf WorldMapArea {} korrigiert.", uiMapId, alt->first);
                        outX = ax;
                        outY = ay;
                        return true;
                    }
                }
            }
        }
    }

    return true;
}


// ---------------------------------------------------------------------------
// Beste Oberflaeche an einer Stelle
// ---------------------------------------------------------------------------
// Map::GetHeight(x, y, MAX_HEIGHT) liefert NUR die Gelaendehoehe: die
// VMap-Abfrage sucht ab dem uebergebenen Z nur DEFAULT_HEIGHT_SEARCH (50 yd)
// nach unten, und von 100000 aus findet sie nichts. Unter Sturmwind ist das
// Rohgelaende rund 35 Yards unter dem Stadtboden -- genau deshalb landete der
// Teleport "unter der Stadt".
//
// Deshalb wird mit realistischen Startpunkten und grossem Suchfenster
// abgetastet und die Flaeche genommen, die der eigenen Hoehe am naechsten
// liegt.

float AutoTravelMgr::BestGroundZ(Player* player, float x, float y) const
{
    Map* map = player->GetMap();
    uint32 phase = player->GetPhaseMask();
    float pz = player->GetPositionZ();

    float const starts[6] = { pz + 300.0f, pz + 120.0f, pz + 40.0f,
                              pz + 5.0f, pz - 40.0f, pz - 150.0f };

    float best = INVALID_HEIGHT;
    float bestDiff = 1.0e9f;

    for (uint8 i = 0; i < 6; ++i)
    {
        float h = map->GetHeight(phase, x, y, starts[i], true, 400.0f);
        if (h <= INVALID_HEIGHT)
            continue;
        float d = std::fabs(h - pz);
        if (d < bestDiff)
        {
            bestDiff = d;
            best = h;
        }
    }

    if (best <= INVALID_HEIGHT)
        best = map->GetHeight(phase, x, y, MAX_HEIGHT);   // reines Gelaende

    if (best <= INVALID_HEIGHT)
        return best;

    // Steht dort Wasser, ist die Oberflaeche die richtige Reisehoehe -- sonst
    // landet ein Teleport am Seeboden und die Route fuehrt tauchend hindurch.
    return TravelZ(player, x, y, best);
}

std::vector<float> AutoTravelMgr::FindGroundPlanes(
    Player* player,
    float x,
    float y,
    float probeZ) const
{
    std::vector<float> planes;

    if (!player)
        return planes;

    Map* map = player->GetMap();

    if (!map)
        return planes;

    uint32 phase = player->GetPhaseMask();

    /*
     * Wir suchen nur in unmittelbarer Naehe der erwarteten
     * Laufhoehe.
     *
     * Eine Flaeche 20, 30 oder 50 Yards unter dem Charakter
     * ist fuer die Torbogen-Erkennung nicht interessant.
     */
    constexpr float SEARCH_ABOVE = 12.0f;
    constexpr float SEARCH_BELOW = 6.0f;

    /*
     * Abstand, mit dem wir unterhalb einer gefundenen Flaeche
     * erneut suchen.
     */
    constexpr float PLANE_EPSILON = 0.35f;

    /*
     * Erste Suche.
     */
    float searchZ =
        probeZ + SEARCH_ABOVE;

    float first =
        map->GetHeight(
            phase,
            x,
            y,
            searchZ,
            true,
            SEARCH_ABOVE + SEARCH_BELOW);

    if (first <= INVALID_HEIGHT)
        return planes;

    /*
     * Nur eine Flaeche akzeptieren, die tatsaechlich in unserem
     * erwarteten Hoehenbereich liegt.
     */
    if (first < probeZ - SEARCH_BELOW ||
        first > probeZ + SEARCH_ABOVE)
    {
        return planes;
    }

    planes.push_back(first);

    /*
     * Zweite Ebene suchen.
     *
     * Wichtig:
     * Wir suchen nur noch innerhalb von SEARCH_BELOW unterhalb
     * der ersten Ebene.
     */
    float secondSearchZ =
        first - PLANE_EPSILON;

    float second =
        map->GetHeight(
            phase,
            x,
            y,
            secondSearchZ,
            true,
            SEARCH_BELOW);

    if (second > INVALID_HEIGHT &&
        second < first - PLANE_EPSILON &&
        second >= probeZ - SEARCH_BELOW)
    {
        planes.push_back(second);
    }

    return planes;
}

float AutoTravelMgr::SelectGroundPlane(
    std::vector<float> const& planes,
    float referenceZ,
    float targetZ,
    float horizontalDistance) const
{
    if (planes.empty())
        return INVALID_HEIGHT;

    /*
     * Maximale vertikale Änderung, die wir bei einem einzelnen
     * Terrain-Zwischenpunkt akzeptieren.
     *
     * Bei terrainStep = 3.0 bedeutet 1.5f ungefähr eine sehr
     * deutliche Steigung, aber noch keinen Sprung auf eine
     * darüberliegende Plattform.
     */
    constexpr float MAX_NORMAL_VERTICAL_CHANGE = 1.75f;

    /*
     * Wenn eine neue Ebene nur wenige Zentimeter entfernt ist,
     * behandeln wir sie als dieselbe Oberfläche.
     */
    constexpr float SAME_PLANE_TOLERANCE = 0.75f;

    /*
     * Für echte Rampen darf die Bodenhöhe natürlich steigen.
     * Wir erlauben deshalb einen größeren Anstieg, wenn der
     * Zielpunkt ebenfalls höher liegt.
     */
    constexpr float MAX_TARGET_ASSISTED_CHANGE = 4.0f;

    /*
     * Die aktuelle Bodenhöhe ist die wichtigste Information.
     *
     * Wir suchen zuerst die Ebene, die am nächsten an referenceZ
     * liegt.
     */
    float best = planes.front();
    float bestScore = 1.0e30f;

    for (float candidate : planes)
    {
        float delta =
            candidate -
            referenceZ;

        float absDelta =
            std::fabs(delta);

        float score =
            absDelta;

        /*
         * Ein Sprung nach oben ist deutlich verdächtiger als
         * ein normaler kleiner Anstieg.
         *
         * Das ist genau der Schutz gegen:
         *
         *   Boden Z=0
         *       ->
         *   Torbogen Z=8
         */
        if (delta > MAX_NORMAL_VERTICAL_CHANGE)
        {
            /*
             * Nur dann akzeptieren wir eine große positive
             * Änderung, wenn der NavMesh-Zielpunkt ebenfalls
             * deutlich höher liegt.
             */
            float targetGain =
                targetZ -
                referenceZ;

            if (targetGain < delta - SAME_PLANE_TOLERANCE)
            {
                score +=
                    1000.0f +
                    (delta -
                     MAX_NORMAL_VERTICAL_CHANGE) *
                    100.0f;
            }
            else if (delta > MAX_TARGET_ASSISTED_CHANGE)
            {
                score +=
                    100.0f +
                    (delta -
                     MAX_TARGET_ASSISTED_CHANGE) *
                    20.0f;
            }
        }

        /*
         * Eine extrem große Z-Änderung bei sehr kleiner horizontaler
         * Distanz ist praktisch immer eine andere Ebene und nicht
         * eine normale Laufoberfläche.
         */
        if (horizontalDistance > 0.01f)
        {
            float slope =
                absDelta /
                horizontalDistance;

            if (slope > 0.75f)
            {
                score +=
                    (slope - 0.75f) *
                    500.0f;
            }
        }

        if (score < bestScore)
        {
            bestScore = score;
            best = candidate;
        }
    }

    /*
     * Jetzt prüfen wir noch einmal, ob die beste Ebene überhaupt
     * plausibel erreichbar ist.
     *
     * Ein Sprung von z.B. 0 -> 8 innerhalb von 3 Yards soll
     * ausdrücklich NICHT als normale Bodenebene akzeptiert werden.
     */
    float delta =
        best -
        referenceZ;

    if (delta > MAX_TARGET_ASSISTED_CHANGE)
    {
        float targetGain =
            targetZ -
            referenceZ;

        if (targetGain < delta - SAME_PLANE_TOLERANCE)
            return referenceZ;
    }

    return best;
}
// ---------------------------------------------------------------------------
// Wasser
// ---------------------------------------------------------------------------
// Die Pfadpunkte des NavMesh liegen im Wasser am GRUND. Faehrt der Spline sie
// unveraendert ab, laeuft der Charakter ueber den Seeboden und ertrinkt --
// bei abgegebener Steuerung kann er nicht selbst auftauchen.
//
// Deshalb wird jeder Punkt gegen die Wasseroberflaeche geprueft und, wo noetig,
// knapp darunter gelegt. Damit schwimmt der Charakter an der Oberflaeche
// entlang, die Luft laeuft nicht ab, und die Schwimmanimation stimmt.

bool AutoTravelMgr::WaterSurface(Player* player, float x, float y, float probeZ, float& level) const
{
    if (!ATConf.swim)
        return false;

    Map* map = player->GetMap();
    float ground = INVALID_HEIGHT;
    float lvl = map->GetWaterOrGroundLevel(player->GetPhaseMask(), x, y, probeZ, &ground);

    if (lvl <= INVALID_HEIGHT || ground <= INVALID_HEIGHT)
        return false;
    if (lvl - ground < ATConf.minSwimDepth)
        return false;                       // Pfuetze oder Furt: normal laufen

    level = lvl;
    return true;
}

float AutoTravelMgr::TravelZ(Player* player, float x, float y, float groundZ) const
{
    float lvl = 0.0f;
    if (WaterSurface(player, x, y, groundZ + 2.0f, lvl))
    {
        float swimZ = lvl - ATConf.swimSurfaceOffset;
        if (swimZ > groundZ)
            return swimZ;
    }
    return groundZ;
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

    z = BestGroundZ(player, x, y);
    if (z <= INVALID_HEIGHT)
    {
        err = "Fuer diese Position sind keine Hoehendaten verfuegbar (fehlende vmaps?).";
        return false;
    }
    return true;
}

void AutoTravelMgr::Resolve(Player* player, uint32 uiMapId, float nx, float ny,
                            bool hasCalib, float pnx, float pny)
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32 mapId = 0;
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
// Diagnose
// ---------------------------------------------------------------------------

void AutoTravelMgr::Diagnose(Player* player, uint32 uiMapId, float nx, float ny,
                             bool hasCalib, float pnx, float pny)
{
    char b[256];

    Msg(player, "--- AutoTravel Diagnose ---");

    std::snprintf(b, sizeof(b), "mmaps aktiv: %s | WorldMapArea geladen: %s",
                  sWorld->getBoolConfig(CONFIG_ENABLE_MMAPS) ? "ja" : "NEIN",
                  sATMapAreasLoaded ? "ja" : "NEIN");
    Msg(player, b);

    std::snprintf(b, sizeof(b), "Spieler: Map %u Zone %u | %.1f / %.1f / %.1f",
                  player->GetMapId(), player->GetZoneId(),
                  player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    Msg(player, b);

    auto fix = sATIdFix.find(uiMapId);
    std::string fixTxt = (fix != sATIdFix.end())
        ? std::to_string(fix->second) : std::string("noch nicht gelernt");
    std::snprintf(b, sizeof(b), "Karten-ID %u -> WorldMapArea %s | Gegenprobe: %s",
                  uiMapId, fixTxt.c_str(), hasCalib ? "vorhanden" : "fehlt");
    Msg(player, b);

    float x, y, z;
    uint32 mapId;
    std::string err;
    if (!ResolveWorld(player, uiMapId, nx, ny, hasCalib, pnx, pny, x, y, z, mapId, err))
    {
        Msg(player, "Zielaufloesung fehlgeschlagen: " + err);
        return;
    }

    std::snprintf(b, sizeof(b), "Ziel: %.1f / %.1f / %.1f | Entfernung %.0f yd",
                  x, y, z, player->GetExactDist2d(x, y));
    Msg(player, b);

    Map* map = player->GetMap();
    uint32 phase = player->GetPhaseMask();
    float terrain = map->GetHeight(phase, x, y, MAX_HEIGHT);
    if (terrain > INVALID_HEIGHT)
        std::snprintf(b, sizeof(b), "Gelaendehoehe dort: %.2f (Ziel-Z %.2f)", terrain, z);
    else
        std::snprintf(b, sizeof(b), "Gelaendehoehe dort: keine - an dieser Stelle ist kein Boden.");
    Msg(player, b);

    float zc[6];
    uint8 zn = 0;
    auto addZ = [&](float v)
    {
        if (v <= INVALID_HEIGHT || zn >= 6) return;
        for (uint8 i = 0; i < zn; ++i) if (std::fabs(zc[i] - v) < 1.5f) return;
        zc[zn++] = v;
    };
    float pz = player->GetPositionZ();
    addZ(z);
    addZ(terrain);
    addZ(map->GetHeight(phase, x, y, pz + 5.0f,   true, 400.0f));
    addZ(map->GetHeight(phase, x, y, pz + 40.0f,  true, 400.0f));
    addZ(map->GetHeight(phase, x, y, pz + 120.0f, true, 400.0f));
    addZ(map->GetHeight(phase, x, y, pz + 300.0f, true, 400.0f));

    Movement::PointsArray pts;
    uint32 type = 0;
    bool inc = false;
    bool done = false;
    for (uint8 pass = 0; pass < 2 && !done; ++pass)
    {
        for (uint8 i = 0; i < zn; ++i)
        {
            bool ok = TryPath(player, x, y, zc[i], pass == 1, pts, type, inc);
            std::snprintf(b, sizeof(b), "  %-10s Z %8.2f -> 0x%X (%s), %u Punkte %s",
                          pass == 1 ? "Eckpunkte" : "geglaettet",
                          zc[i], type, PathTypeName(type).c_str(),
                          uint32(pts.size()), ok ? "AKZEPTIERT" : "verworfen");
            Msg(player, b);
            if (ok) { done = true; break; }
        }
    }
    if (zn == 0)
        Msg(player, "  Keine gueltige Hoehe an dieser Stelle - dort ist kein Boden.");
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

    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32 mapId = 0;
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

    // Laufende Reise sauber beenden, damit Spline und Teleport sich nicht
    // gegenseitig ins Gehege kommen.
    auto it = _sessions.find(player->GetGUID());
    if (it != _sessions.end())
    {
        HaltMovement(player, it->second);
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
// Route
// ---------------------------------------------------------------------------
// Carbonite kennt die groben Stuetzpunkte einer Reise: Zonenuebergaenge,
// Torbogen, Bruecken, Flugpunkte, das Ziel. Genau die fehlen dem NavMesh als
// Zwischenziele -- deshalb scheiterte "in einem Rutsch nach Sturmwind".
//
// AutoTravel laeuft die Stuetzpunkte der Reihe nach an und laesst zwischen je
// zwei Punkten den PathGenerator den echten Weg suchen. Carbonite gibt also
// die Abfolge vor, das NavMesh den Weg.
//
// Format eines Stuetzpunkts:  <kartenId>:<nx>:<ny>:<flags>

void AutoTravelMgr::RouteAdd(Player* player, bool clearFirst, std::string const& packed)
{
    std::vector<ATLeg>& r = _pendingRoutes[player->GetGUID()];
    if (clearFirst)
        r.clear();

    std::istringstream iss(packed);
    std::string tok;
    while (iss >> tok)
    {
        ATLeg leg;
        size_t a = tok.find(':');
        size_t b = (a == std::string::npos) ? a : tok.find(':', a + 1);
        size_t c = (b == std::string::npos) ? b : tok.find(':', b + 1);
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos)
            continue;

        leg.uiMapId = uint32(atoi(tok.substr(0, a).c_str()));
        leg.nx      = float(atof(tok.substr(a + 1, b - a - 1).c_str()));
        leg.ny      = float(atof(tok.substr(b + 1, c - b - 1).c_str()));
        leg.flags   = uint8(atoi(tok.substr(c + 1).c_str()));

        if (!leg.uiMapId || leg.nx < 0.0f || leg.nx > 1.0f || leg.ny < 0.0f || leg.ny > 1.0f)
            continue;
        if (r.size() >= 32)
            break;
        r.push_back(leg);
    }
}

bool AutoTravelMgr::RouteStart(Player* player, std::string const& name)
{
    auto it = _pendingRoutes.find(player->GetGUID());
    if (it == _pendingRoutes.end() || it->second.empty())
    {
        Msg(player, "Keine Route empfangen.");
        return false;
    }

    if (!ATConf.enable)                       { Msg(player, "AutoTravel ist deaktiviert."); return false; }
    if (!sWorld->getBoolConfig(CONFIG_ENABLE_MMAPS))
    {
        Msg(player, "Serverseitiges Pathfinding (mmaps) ist deaktiviert.");
        return false;
    }
    if (!player->IsAlive())                   { Msg(player, "Du bist tot."); return false; }
    if (player->IsInFlight() || player->GetVehicle() || player->IsBeingTeleported())
    {
        Msg(player, "Jetzt gerade nicht moeglich (Flug/Fahrzeug/Teleport).");
        return false;
    }

    ATSession& s = _sessions[player->GetGUID()];
    bool wasDebug = s.debug;
    uint32 keepGrace = s.graceOverride;
    float keepArrival = s.arrivalOverride;
    s = ATSession();
    s.debug           = wasDebug;
    s.graceOverride   = keepGrace;
    s.arrivalOverride = keepArrival;
    s.mapId    = player->GetMapId();
    s.destName = name.empty() ? "Ziel" : name;
    s.route    = it->second;
    s.legIdx   = 0;

    char buf[160];
    std::snprintf(buf, sizeof(buf), "Route mit %u Stuetzpunkten uebernommen.", uint32(s.route.size()));
    Dbg(player, s, buf);

    return BeginTravel(player, s);
}

// Aktuellen Stuetzpunkt in Weltkoordinaten aufloesen.
bool AutoTravelMgr::SetLegTarget(Player* player, ATSession& s)
{
    while (s.legIdx < s.route.size())
    {
        ATLeg& leg = s.route[s.legIdx];

        if (!leg.resolved)
        {
            uint32 mapId = 0;
            std::string err;
            if (!ResolveWorld(player, leg.uiMapId, leg.nx, leg.ny, false, 0.0f, 0.0f,
                              leg.wx, leg.wy, leg.wz, mapId, err))
            {
                char b[224];
                std::snprintf(b, sizeof(b), "Stuetzpunkt %u uebersprungen: %s",
                              uint32(s.legIdx + 1), err.c_str());
                Dbg(player, s, b);
                ++s.legIdx;
                continue;
            }
            leg.resolved = true;
        }

        s.destX = leg.wx;
        s.destY = leg.wy;
        s.destZ = leg.wz;
        return true;
    }
    return false;
}

// Wenn der Playerbot-Knotengraph verfuegbar ist, ersetzt er die groben
// Carbonite-Stuetzpunkte: er kennt Flugrouten, Portale und Schiffe und liefert
// echte Weltkoordinaten. Das eigentliche Ziel bleibt der letzte Carbonite-Punkt.
void AutoTravelMgr::ApplyNodeRouting(Player* player, ATSession& s)
{
    if (!ATConf.useTravelNodes || s.route.empty())
        return;

    ATLeg last = s.route.back();
    if (!last.resolved)
    {
        uint32 m = 0;
        std::string err;
        if (!ResolveWorld(player, last.uiMapId, last.nx, last.ny, false, 0.0f, 0.0f,
                          last.wx, last.wy, last.wz, m, err))
            return;
        last.resolved = true;
    }

    float d = player->GetExactDist2d(last.wx, last.wy);
    if (d < ATConf.nodeMinDistance)
        return;                       // kurze Strecke: direkt pathen

    std::vector<ATLeg> nodeLegs;
    std::string note;
    if (!BuildNodeRoute(player, last.wx, last.wy, last.wz, nodeLegs, note))
    {
        Dbg(player, s, "Knotenroute nicht nutzbar (" + note + ") - benutze Carbonite-Stuetzpunkte.");
        return;
    }

    nodeLegs.push_back(last);
    s.route  = nodeLegs;
    s.legIdx = 0;

    uint32 special = 0;
    for (ATLeg const& l : s.route)
        if (l.flags & AT_LEG_SPECIAL)
            ++special;

    char b[224];
    std::snprintf(b, sizeof(b), "Knotenroute: %s%s",
                  note.c_str(),
                  special ? " (enthaelt Sonderverbindungen)" : "");
    Dbg(player, s, b);

    if (special)
    {
        std::snprintf(b, sizeof(b),
                      "Route benutzt %u Sonderverbindung(en) - dort musst du selbst "
                      "fliegen oder das Portal nehmen.", special);
        Msg(player, b);
    }
}

bool AutoTravelMgr::BeginTravel(Player* player, ATSession& s)
{
    ApplyNodeRouting(player, s);

    if (!SetLegTarget(player, s))
    {
        Msg(player, "Kein brauchbarer Stuetzpunkt in der Route.");
        _sessions.erase(player->GetGUID());
        return false;
    }

    s.state = AT_CALCULATE_PATH;
    s.lastX = player->GetPositionX();
    s.lastY = player->GetPositionY();
    s.lastZ = player->GetPositionZ();

    Msg(player, "Reise gestartet: " + s.destName);
    PushStatus(player, s);
    return true;
}

// Naechsten Stuetzpunkt aktivieren. false = Ziel erreicht.
bool AutoTravelMgr::AdvanceLeg(Player* player, ATSession& s)
{
    ++s.legIdx;
    s.repathAttempts = 0;
    s.mountTried = false;
    s.path.clear();
    s.idx = 0;

    if (s.legIdx >= s.route.size())
        return false;

    if (!SetLegTarget(player, s))
        return false;

    char b[160];
    std::snprintf(b, sizeof(b), "Stuetzpunkt %u/%u erreicht, weiter zum naechsten.",
                  uint32(s.legIdx), uint32(s.route.size()));
    Dbg(player, s, b);

    s.state = AT_CALCULATE_PATH;
    return true;
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
    // Einzelziel = Route mit genau einem Stuetzpunkt. Dadurch gibt es nur
    // einen Ablauf im Tick, egal ob Carbonite eine Route geliefert hat.
    if (!ATConf.enable)                       { Msg(player, "AutoTravel ist deaktiviert."); return false; }
    if (!sWorld->getBoolConfig(CONFIG_ENABLE_MMAPS))
    {
        Msg(player, "Serverseitiges Pathfinding (mmaps) ist deaktiviert.");
        return false;
    }
    if (!player->IsAlive())                   { Msg(player, "Du bist tot."); return false; }
    if (player->IsInFlight() || player->GetVehicle() || player->IsBeingTeleported())
    {
        Msg(player, "Jetzt gerade nicht moeglich (Flug/Fahrzeug/Teleport).");
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
    uint32 keepGrace = s.graceOverride;
    float keepArrival = s.arrivalOverride;
    s = ATSession();
    s.debug           = wasDebug;
    s.graceOverride   = keepGrace;
    s.arrivalOverride = keepArrival;
    s.mapId    = player->GetMapId();
    s.destName = name.empty() ? "Ziel" : name;

    ATLeg leg;
    leg.uiMapId = uiMapId;
    leg.nx = nx;
    leg.ny = ny;
    leg.wx = wx;
    leg.wy = wy;
    leg.wz = wz;
    leg.resolved = true;
    s.route.push_back(leg);
    s.legIdx = 0;

    char buf[224];
    std::snprintf(buf, sizeof(buf), "Ziel: Map %u | X %.2f Y %.2f Z %.2f", s.mapId, wx, wy, wz);
    Dbg(player, s, buf);

    return BeginTravel(player, s);
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
        if (v < 0.5f || v > 100.0f)
        {
            Msg(player, "Zielradius muss zwischen 0.5 und 100 Yards liegen.");
            return;
        }
        s.arrivalOverride = v;
        Msg(player, "Arrival Distance = " + value);
    }
    else if (key == "grace")
    {
        float v = float(atof(value.c_str()));
        if (v < 0.0f || v > 30.0f)
        {
            Msg(player, "Grace muss zwischen 0 und 30 Sekunden liegen.");
            return;
        }
        s.graceOverride = uint32(v * 1000.0f);
        Dbg(player, s, "Wartezeit nach Kampf: " + value + " s");
    }
    else
        Msg(player, "Unbekannte Option: " + key);

    if (s.state == AT_IDLE && s.arrivalOverride <= 0.0f && s.graceOverride == 0 && !s.debug)
        _sessions.erase(player->GetGUID());
}

// ---------------------------------------------------------------------------
// Bewegung
// ---------------------------------------------------------------------------

void AutoTravelMgr::ReleaseControl(Player* player, ATSession& s)
{
    if (s.controlTaken)
    {
        // Fallbezug und Bewegungsflags zuruecksetzen, bevor der Client wieder
        // selbst rechnet -- sonst bleibt die Fall- oder Schwimmanimation haengen.
        player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
        if (s.swimming && !player->IsInWater())
            player->RemoveUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
        s.swimming = false;
        player->SetFallInformation(0, player->GetPositionZ());
        player->SetClientControl(player, true);
        s.controlTaken = false;
    }
}

void AutoTravelMgr::HaltMovement(Player* player, ATSession& s)
{
    // Sicherung: waehrend eines Taxifluges wuerde StopMoving() den Spline des
    // FlightPathMovementGenerator loeschen. Der Spieler sitzt dann auf dem
    // Greifen, fliegt aber nicht. Deshalb hier grundsaetzlich nichts tun --
    // unabhaengig davon, welche Stelle im Modul den Aufruf ausloest.
    if (player->IsInFlight())
    {
        ReleaseControl(player, s);
        return;
    }

    player->StopMoving();
    ReleaseControl(player, s);
}

void AutoTravelMgr::LaunchChunk(Player* player, ATSession& s)
{
    if (s.idx >= s.path.size())
        return;

    /*
     * MoveSpline simuliert zwischen seinen Punkten keine normale
     * Gravitation. Werden zwei weit auseinanderliegende NavMesh-Punkte
     * direkt verbunden, kann der Charakter deshalb ueber Abhaenge
     * "fliegen".
     *
     * Wir verdichten den Pfad horizontal und projizieren jeden erzeugten
     * Punkt auf die tatsaechliche Oberflaeche.
     *
     * WICHTIG:
     *
     * terrainStep kommt aus ATConf und wird ueber
     * AutoTravel.TerrainStep konfiguriert.
     *
     * Der aktuelle Git-Stand hatte hier noch einen hartcodierten Wert
     * von 3.0f. Das wird hier bewusst entfernt.
     */
    float terrainStep =
        std::max(0.5f, ATConf.terrainStep);

    constexpr float TERRAIN_OFFSET = 0.10f;

    /*
     * Kein kuenstliches Limit pro Chunk.
     *
     * ChunkPoints begrenzt weiterhin die Anzahl der PathGenerator-
     * Quellpunkte. Die daraus entstehenden Terrainpunkte duerfen mehr
     * sein, weil terrainStep genau dafuer gedacht ist.
     */
    Movement::PointsArray chunk;

    G3D::Vector3 current(
        player->GetPositionX(),
        player->GetPositionY(),
        player->GetPositionZ());

    chunk.push_back(current);

    bool water = false;
    uint32 sourcePoints = 0;

    Map* map = player->GetMap();
    uint32 phase = player->GetPhaseMask();

    while (s.idx < s.path.size() &&
           sourcePoints < ATConf.chunkPoints)
    {
        G3D::Vector3 target =
            s.path[s.idx];

        float dx =
            target.x - current.x;

        float dy =
            target.y - current.y;

        float horizontal =
            std::sqrt(
                dx * dx +
                dy * dy);

        /*
         * Anzahl der Terrainabschnitte.
         *
         * Beispiel:
         *
         * terrainStep = 3
         * Segment = 12 yd
         *
         * => 4 Zwischenpunkte
         */
        uint32 steps =
            std::max<uint32>(
                1,
                uint32(
                    std::ceil(
                        horizontal /
                        terrainStep)));

        /*
         * Jeder einzelne Abschnitt wird vollstaendig erzeugt.
         *
         * Wir kuerzen steps NICHT mehr anhand einer globalen
         * Punktbegrenzung. Der alte Code konnte dadurch ein langes
         * Segment mitten drin abbrechen und danach trotzdem s.idx
         * erhoehen. Das konnte wieder einen Flug/Sprung erzeugen.
         */
        for (uint32 step = 1;
             step <= steps;
             ++step)
        {
            float t =
                float(step) /
                float(steps);

            float x =
                current.x +
                (target.x - current.x) *
                t;

            float y =
                current.y +
                (target.y - current.y) *
                t;

            /*
             * Fuer die Hoehenabfrage nicht nur current.z verwenden.
             *
             * Bei einem starken Abstieg kann die Oberflaeche deutlich
             * unter current.z liegen. Bei einem starken Aufstieg kann
             * sie entsprechend deutlich darueber liegen.
             *
             * Deshalb wird die hoehere der beiden bekannten Hoehen als
             * Startpunkt verwendet und ein grosszuegiges Suchfenster
             * gegeben.
             */

            /*
             * Nicht mehr blind die erste GetHeight()-Fläche verwenden.
             *
             * An Torbögen können mehrere VMap-Flächen exakt unter
             * demselben X/Y liegen:
             *
             *     Z=8  <- Torbogen
             *     Z=0  <- Boden
             *
             * GetHeight() liefert normalerweise die zuerst gefundene
             * Fläche. Deshalb suchen wir mehrere Ebenen und wählen
             * anschließend die Ebene, die zur bisherigen Bodenhöhe
             * passt.
             */

            std::vector<float> planes =
                FindGroundPlanes(
                    player,
                    x,
                    y,
                    current.z);

            float ground =
                SelectGroundPlane(
                    planes,
                    current.z,
                    target.z,
                    horizontal);

            /*
             * Falls keine brauchbare Ebene gefunden wurde, benutzen
             * wir weiterhin den bisherigen Fallback.
             */
            if (ground <= INVALID_HEIGHT)
            {
                ground =
                    BestGroundZ(
                        player,
                        x,
                        y);
            }

            float z;

            if (ground > INVALID_HEIGHT)
            {
                /*
                 * Der Charakter wird minimal ueber der Oberflaeche
                 * gehalten, damit er nicht in die VMap/Map-Fläche
                 * gedrueckt wird.
                 */
                z =
                    ground +
                    TERRAIN_OFFSET;
            }
            else
            {
                /*
                 * Nur wenn gar keine Bodenhoehe verfuegbar ist,
                 * verwenden wir die NavMesh-Z-Interpolation als
                 * Fallback.
                 *
                 * Dieser Fall sollte selten sein.
                 */
                z =
                    current.z +
                    (target.z - current.z) *
                    t;
            }

            /*
             * Wasser weiterhin ueber TravelZ behandeln.
             *
             * Die Bodenprojektion bleibt fuer normales Terrain aktiv.
             * Im Wasser darf TravelZ die Z-Hoehe nach oben verschieben.
             */
            float base =
                (ground > INVALID_HEIGHT)
                    ? ground
                    : z;

            float travel =
                TravelZ(
                    player,
                    x,
                    y,
                    base);

            if (travel > z)
            {
                z = travel;
                water = true;
            }

            chunk.push_back(
                G3D::Vector3(
                    x,
                    y,
                    z));
        }

        /*
         * Der komplette Abschnitt bis zum NavMesh-Zielpunkt wurde jetzt
         * erzeugt.
         *
         * WICHTIG:
         *
         * Nicht einfach current = target setzen.
         * current muss die tatsaechlich erzeugte Terrainposition sein,
         * damit das naechste Segment von der realen Z-Hoehe ausgeht.
         */
        current =
            chunk.back();

        /*
         * Jetzt darf der PathGenerator-Punkt als verarbeitet gelten.
         */
        ++s.idx;
        ++sourcePoints;
    }

    if (chunk.size() < 2)
        return;

    /*
     * Serverkontrolle nur einmal pro Reise uebernehmen.
     */
    if (ATConf.takeClientControl &&
        !s.controlTaken)
    {
        player->SetClientControl(
            player,
            false);

        s.controlTaken = true;
    }

    /*
     * Alte Falling-Flags nicht in den neuen Spline uebernehmen.
     */
    player->RemoveUnitMovementFlag(
        MOVEMENTFLAG_FALLING |
        MOVEMENTFLAG_FALLING_FAR);

    player->SetFallInformation(
        0,
        player->GetPositionZ());

    /*
     * Schwimmen nur aktivieren, wenn mindestens ein Punkt dieses
     * Chunks eine Schwimmhoehe benoetigt.
     */
    if (water)
    {
        player->AddUnitMovementFlag(
            MOVEMENTFLAG_SWIMMING);
    }
    else
    {
        player->RemoveUnitMovementFlag(
            MOVEMENTFLAG_SWIMMING);
    }

    s.swimming = water;

    /*
     * MoveSpline bekommt jetzt einen bereits terrain-following Pfad.
     *
     * Die grossen Z-Spruenge sind durch die vielen Zwischenpunkte
     * beseitigt.
     */
    Movement::MoveSplineInit init(player);

    init.MovebyPath(chunk);
    init.SetWalk(false);
    init.Launch();

    /*
     * Fallbezug nach dem Launch aktualisieren.
     */
    player->SetFallInformation(
        0,
        player->GetPositionZ());

    char buf[192];

    std::snprintf(
        buf,
        sizeof(buf),
        "Movement gestartet: %u Terrainpunkte, "
        "TerrainStep %.2f, Index %u/%u",
        uint32(chunk.size()),
        terrainStep,
        uint32(s.idx),
        uint32(s.path.size()));

    Dbg(
        player,
        s,
        buf);
}

// ---------------------------------------------------------------------------
// Pathfinding (echtes AzerothCore NavMesh via PathGenerator)
// ---------------------------------------------------------------------------


// Ein einzelner Pathfinding-Versuch auf einen konkreten Punkt.
bool AutoTravelMgr::TryPath(
    Player* player,
    float x,
    float y,
    float z,
    bool straight,
    Movement::PointsArray& out,
    uint32& typeOut,
    bool& incomplete) const
{
    return TryPathBetween(
        player,
        player->GetPositionX(),
        player->GetPositionY(),
        player->GetPositionZ(),
        x,
        y,
        z,
        straight,
        out,
        typeOut,
        incomplete);
}

bool AutoTravelMgr::TryPathBetween(
    Player* player,
    float startX,
    float startY,
    float startZ,
    float destX,
    float destY,
    float destZ,
    bool straight,
    Movement::PointsArray& out,
    uint32& typeOut,
    bool& incomplete) const
{
    PathGenerator gen(player);
    gen.SetUseStraightPath(straight);

    bool built = gen.CalculatePath(
        startX,
        startY,
        startZ,
        destX,
        destY,
        destZ,
        false);

    typeOut = uint32(gen.GetPathType());
    out = gen.GetPath();
        if (ATConf.debug)
    {
        char b[256];

        std::snprintf(
            b,
            sizeof(b),
            "PathGenerator: Start %.1f/%.1f/%.1f -> "
            "Ziel %.1f/%.1f/%.1f | built=%u type=0x%X "
            "(%s) Punkte=%u",
            startX,
            startY,
            startZ,
            destX,
            destY,
            destZ,
            built ? 1u : 0u,
            typeOut,
            PathTypeName(typeOut).c_str(),
            uint32(out.size()));

        Dbg(
            player,
            _sessions.at(player->GetGUID()),
            b);
    }

    if (!built || out.size() < 2)
        return false;

    if (typeOut & PATHFIND_NOPATH)
        return false;

    if (typeOut & PATHFIND_NOT_USING_PATH)
        return false;

    if ((typeOut & PATHFIND_SHORTCUT) &&
        !(typeOut & PATHFIND_NORMAL))
    {
        float dx = destX - startX;
        float dy = destY - startY;

        float distance =
            std::sqrt(dx * dx + dy * dy);

        if (distance > 40.0f)
            return false;
    }

    G3D::Vector3 const& last = out.back();

        /*
     * Nicht nur den letzten Punkt pruefen.
     *
     * Ein PathGenerator-Pfad kann zwischen Start und Ende grosse
     * Hoehenspruenge enthalten. Der alte Code akzeptierte ihn trotzdem,
     * solange nur der letzte Punkt auf dem Boden lag.
     *
     * Wir pruefen deshalb alle NavMesh-Punkte.
     */
    Map* map = player->GetMap();
    uint32 phase = player->GetPhaseMask();

    for (size_t i = 0; i < out.size(); ++i)
    {
        G3D::Vector3& p = out[i];

        /*
         * WICHTIG:
         *
         * TryPathBetween() prueft den vom PathGenerator gelieferten
         * NavMesh-Pfad.
         *
         * Hier KEINE Multi-Plane-Erkennung verwenden.
         *
         * Ein Torbogen kann mehrere VMap-Flaechen unter derselben
         * X/Y-Position besitzen. Diese Information ist fuer die
         * Terrain-Spline-Erzeugung in LaunchChunk() interessant,
         * darf aber nicht den NavMesh-Pfad selbst umdeuten.
         */

        float ground =
            map->GetHeight(
                phase,
                p.x,
                p.y,
                p.z + 5.0f,
                true,
                200.0f);

        if (ground <= INVALID_HEIGHT)
        {
            ground =
                BestGroundZ(
                    player,
                    p.x,
                    p.y);
        }

        if (ground <= INVALID_HEIGHT)
            return false;

        if (p.z < ground - 5.0f)
            return false;

        if (p.z > ground + 8.0f)
            return false;
    }

    incomplete =
        (typeOut & PATHFIND_INCOMPLETE) != 0;

    return true;
}

float AutoTravelMgr::PathDistance(
    Movement::PointsArray const& path) const
{
    if (path.size() < 2)
        return 0.0f;

    float distance = 0.0f;

    for (size_t i = 1; i < path.size(); ++i)
    {
        float dx =
            path[i].x - path[i - 1].x;

        float dy =
            path[i].y - path[i - 1].y;

        distance +=
            std::sqrt(dx * dx + dy * dy);
    }

    return distance;
}

float AutoTravelMgr::ScoreNaturalPath(
    Player* /*player*/,
    Movement::PointsArray const& path,
    bool incomplete) const
{
    if (path.size() < 2)
        return 1.0e30f;

    float score = PathDistance(path);

    if (incomplete)
        score += ATConf.incompletePathPenalty;

    /*
     * Individual slope penalty.
     */
    for (size_t i = 1; i < path.size(); ++i)
    {
        float dx =
            path[i].x - path[i - 1].x;

        float dy =
            path[i].y - path[i - 1].y;

        float dz =
            std::fabs(
                path[i].z - path[i - 1].z);

        float horizontal =
            std::sqrt(dx * dx + dy * dy);

        if (horizontal < 0.01f)
            continue;

        float slope =
            dz / horizontal;

        if (slope > ATConf.slopeStart)
        {
            float excess =
                slope - ATConf.slopeStart;

            score +=
                horizontal *
                excess *
                ATConf.slopePenalty;
        }

        if (slope > ATConf.slopeStrong)
        {
            float excess =
                slope - ATConf.slopeStrong;

            score +=
                horizontal *
                excess *
                ATConf.steepSlopePenalty;
        }

        if (slope > ATConf.slopeExtreme)
        {
            float excess =
                slope - ATConf.slopeExtreme;

            score +=
                horizontal *
                excess *
                ATConf.extremeSlopePenalty;
        }
    }

    /*
     * Sustained uphill movement.
     *
     * Detects a mountain that consists of many individually harmless
     * NavMesh segments.
     *
     * ACHTUNG - hier lagen zwei Fehler in der Vorfassung:
     *
     * 1. Das Fenster startete an JEDEM Punkt. Damit haing die Strafe an der
     *    Punktdichte statt an der Geometrie: derselbe Hang kostete 0 bei
     *    2-Yard-Abtastung und 1000 bei 20-Yard-Abtastung. Geglaettete Pfade
     *    (etwa 4 Yard Schrittweite) bekamen also gar keine Bergstrafe --
     *    ausgerechnet die, die normalerweise benutzt werden.
     *
     * 2. Das Fenster war mit 10 Yards zu kurz. Ueber 10 Yards steigt auch ein
     *    langer Berg nur ein bis zwei Yards und blieb unter der Schwelle.
     *
     * Jetzt: Fenster fester Laenge (Standard 40 yd), gesetzt in festen
     * Abstaenden entlang der STRECKE, nicht je Punkt. Die Strafe ist in Yards
     * gerechnet und damit mit der Wegstrecke vergleichbar.
     */
    float const window = std::max(10.0f, ATConf.elevationWindow);
    float const stride = std::max(5.0f, window * 0.5f);

    {
        // Kumulierte Streckenlaenge je Punkt, einmal vorab.
        std::vector<float> along(path.size(), 0.0f);
        for (size_t i = 1; i < path.size(); ++i)
        {
            float dx = path[i].x - path[i - 1].x;
            float dy = path[i].y - path[i - 1].y;
            along[i] = along[i - 1] + std::sqrt(dx * dx + dy * dy);
        }

        float const total = along.back();
        size_t startIdx = 0;

        for (float at = 0.0f; at + window <= total + 0.01f; at += stride)
        {
            while (startIdx + 1 < path.size() && along[startIdx + 1] <= at)
                ++startIdx;

            size_t endIdx = startIdx;
            while (endIdx + 1 < path.size() && along[endIdx + 1] <= at + window)
                ++endIdx;

            if (endIdx <= startIdx)
                continue;

            // Nur Anstieg zaehlt. Bergab ist kein Kletterproblem.
            float gain = path[endIdx].z - path[startIdx].z;
            if (gain <= ATConf.elevationGainStart)
                continue;

            score += (gain - ATConf.elevationGainStart) * ATConf.elevationPenalty;

            if (gain > ATConf.elevationGainStrong)
                score += (gain - ATConf.elevationGainStrong) * ATConf.strongElevationPenalty;

            if (gain > ATConf.elevationGainExtreme)
                score += (gain - ATConf.elevationGainExtreme) * ATConf.extremeElevationPenalty;
        }
    }

    /*
     * Sharp-turn penalty.
     */
    if (path.size() >= 3)
    {
        for (size_t i = 1;
             i + 1 < path.size();
             ++i)
        {
            float ax =
                path[i].x -
                path[i - 1].x;

            float ay =
                path[i].y -
                path[i - 1].y;

            float bx =
                path[i + 1].x -
                path[i].x;

            float by =
                path[i + 1].y -
                path[i].y;

            float lenA =
                std::sqrt(ax * ax + ay * ay);

            float lenB =
                std::sqrt(bx * bx + by * by);

            if (lenA < 0.01f ||
                lenB < 0.01f)
            {
                continue;
            }

            float dot =
                (ax * bx + ay * by) /
                (lenA * lenB);

            dot =
                std::max(
                    -1.0f,
                    std::min(1.0f, dot));

            float angle =
                std::acos(dot) *
                180.0f /
                3.14159265358979323846f;

            if (angle > ATConf.turnPenaltyStart)
            {
                score +=
                    (angle -
                     ATConf.turnPenaltyStart) *
                    ATConf.turnPenalty;
            }

            if (angle > ATConf.turnPenaltyStrong)
            {
                score +=
                    (angle -
                     ATConf.turnPenaltyStrong) *
                    ATConf.strongTurnPenalty;
            }

            if (angle > ATConf.turnPenaltyExtreme)
            {
                score +=
                    (angle -
                     ATConf.turnPenaltyExtreme) *
                    ATConf.extremeTurnPenalty;
            }
        }
    }

    return score;
}

bool AutoTravelMgr::HasMountainClimb(
    Movement::PointsArray const& path) const
{
    if (path.size() < 2)
        return false;

    float window =
        std::max(5.0f, ATConf.elevationWindow);

    for (size_t start = 0;
         start < path.size();
         ++start)
    {
        float travelled = 0.0f;
        size_t end = start + 1;

        float startZ = path[start].z;
        float endZ = startZ;

        while (end < path.size() &&
               travelled < window)
        {
            float dx =
                path[end].x -
                path[end - 1].x;

            float dy =
                path[end].y -
                path[end - 1].y;

            travelled +=
                std::sqrt(dx * dx + dy * dy);

            endZ = path[end].z;

            ++end;
        }

        if (travelled < 10.0f)
            continue;

        float gain = endZ - startZ;

        if (gain < ATConf.contourTriggerElevation)
            continue;

        float slope = gain / travelled;

        if (slope >= ATConf.contourTriggerSlope)
            return true;
    }

    return false;
}

bool AutoTravelMgr::BuildContourCandidate(
    Player* player,
    ATSession const& s,
    float offset,
    bool left,
    Movement::PointsArray& out,
    uint32& typeOut,
    bool& incomplete,
    float& score) const
{
    float startX = player->GetPositionX();
    float startY = player->GetPositionY();
    float startZ = player->GetPositionZ();

    float dx = s.destX - startX;
    float dy = s.destY - startY;

    float distance =
        std::sqrt(dx * dx + dy * dy);

    if (distance < 20.0f)
        return false;

    /*
     * Direction from A -> B.
     */
    float dirX = dx / distance;
    float dirY = dy / distance;

    /*
     * Perpendicular vector.
     *
     * Left:
     *     (-dirY, dirX)
     *
     * Right:
     *     ( dirY,-dirX)
     */
    float sideX;
    float sideY;

    if (left)
    {
        sideX = -dirY;
        sideY =  dirX;
    }
    else
    {
        sideX =  dirY;
        sideY = -dirX;
    }

    float p1Progress =
        std::max(
            0.20f,
            std::min(
                0.80f,
                ATConf.contourFirstProgress));

    float p2Progress =
        std::max(
            p1Progress + 0.10f,
            std::min(
                0.90f,
                ATConf.contourSecondProgress));

    /*
     * Two points on the same side of the route.
     *
     * This is significantly better than testing one side point:
     *
     *             mountain
     *               /\
     *              /  \
     * A ----------P1  P2---------- B
     *
     * The path can now follow the side of the terrain instead
     * of simply touching it once.
     */
    float p1X =
        startX +
        dx * p1Progress +
        sideX * offset;

    float p1Y =
        startY +
        dy * p1Progress +
        sideY * offset;

    float p2X =
        startX +
        dx * p2Progress +
        sideX * offset;

    float p2Y =
        startY +
        dy * p2Progress +
        sideY * offset;

    Map* map = player->GetMap();
    uint32 phase = player->GetPhaseMask();

    float p1Z =
        BestGroundZ(player, p1X, p1Y);

    float p2Z =
        BestGroundZ(player, p2X, p2Y);

    if (p1Z <= INVALID_HEIGHT ||
        p2Z <= INVALID_HEIGHT)
    {
        return false;
    }

    /*
     * Test both smooth and corner paths.
     */
    float bestScore = 1.0e30f;

    Movement::PointsArray best;
    uint32 bestType = PATHFIND_NOPATH;
    bool bestIncomplete = false;

    for (uint8 pass = 0; pass < 2; ++pass)
    {
        bool straight = (pass == 1);

        Movement::PointsArray a;
        Movement::PointsArray b;
        Movement::PointsArray c;

        uint32 typeA = PATHFIND_NOPATH;
        uint32 typeB = PATHFIND_NOPATH;
        uint32 typeC = PATHFIND_NOPATH;

        bool incompleteA = false;
        bool incompleteB = false;
        bool incompleteC = false;

        /*
         * A -> P1
         */
        if (!TryPathBetween(
                player,
                startX,
                startY,
                startZ,
                p1X,
                p1Y,
                p1Z,
                straight,
                a,
                typeA,
                incompleteA))
        {
            continue;
        }

        /*
         * P1 -> P2
         */
        if (!TryPathBetween(
                player,
                p1X,
                p1Y,
                p1Z,
                p2X,
                p2Y,
                p2Z,
                straight,
                b,
                typeB,
                incompleteB))
        {
            continue;
        }

        /*
         * P2 -> B
         */
        if (!TryPathBetween(
                player,
                p2X,
                p2Y,
                p2Z,
                s.destX,
                s.destY,
                s.destZ,
                straight,
                c,
                typeC,
                incompleteC))
        {
            continue;
        }

        Movement::PointsArray combined;

        /*
         * First segment includes its starting point.
         */
        for (auto const& p : a)
            combined.push_back(p);

        /*
         * Don't duplicate P1.
         */
        for (size_t i = 1; i < b.size(); ++i)
            combined.push_back(b[i]);

        /*
         * Don't duplicate P2.
         */
        for (size_t i = 1; i < c.size(); ++i)
            combined.push_back(c[i]);

        if (combined.size() < 2)
            continue;

        bool combinedIncomplete =
            incompleteA ||
            incompleteB ||
            incompleteC;

        float candidateScore =
            ATConf.naturalPathing
                ? ScoreNaturalPath(
                      player,
                      combined,
                      combinedIncomplete)
                : PathDistance(combined);

        /*
         * A contour route should not become a ridiculous detour.
         */
        float directDistance =
            std::sqrt(dx * dx + dy * dy);

        float candidateDistance =
            PathDistance(combined);

        if (candidateDistance >
            directDistance *
            ATConf.contourMaxDistanceFactor)
        {
            continue;
        }

        /*
         * Slight preference for smooth path representation.
         */
        if (!straight)
            candidateScore -= 0.01f;

        /*
         * Don't allow incomplete contour paths to win against
         * complete direct paths.
         */
        if (combinedIncomplete)
            candidateScore +=
                ATConf.incompletePathPenalty;

        if (candidateScore < bestScore)
        {
            bestScore = candidateScore;
            best = combined;

            bestType =
                typeA |
                typeB |
                typeC;

            bestIncomplete =
                combinedIncomplete;
        }
    }

    if (best.empty())
        return false;

    out = best;
    typeOut = bestType;
    incomplete = bestIncomplete;
    score = bestScore;

    return true;
}


bool AutoTravelMgr::CalculatePath(
    Player* player,
    ATSession& s)
{
    Map* map = player->GetMap();
    uint32 phase = player->GetPhaseMask();
    float pz = player->GetPositionZ();

    /*
     * -------------------------------------------------------------
     * Height candidates at the actual destination
     * -------------------------------------------------------------
     */
    float zc[10];
    uint8 zn = 0;

    auto addZ = [&](float z)
    {
        if (z <= INVALID_HEIGHT ||
            zn >= 10)
        {
            return;
        }

        for (uint8 i = 0; i < zn; ++i)
        {
            if (std::fabs(zc[i] - z) < 1.5f)
                return;
        }

        zc[zn++] = z;
    };

    addZ(s.destZ);

    addZ(
        BestGroundZ(
            player,
            s.destX,
            s.destY));

    addZ(
        map->GetHeight(
            phase,
            s.destX,
            s.destY,
            MAX_HEIGHT));

    addZ(
        map->GetHeight(
            phase,
            s.destX,
            s.destY,
            pz + 5.0f,
            true,
            400.0f));

    addZ(
        map->GetHeight(
            phase,
            s.destX,
            s.destY,
            pz + 40.0f,
            true,
            400.0f));

    addZ(
        map->GetHeight(
            phase,
            s.destX,
            s.destY,
            pz + 120.0f,
            true,
            400.0f));

    addZ(
        map->GetHeight(
            phase,
            s.destX,
            s.destY,
            pz + 300.0f,
            true,
            400.0f));

    /*
     * -------------------------------------------------------------
     * Evaluate all direct candidates.
     *
     * IMPORTANT:
     *
     * The old code returned immediately after the first valid
     * TryPath(). We deliberately do NOT do that anymore.
     * -------------------------------------------------------------
     */
    Movement::PointsArray bestPath;

    uint32 bestType = PATHFIND_NOPATH;
    bool bestIncomplete = false;

    float bestScore = 1.0e30f;

    for (uint8 pass = 0; pass < 2; ++pass)
    {
        bool straight = (pass == 1);

        for (uint8 i = 0; i < zn; ++i)
        {
            Movement::PointsArray candidate;

            uint32 type =
                PATHFIND_NOPATH;

            bool incomplete = false;

            if (!TryPath(
                    player,
                    s.destX,
                    s.destY,
                    zc[i],
                    straight,
                    candidate,
                    type,
                    incomplete))
            {
                continue;
            }

            float score =
                ATConf.naturalPathing
                    ? ScoreNaturalPath(
                          player,
                          candidate,
                          incomplete)
                    : PathDistance(candidate);

            if (!straight)
                score -= 0.01f;

            if (score < bestScore)
            {
                bestScore = score;
                bestPath = candidate;
                bestType = type;
                bestIncomplete = incomplete;
                s.destZ = zc[i];
            }

            char b[256];

            std::snprintf(
                b,
                sizeof(b),
                "Direktkandidat %s: Z=%.2f "
                "Distanz=%.1f Score=%.1f "
                "Incomplete=%u Punkte=%u",
                straight
                    ? "Eckpunkte"
                    : "Geglaettet",
                zc[i],
                PathDistance(candidate),
                score,
                incomplete ? 1u : 0u,
                uint32(candidate.size()));

            Dbg(player, s, b);
        }
    }

    /*
     * -------------------------------------------------------------
     * CONTOUR PROBING
     * -------------------------------------------------------------
     *
     * Only activate this expensive search when the chosen route
     * actually looks like a mountain climb.
     */
    if (ATConf.contourProbing &&
        !bestPath.empty() &&
        HasMountainClimb(bestPath))
    {
        Dbg(
            player,
            s,
            "Berganstieg erkannt - "
            "Contour-Probing wird gestartet.");

        /*
         * Four candidates:
         *
         *   LEFT  + narrow
         *   LEFT  + wide
         *   RIGHT + narrow
         *   RIGHT + wide
         *
         * Each candidate contains TWO intermediate points.
         *
         * Therefore:
         *
         *   4 routes × 2 points = 8 probe points.
         */
        float offsets[2] =
        {
            ATConf.contourNarrowOffset,
            ATConf.contourWideOffset
        };

        for (uint8 side = 0;
             side < 2;
             ++side)
        {
            bool left = (side == 0);

            for (uint8 oi = 0;
                 oi < 2;
                 ++oi)
            {
                float contourScore = 1.0e30f;

                Movement::PointsArray contourPath;

                uint32 contourType =
                    PATHFIND_NOPATH;

                bool contourIncomplete = false;

                if (!BuildContourCandidate(
                        player,
                        s,
                        offsets[oi],
                        left,
                        contourPath,
                        contourType,
                        contourIncomplete,
                        contourScore))
                {
                    continue;
                }

                char b[256];

                std::snprintf(
                    b,
                    sizeof(b),
                    "Contour %s %.0f yd: "
                    "Distanz=%.1f Score=%.1f "
                    "Punkte=%u",
                    left ? "links" : "rechts",
                    offsets[oi],
                    PathDistance(contourPath),
                    contourScore,
                    uint32(contourPath.size()));

                Dbg(player, s, b);

                if (contourScore < bestScore)
                {
                    bestScore = contourScore;
                    bestPath = contourPath;
                    bestType = contourType;
                    bestIncomplete = contourIncomplete;

                    Dbg(
                        player,
                        s,
                        left
                            ? "Contour-Pfad links gewinnt."
                            : "Contour-Pfad rechts gewinnt.");
                }
            }
        }
    }

    /*
     * -------------------------------------------------------------
     * Normal path / contour path found.
     * -------------------------------------------------------------
     */
    if (!bestPath.empty())
    {
        s.lastPathType = bestType;
        s.path = bestPath;
        s.idx = 1;
        s.pathIncomplete = bestIncomplete;

        char b[256];

        std::snprintf(
            b,
            sizeof(b),
            "Bester Pfad: Score=%.1f "
            "Distanz=%.1f Type=0x%X (%s) "
            "Punkte=%u Incomplete=%u",
            bestScore,
            PathDistance(bestPath),
            bestType,
            PathTypeName(bestType).c_str(),
            uint32(bestPath.size()),
            bestIncomplete ? 1u : 0u);

        Dbg(player, s, b);

        return true;
    }

    /*
     * -------------------------------------------------------------
     * Original local fallback.
     *
     * This is only used if the actual destination isn't reachable.
     * It is NOT part of contour probing.
     * -------------------------------------------------------------
     */
    static float const RINGS[2] =
    {
        12.0f,
        30.0f
    };

    Movement::PointsArray pts;

    uint32 type = PATHFIND_NOPATH;
    bool incomplete = false;

    for (uint8 r = 0; r < 2; ++r)
    {
        for (uint8 a = 0; a < 8; ++a)
        {
            float ang =
                float(a) *
                (6.28318531f / 8.0f);

            float x =
                s.destX +
                std::cos(ang) *
                RINGS[r];

            float y =
                s.destY +
                std::sin(ang) *
                RINGS[r];

            float z =
                BestGroundZ(
                    player,
                    x,
                    y);

            if (z <= INVALID_HEIGHT)
                continue;

            Movement::PointsArray candidate;
            uint32 candidateType =
                PATHFIND_NOPATH;

            bool candidateIncomplete =
                false;

            bool success =
                TryPath(
                    player,
                    x,
                    y,
                    z,
                    false,
                    candidate,
                    candidateType,
                    candidateIncomplete);

            if (!success)
            {
                success =
                    TryPath(
                        player,
                        x,
                        y,
                        z,
                        true,
                        candidate,
                        candidateType,
                        candidateIncomplete);
            }

            if (!success)
                continue;

            char b[192];

            std::snprintf(
                b,
                sizeof(b),
                "Ziel um %.0f yd versetzt "
                "(urspruengliche Stelle nicht begehbar).",
                RINGS[r]);

            Dbg(player, s, b);

            s.destX = x;
            s.destY = y;
            s.destZ = z;

            s.lastPathType = candidateType;
            s.path = candidate;
            s.idx = 1;
            s.pathIncomplete = candidateIncomplete;

            return true;
        }
    }

    s.lastPathType = type;

    char buf[256];

    std::snprintf(
        buf,
        sizeof(buf),
        "Kein Pfad. Letzter Typ 0x%X (%s), "
        "%u Hoehen und 16 Nachbarpunkte geprueft. "
        "Ziel %.1f / %.1f / %.1f",
        type,
        PathTypeName(type).c_str(),
        zn,
        s.destX,
        s.destY,
        s.destZ);

    Dbg(player, s, buf);

    return false;
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
            uint32 aura = si->Effects[i].ApplyAuraName;

            if (aura == SPELL_AURA_MOUNTED)
                mounted = true;
            else if (aura == SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)
                speed = std::max<int32>(speed, si->Effects[i].BasePoints);
            else if (aura >= 206 && aura <= 208)
            {
                // 206-208 sind die Flugtempo-Auren. Die Enum-Namen dafuer
                // unterscheiden sich zwischen den Cores, die Werte nicht.
                flying = true;
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
    // --- Taxiflug ------------------------------------------------------------
    // Waehrend eines Fluges wird NICHTS angefasst. Insbesondere darf hier kein
    // StopMoving() laufen: das loescht den Spline des FlightPathMovementGenerator.
    // Der Spieler sitzt dann zwar auf dem Greifen, fliegt aber nicht und kann
    // stattdessen zu Fuss herumlaufen -- genau dieses Fehlerbild.
    if (player->IsInFlight())
    {
        if (s.controlTaken)
            ReleaseControl(player, s);     // ohne StopMoving

        s.wasInFlight = true;
        if (s.state != AT_WAIT_FLIGHT)
        {
            s.state = AT_WAIT_FLIGHT;
            Msg(player, "Flug laeuft - AutoTravel wartet auf die Landung.");
            PushStatus(player, s);
        }
        return;
    }

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

    if (player->GetVehicle())
    {
        HaltMovement(player, s);
        return;
    }

    // --- Kein Kontakt zur begehbaren Flaeche --------------------------------
    // Zwei Stoerungen mit demselben Bild: der Charakter huepft in der
    // Fallanimation und laeuft dabei geradeaus weiter.
    //
    //   zu tief  -> durch den Boden gefallen, kommt nie wieder hoch
    //   zu hoch  -> haengt in der Luft und sinkt langsam ab
    //
    // Bezugsgroesse ist NICHT die rohe Bodenhoehe, sondern die Reisehoehe:
    // beim Schwimmen ist das die Wasseroberflaeche, sonst der Boden. Ohne das
    // waere jeder Schwimmzug ein Fehlalarm, weil der Seegrund weit unten liegt.
    if (ATConf.rescueUnderMesh && s.state != AT_WAIT_FLIGHT && s.state != AT_COMBAT_PAUSED
        && !player->IsInFlight())
    {
        float px = player->GetPositionX();
        float py = player->GetPositionY();
        float pz = player->GetPositionZ();

        // --- Bezugshoehe bestimmen -----------------------------------------
        // Frueher wurde von pz+2 aus abwaerts gesucht. Auf Treppen und Rampen
        // ist das falsch: rutscht der Charakter mehr als zwei Yards durch die
        // Stufe, liegt pz+2 UNTER der Stufe, und die Suche findet als naechste
        // Flaeche das Rohgelaende - in Sturmwind rund 35 Yards tiefer unter
        // der Stadt. Die Rettung hat den Charakter dann genau dorthin gesetzt.
        //
        // Verlaesslicher ist der Pfad selbst: seine Punkte stammen aus dem
        // NavMesh und liegen damit auf der richtigen begehbaren Ebene. Gesucht
        // wird deshalb um den naechstgelegenen Pfadpunkt herum, und nur in
        // einem engen Fenster - so kann keine ganz andere Etage gewinnen.
        float refBase = pz;
        bool haveRef = false;

        if (!s.path.empty())
        {
            float best = 1.0e30f;
            for (size_t i = 0; i < s.path.size(); ++i)
            {
                float dx = s.path[i].x - px, dy = s.path[i].y - py;
                float d2 = dx * dx + dy * dy;
                if (d2 < best) { best = d2; refBase = s.path[i].z; haveRef = true; }
            }
        }
        if (!haveRef && s.hasGoodZ)
        {
            refBase = s.lastGoodZ;
            haveRef = true;
        }

        float const range = std::max(4.0f, ATConf.rescueSearchRange);
        float ground = player->GetMap()->GetHeight(player->GetPhaseMask(), px, py,
                                                   refBase + 2.0f, true, range);
        if (ground <= INVALID_HEIGHT)
            ground = player->GetMap()->GetHeight(player->GetPhaseMask(), px, py,
                                                 pz + 2.0f, true, range);
        if (ground <= INVALID_HEIGHT)
            ground = BestGroundZ(player, px, py);

        float ref = (ground > INVALID_HEIGHT) ? TravelZ(player, px, py, ground) : INVALID_HEIGHT;

        // Sicherung: liegt die gefundene Flaeche weit unter der Pfadhoehe,
        // ist es eine andere Etage. Dann lieber gar nicht zuruecksetzen.
        if (haveRef && ref > INVALID_HEIGHT && ref < refBase - range)
        {
            Dbg(player, s, "Rettung abgebrochen: gefundene Flaeche liegt eine Etage tiefer.");
            s.offMeshHits = 0;
            ref = INVALID_HEIGHT;
        }

        if (ref > INVALID_HEIGHT && std::fabs(pz - ref) <= ATConf.underMeshDepth)
        {
            s.lastGoodZ = pz;      // hier stand der Charakter sauber
            s.hasGoodZ  = true;
        }

        bool tooLow  = (ref > INVALID_HEIGHT) && (pz < ref - ATConf.underMeshDepth);
        bool tooHigh = (ref > INVALID_HEIGHT) && (pz > ref + ATConf.aboveMeshHeight);

        if (tooLow || tooHigh)
        {
            // Mehrere Messungen hintereinander, damit ein Sprung, eine Rampe
            // oder ein Punkt unter einer Bruecke nicht faelschlich ausloest.
            if (++s.offMeshHits >= 3)
            {
                s.offMeshHits = 0;
                ++s.rescueCount;

                float target = ref + 0.5f;
                HaltMovement(player, s);
                player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
                player->NearTeleportTo(px, py, target, player->GetOrientation());
                player->SetFallInformation(0, target);

                char b[224];
                std::snprintf(b, sizeof(b),
                              "Kein Bodenkontakt (%.1f statt %.1f, %s) - zurueckgesetzt "
                              "und neu berechnet.",
                              pz, target, tooLow ? "unter der Flaeche" : "in der Luft");
                Msg(player, b);

                s.path.clear();
                s.idx = 0;
                s.state = AT_CALCULATE_PATH;

                if (s.rescueCount >= 6)
                {
                    Msg(player, "Zu oft ohne Bodenkontakt - Navigation gestoppt.");
                    s.state = AT_IDLE;
                }
                return;
            }
        }
        else
            s.offMeshHits = 0;
    }

    // --- Untergetaucht --------------------------------------------------------
    // Normalfall ist Schwimmen an der Oberflaeche; die Pfadpunkte liegen dafuer
    // knapp unter dem Wasserspiegel. Bleibt der Charakter trotzdem laenger
    // untergetaucht, wird abgebrochen, bevor die Luft ausgeht.
    if (player->IsUnderWater() && s.state != AT_WAIT_FLIGHT)
    {
        s.underwaterTimer += diff;
        if (!ATConf.swim || s.underwaterTimer > ATConf.maxUnderwaterMs)
        {
            HaltMovement(player, s);
            Msg(player, ATConf.swim
                ? "Zu lange unter Wasser - Reise beendet, bevor die Luft ausgeht."
                : "Der Weg fuehrt durch Wasser - Schwimmen ist abgeschaltet.");
            s.state = AT_IDLE;
            return;
        }
    }
    else
        s.underwaterTimer = 0;

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
    bool lastLeg = (s.legIdx + 1 >= s.route.size());
    float radius = lastLeg ? ArrivalDist(s) : ATConf.legDistance;

    float dist = player->GetExactDist2d(s.destX, s.destY);
    if (dist <= radius && s.state != AT_MOUNTING && s.state != AT_WAIT_FLIGHT)
    {
        HaltMovement(player, s);

        // Flugpunkt erreicht: der Spieler muss den Flug selbst starten,
        // ein Addon darf das in 3.3.5a nicht (geschuetzte Funktion).
        ATLeg const& cur = s.route[s.legIdx];
        if (!lastLeg && (cur.flags & (AT_LEG_FLIGHT | AT_LEG_SPECIAL)))
        {
            s.state = AT_WAIT_FLIGHT;
            s.wasInFlight = false;

            // Zielangabe: Name des naechsten Punktes, sonst das Reiseziel.
            std::string where = cur.nextName;
            if (where.empty() && s.legIdx + 1 < s.route.size())
                where = s.route[s.legIdx + 1].name;
            if (where.empty())
                where = "Richtung " + s.destName;

            float rest = 0.0f;
            if (s.legIdx + 1 < s.route.size() && s.route[s.legIdx + 1].resolved)
                rest = player->GetExactDist2d(s.route[s.legIdx + 1].wx,
                                              s.route[s.legIdx + 1].wy);

            std::string restTxt;
            if (rest > 0.0f)
                restTxt = " (" + std::to_string(int(rest)) + " yd)";

            char mb[320];
            if (cur.flags & AT_LEG_SPECIAL)
                std::snprintf(mb, sizeof(mb),
                    "Weiter per %s nach: %s%s. Nimm die Verbindung - AutoTravel "
                    "macht danach von selbst weiter.",
                    LinkTypeNameFor(cur.linkType), where.c_str(), restTxt.c_str());
            else
                std::snprintf(mb, sizeof(mb),
                    "Flugmeister erreicht. Nimm den Flug nach: %s%s. AutoTravel setzt "
                    "die Reise nach der Landung selbst fort.",
                    where.c_str(), restTxt.c_str());
            Msg(player, mb);
            PushStatus(player, s);
            return;
        }

        if (AdvanceLeg(player, s))
        {
            PushStatus(player, s);
            return;
        }

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
            uint32 grace = s.graceOverride ? s.graceOverride : ATConf.combatGraceMs;
            if (s.combatTimer < grace)
                return;

            Msg(player, "Kampf beendet - berechne neuen Pfad von der aktuellen Position.");
            s.state = AT_CALCULATE_PATH;
            s.repathAttempts = 0;
            s.mountTried = false;
            PushStatus(player, s);
            return;
        }

        // -------------------------------------------------------------------
        case AT_WAIT_FLIGHT:
        {
            // Auch ohne erkannten Flug fortsetzen, sobald der Charakter in der
            // Naehe des naechsten Punktes auftaucht - das deckt Portale,
            // Schiffe und Zeppeline mit ab.
            if (!s.wasInFlight && s.legIdx + 1 < s.route.size())
            {
                ATLeg const& nxt = s.route[s.legIdx + 1];
                if (nxt.resolved && player->GetExactDist2d(nxt.wx, nxt.wy) < 80.0f)
                {
                    Msg(player, "Verbindung genutzt - Reise wird fortgesetzt.");
                    if (!AdvanceLeg(player, s))
                    {
                        s.state = AT_ARRIVED;
                        PushStatus(player, s);
                        Msg(player, "Ziel erreicht - " + s.destName + ".");
                        s.state = AT_IDLE;
                    }
                    PushStatus(player, s);
                    return;
                }
            }

            if (s.wasInFlight)
            {
                s.wasInFlight = false;
                Msg(player, "Gelandet - Reise wird fortgesetzt.");
                if (!AdvanceLeg(player, s))
                {
                    s.state = AT_ARRIVED;
                    PushStatus(player, s);
                    Msg(player, "Ziel erreicht - " + s.destName + ".");
                    s.state = AT_IDLE;
                }
                PushStatus(player, s);
            }
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

                    // Nicht die ganze Reise wegwerfen: der naechste
                    // Stuetzpunkt ist oft von hier aus erreichbar.
                    if (s.legIdx + 1 < s.route.size())
                    {
                        char sb[160];
                        std::snprintf(sb, sizeof(sb),
                                      "Stuetzpunkt %u nicht erreichbar - ueberspringe ihn.",
                                      uint32(s.legIdx + 1));
                        Msg(player, sb);
                        if (AdvanceLeg(player, s))
                        {
                            PushStatus(player, s);
                            return;
                        }
                    }

                    if (s.lastPathType & PATHFIND_NOT_USING_PATH)
                        Msg(player, "Fuer diese Kartenkachel sind keine mmaps geladen. Der Server "
                                    "kann hier keinen Weg berechnen - eine Luftlinie waere quer "
                                    "durch Berge und wird abgelehnt.");
                    else
                        Msg(player, "Kein begehbarer Weg gefunden. Navigation gestoppt. "
                                    "'at diag' im Addon zeigt, woran es liegt.");
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
