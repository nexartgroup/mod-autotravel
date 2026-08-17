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
#include <vector>

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

    LoadMapAreas();
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
        // Fallbezug zuruecksetzen, bevor der Client wieder selbst rechnet.
        player->SetFallInformation(0, player->GetPositionZ());
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

    // Der Client zaehlt waehrend einer servergesteuerten Bewegung Fallzeit
    // mit, wenn der Spline bergab durch die Luft schneidet. Beim Zurueckgeben
    // der Kontrolle wird daraus Fallschaden, ohne dass der Charakter wirklich
    // gefallen ist. Der Bezugspunkt wird deshalb bei jedem Abschnitt neu
    // gesetzt.
    player->SetFallInformation(0, player->GetPositionZ());

    char buf[128];
    std::snprintf(buf, sizeof(buf), "Movement gestartet: %u Punkte (Index %u/%u)",
                  uint32(chunk.size()), uint32(s.idx), uint32(s.path.size()));
    Dbg(player, s, buf);
}

// ---------------------------------------------------------------------------
// Pathfinding (echtes AzerothCore NavMesh via PathGenerator)
// ---------------------------------------------------------------------------

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

// Ein einzelner Pathfinding-Versuch auf einen konkreten Punkt.
bool AutoTravelMgr::TryPath(Player* player, float x, float y, float z,
                            Movement::PointsArray& out, uint32& typeOut, bool& incomplete) const
{
    PathGenerator gen(player);
    gen.SetUseStraightPath(false);

    bool built = gen.CalculatePath(x, y, z, false);
    typeOut = uint32(gen.GetPathType());

    if (!built || (typeOut & PATHFIND_NOPATH) || gen.GetPath().size() < 2)
        return false;

    // Reine Geradeauslinie ohne NavMesh nur ueber sehr kurze Distanz zulassen.
    if ((typeOut & PATHFIND_SHORTCUT) && !(typeOut & PATHFIND_NORMAL))
    {
        if (player->GetExactDist2d(x, y) > 40.0f)
            return false;
    }

    out = gen.GetPath();
    incomplete = (typeOut & PATHFIND_INCOMPLETE) != 0;
    return true;
}

// ---------------------------------------------------------------------------
// Pfadberechnung mit Hoehen- und Umgebungssuche
// ---------------------------------------------------------------------------
// Der Carbonite-Wegpunkt kommt von einer 2D-Karte, eine Hoehe gibt es dort
// nicht. Die aus der Gelaendehoehe geschaetzte Z-Koordinate kann daneben
// liegen -- an Haengen, unter Bruecken, in Gebaeuden, an Klippen.
//
// Detour sucht das Zielpolygon nur in einem begrenzten Fenster um den
// uebergebenen Punkt. Liegt Z zu weit daneben, findet es gar nichts und
// meldet PATHFIND_NOPATH mit zwei Punkten (Start und Ziel) -- genau das
// Bild aus dem Log.
//
// Deshalb wird nicht ein einziger Punkt probiert, sondern:
//   1. mehrere plausible Hoehen an derselben X/Y-Stelle
//   2. falls alles scheitert, ein Ring benachbarter Punkte
//      (das Ziel liegt dann z. B. im Fels oder auf einem Dach)
//
// Der erste Treffer gewinnt und wird als neues Ziel uebernommen.

bool AutoTravelMgr::CalculatePath(Player* player, ATSession& s)
{
    Map* map = player->GetMap();
    uint32 phase = player->GetPhaseMask();
    float pz = player->GetPositionZ();

    // --- Hoehenkandidaten sammeln -----------------------------------------
    float zc[10];
    uint8 zn = 0;
    auto addZ = [&](float z)
    {
        if (z <= INVALID_HEIGHT || zn >= 10)
            return;
        for (uint8 i = 0; i < zn; ++i)
            if (std::fabs(zc[i] - z) < 1.5f)
                return;
        zc[zn++] = z;
    };

    addZ(s.destZ);
    addZ(map->GetHeight(phase, s.destX, s.destY, MAX_HEIGHT));
    addZ(map->GetHeight(phase, s.destX, s.destY, pz + 5.0f));
    addZ(map->GetHeight(phase, s.destX, s.destY, pz + 60.0f));
    addZ(map->GetHeight(phase, s.destX, s.destY, pz + 200.0f));
    addZ(map->GetHeight(phase, s.destX, s.destY, pz - 60.0f));
    addZ(pz);

    uint32 type = 0;
    bool incomplete = false;
    Movement::PointsArray pts;

    for (uint8 i = 0; i < zn; ++i)
    {
        if (TryPath(player, s.destX, s.destY, zc[i], pts, type, incomplete))
        {
            if (std::fabs(zc[i] - s.destZ) > 1.5f)
            {
                char b[160];
                std::snprintf(b, sizeof(b), "Zielhoehe korrigiert: %.2f -> %.2f", s.destZ, zc[i]);
                Dbg(player, s, b);
            }
            s.destZ = zc[i];
            s.path = pts;
            s.idx = 1;
            s.pathIncomplete = incomplete;

            char b[192];
            std::snprintf(b, sizeof(b), "Pfad ok: type=0x%X (%s) points=%u",
                          type, PathTypeName(type).c_str(), uint32(pts.size()));
            Dbg(player, s, b);
            return true;
        }
    }

    // --- Ring in der Umgebung ---------------------------------------------
    static float const RINGS[2] = { 12.0f, 30.0f };
    for (uint8 r = 0; r < 2; ++r)
    {
        for (uint8 a = 0; a < 8; ++a)
        {
            float ang = float(a) * (6.28318531f / 8.0f);
            float x = s.destX + std::cos(ang) * RINGS[r];
            float y = s.destY + std::sin(ang) * RINGS[r];
            float z = map->GetHeight(phase, x, y, MAX_HEIGHT);
            if (z <= INVALID_HEIGHT)
                z = pz;

            if (TryPath(player, x, y, z, pts, type, incomplete))
            {
                char b[192];
                std::snprintf(b, sizeof(b),
                              "Ziel um %.0f yd versetzt (urspruengliche Stelle nicht begehbar).",
                              RINGS[r]);
                Dbg(player, s, b);
                s.destX = x;
                s.destY = y;
                s.destZ = z;
                s.path = pts;
                s.idx = 1;
                s.pathIncomplete = incomplete;
                return true;
            }
        }
    }

    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "Kein Pfad. Letzter Typ 0x%X (%s), %u Hoehen und 16 Nachbarpunkte geprueft. "
                  "Ziel %.1f / %.1f / %.1f",
                  type, PathTypeName(type).c_str(), zn, s.destX, s.destY, s.destZ);
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
                    Msg(player, "Kein begehbarer Weg zum Ziel gefunden. Navigation gestoppt. "
                                "Der Wegpunkt liegt vermutlich auf einer Flaeche ohne NavMesh "
                                "(Fels, Wasser, Dach). Debug zeigt die geprueften Hoehen.");
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
