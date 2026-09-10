/*
 * AutoTravel_Move.cpp
 * ---------------------------------------------------------------------------
 * Alles, was den Charakter tatsaechlich bewegt: Spline-Abschnitte,
 * Kontrollwechsel zwischen Server und Client, Reittiere und der Flugmodus.
 *
 * Zur Kontrolle: MoveSpline bewegt einen Spieler nur zuverlaessig, solange der
 * Client die Steuerung NICHT hat. Deshalb nimmt das Modul sie waehrend der
 * Fahrt und gibt sie bei jedem Anlass sofort zurueck -- Kampf, Pause, Tod,
 * Kartenwechsel, Taxiflug, Logout. Sie liegt nie laenger beim Server als noetig.
 */

#include "AutoTravel.h"

#include "GridDefines.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// Kontrolle
// ---------------------------------------------------------------------------

void AutoTravelMgr::TakeControl(Player* player, ATSession& s)
{
    if (!ATConf.takeClientControl || s.controlTaken)
        return;

    player->SetClientControl(player, false);
    s.controlTaken = true;
}

void AutoTravelMgr::ReleaseControl(Player* player, ATSession& s)
{
    if (!s.controlTaken)
        return;

    // Fallbezug und Bewegungsflags zuruecksetzen, BEVOR der Client wieder
    // selbst rechnet -- sonst bleibt die Fall- oder Schwimmanimation haengen
    // und der Charakter huepft an Ort und Stelle.
    player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);

    if (s.swimming && !player->IsInWater())
        player->RemoveUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
    s.swimming = false;

    player->SetFallInformation(0, player->GetPositionZ());
    player->SetClientControl(player, true);
    s.controlTaken = false;
}

void AutoTravelMgr::HaltMovement(Player* player, ATSession& s)
{
    // Waehrend eines Taxifluges wuerde StopMoving() den Spline des
    // FlightPathMovementGenerator loeschen. Der Spieler sitzt dann auf dem
    // Greifen, fliegt aber nicht und kann stattdessen zu Fuss herumlaufen.
    // Deshalb hier grundsaetzlich nur die Kontrolle zurueckgeben.
    if (player->IsInFlight())
    {
        ReleaseControl(player, s);
        return;
    }

    player->StopMoving();
    ReleaseControl(player, s);
}

// ---------------------------------------------------------------------------
// Ein Spline-Abschnitt
// ---------------------------------------------------------------------------
//
// MoveSpline simuliert zwischen seinen Punkten keine Gravitation. Werden zwei
// weit auseinanderliegende NavMesh-Punkte direkt verbunden, "fliegt" der
// Charakter ueber Senken hinweg und rutscht in Anstiege hinein.
//
// Deshalb wird der Pfad horizontal verdichtet und jeder erzeugte Punkt auf die
// tatsaechliche Oberflaeche projiziert. Bezug fuer die Sollhoehe ist dabei
// immer der ROUTENVERLAUF, nie die Spielerposition: die kann bereits durch
// eine Treppenstufe gerutscht sein, und dann wandert der Fehler mit.
//
// Im Flugmodus entfaellt die Projektion vollstaendig -- dort sind die Punkte
// bereits Flughoehen.

void AutoTravelMgr::LaunchChunk(Player* player, ATSession& s)
{
    if (s.idx >= s.path.size())
        return;

    float const terrainStep = std::max(0.5f, ATConf.terrainStep);
    constexpr float TERRAIN_OFFSET = 0.10f;

    Movement::PointsArray chunk;

    G3D::Vector3 current(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    chunk.push_back(current);

    bool water = false;
    uint32 sourcePoints = 0;

    // Hoehe, an der das aktuelle Segment beginnt -- aus der Route, nicht aus
    // der Spielerposition.
    float segStartZ = (s.idx > 0) ? s.path[s.idx - 1].z : current.z;
    float prevPlane = INVALID_HEIGHT;

    std::vector<float> planes;

    while (s.idx < s.path.size() && sourcePoints < ATConf.chunkPoints)
    {
        G3D::Vector3 target = s.path[s.idx];

        // --- Flugmodus: Punkte unveraendert uebernehmen --------------------
        if (s.flying)
        {
            chunk.push_back(target);
            current = target;
            ++s.idx;
            ++sourcePoints;
            continue;
        }

        float horizontal = AT::Dist2D(target.x, target.y, current.x, current.y);

        uint32 steps = std::max<uint32>(1, uint32(std::ceil(horizontal / terrainStep)));

        // Deckel gegen Ausreisser: ein einzelnes NavMesh-Segment von mehreren
        // hundert Yards darf nicht tausend Hoehenabfragen ausloesen.
        if (steps > 200)
            steps = 200;

        float const stepLen = horizontal / float(steps);

        for (uint32 step = 1; step <= steps; ++step)
        {
            float t = float(step) / float(steps);
            float x = current.x + (target.x - current.x) * t;
            float y = current.y + (target.y - current.y) * t;

            // Sollhoehe aus dem Routenverlauf, linear zwischen Segmentanfang
            // und NavMesh-Zielpunkt.
            float expectedZ = segStartZ + (target.z - segStartZ) * t;

            FindGroundPlanes(player, x, y, expectedZ, planes);
            float ground = SelectGroundPlaneOnRoute(planes, expectedZ, prevPlane, stepLen);

            if (ground <= INVALID_HEIGHT)
                ground = BestGroundZ(player, x, y);

            float z;
            if (ground > INVALID_HEIGHT)
                z = ground + TERRAIN_OFFSET;   // minimal ueber der Flaeche halten
            else
                z = expectedZ;                 // gar keine Hoehe: Route glauben

            // Wasser: die Reisehoehe ist die Oberflaeche, nicht der Grund.
            float base = (ground > INVALID_HEIGHT) ? ground : z;
            float travel = TravelZ(player, x, y, base);
            if (travel > z)
            {
                z = travel;
                water = true;
            }

            chunk.push_back(G3D::Vector3(x, y, z));

            if (ground > INVALID_HEIGHT)
                prevPlane = ground;
        }

        // current ist die tatsaechlich erzeugte Terrainposition, damit das
        // naechste Segment von der realen Hoehe ausgeht.
        current = chunk.back();

        // Fuer das naechste Segment ist die ROUTENHOEHE des erreichten Punkts
        // der Bezug, nicht die erzeugte Terrainhoehe. Sonst wandert ein
        // einmaliger Fehlgriff durch die ganze Treppe mit.
        segStartZ = target.z;

        ++s.idx;
        ++sourcePoints;
    }

    if (chunk.size() < 2)
        return;

    TakeControl(player, s);

    // Alte Falling-Flags nicht in den neuen Spline uebernehmen.
    player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
    player->SetFallInformation(0, player->GetPositionZ());

    // --- Bewegungsart ------------------------------------------------------
    float velocity;

    if (s.flying)
    {
        player->RemoveUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
        s.swimming = false;
        velocity = player->GetSpeed(MOVE_FLIGHT);
    }
    else if (water)
    {
        player->AddUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
        s.swimming = true;
        velocity = player->GetSpeed(MOVE_SWIM);
    }
    else
    {
        player->RemoveUnitMovementFlag(MOVEMENTFLAG_SWIMMING);
        s.swimming = false;
        velocity = player->GetSpeed(MOVE_RUN);
    }

    if (velocity < 0.1f)
        velocity = 7.0f;                       // Notnagel, falls etwas fehlt

    Movement::MoveSplineInit init(player);
    init.MovebyPath(chunk);
    init.SetWalk(false);
    init.SetVelocity(velocity);

    if (s.flying)
        init.SetFly();

    // Am Ende des Abschnitts in Fahrtrichtung schauen. Ohne das dreht sich der
    // Charakter beim Anhalten in die zuletzt vom Client gemeldete Richtung --
    // was aussieht, als wuerde er beim Laufen den Kopf verlieren.
    if (chunk.size() >= 2)
    {
        G3D::Vector3 const& a = chunk[chunk.size() - 2];
        G3D::Vector3 const& b = chunk.back();
        float dx = b.x - a.x;
        float dy = b.y - a.y;
        if (std::fabs(dx) > 0.001f || std::fabs(dy) > 0.001f)
            init.SetFacing(std::atan2(dy, dx));
    }

    init.Launch();

    player->SetFallInformation(0, player->GetPositionZ());

    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "Abschnitt gestartet: %u Punkte, Schrittweite %.2f, Tempo %.1f, Index %u/%u%s",
                  uint32(chunk.size()), terrainStep, velocity,
                  uint32(s.idx), uint32(s.path.size()),
                  s.flying ? ", Flug" : (water ? ", Wasser" : ""));
    Dbg(player, s, buf);
}

// ---------------------------------------------------------------------------
// Reittiere
// ---------------------------------------------------------------------------
//
// Gesucht wird der schnellste passende Zauber aus dem eigenen Zauberbuch.
// Die Flugtempo-Auren tragen die Werte 206..208; die Enum-Namen dafuer
// unterscheiden sich zwischen den Cores, die Zahlen nicht.

uint32 AutoTravelMgr::PickMount(Player* player, bool wantFlying) const
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
                flying = true;
                speed = std::max<int32>(speed, si->Effects[i].BasePoints);
            }
        }

        if (!mounted)
            continue;
        if (flying != wantFlying)
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

    float remaining = player->GetExactDist2d(s.destX, s.destY);
    if (remaining < ATConf.mountMinDistance)
        return false;

    // Erst das Flugmount versuchen, wenn Fliegen ueberhaupt in Frage kommt.
    // Ausserhalb der Flugzonen gewaehrt derselbe Zauber nur Bodentempo -- das
    // faellt hier nicht negativ auf, es wird dann eben nicht geflogen.
    uint32 spellId = 0;
    bool triedFlying = false;

    if (ATConf.allowFlying && remaining >= ATConf.flyMinDistance)
    {
        spellId = PickMount(player, true);
        triedFlying = (spellId != 0);
    }

    if (!spellId)
        spellId = PickMount(player, false);

    if (!spellId)
    {
        Dbg(player, s, "Kein geeignetes Reittier im Zauberbuch gefunden.");
        return false;
    }

    HaltMovement(player, s);

    // Bewusst NICHT triggered: alle normalen Pruefungen (Zone, Reitkunst,
    // Kampf, Wasser, Innenraum) greifen. Schlaegt es fehl, laufen wir zu Fuss.
    SpellCastResult res = player->CastSpell(player, spellId, false);
    if (res != SPELL_CAST_OK)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Reittier abgelehnt (SpellCastResult %u) - weiter zu Fuss.", uint32(res));
        Dbg(player, s, buf);
        return false;
    }

    s.state = AT_MOUNTING;
    s.mountTimer = 0;
    s.flyChecked = false;
    Dbg(player, s, triedFlying ? "Flugmount wird gerufen." : "Reittier wird gerufen.");
    return true;
}

// ---------------------------------------------------------------------------
// Fliegen
// ---------------------------------------------------------------------------

bool AutoTravelMgr::ShouldFly(Player* player, ATSession& s) const
{
    if (!ATConf.allowFlying)
        return false;
    if (!player->IsMounted())
        return false;

    // Die verlaesslichste Auskunft, ob Fliegen hier erlaubt ist, gibt der
    // Client-Bewegungszustand nach dem Aufsitzen: MOVEMENTFLAG_CAN_FLY setzt
    // der Server nur, wenn Zone und Zauber es hergeben. In Azeroth gewaehrt
    // dasselbe Flugmount lediglich Bodentempo -- dann ist CanFly() falsch und
    // wir laufen ganz normal.
    if (!player->CanFly())
        return false;

    if (player->IsInCombat() || player->IsInFlight() || player->GetVehicle())
        return false;
    if (player->GetMap()->IsBattlegroundOrArena() || player->GetMap()->IsDungeon())
        return false;

    if (player->GetExactDist2d(s.destX, s.destY) < ATConf.flyMinDistance)
        return false;

    return true;
}

// Luftroute: Steigflug, Reiseflug entlang eines Hoehenprofils, Sinkflug.
//
//    Reisehoehe = hoechstes Hindernis im Fenster + Sicherheitsabstand
//
//              ____________
//             /            \.
//        ____/    Berg      \____
//       /                        \.
//      A                          B
//
// Das Profil wird zweimal geglaettet (vorwaerts und rueckwaerts jeweils mit
// dem Maximum der Nachbarschaft), damit die Route VOR dem Berg steigt und
// nicht erst hineinfliegt.

bool AutoTravelMgr::BuildAirPath(Player* player, ATSession& s)
{
    float const sx = player->GetPositionX();
    float const sy = player->GetPositionY();
    float const sz = player->GetPositionZ();

    float const distance = AT::Dist2D(sx, sy, s.destX, s.destY);
    if (distance < ATConf.flyMinDistance)
        return false;

    float const step = std::max(5.0f, ATConf.flySampleStep);
    uint32 samples = uint32(std::ceil(distance / step));
    if (samples < 2)
        return false;
    if (samples > 120)
        samples = 120;                       // laengere Strecken in Etappen

    // --- Hoehenprofil abtasten --------------------------------------------
    std::vector<float> ceiling(samples + 1, INVALID_HEIGHT);
    std::vector<float> px(samples + 1), py(samples + 1);

    float const reach = float(samples) * step;
    float const useDist = std::min(distance, reach);
    float const dirX = (s.destX - sx) / distance;
    float const dirY = (s.destY - sy) / distance;

    bool anyHeight = false;

    for (uint32 i = 0; i <= samples; ++i)
    {
        float d = useDist * float(i) / float(samples);
        px[i] = sx + dirX * d;
        py[i] = sy + dirY * d;

        float prevX = (i == 0) ? sx : px[i - 1];
        float prevY = (i == 0) ? sy : py[i - 1];

        float c = TerrainCeilingBetween(player, prevX, prevY, px[i], py[i], step * 0.5f);
        ceiling[i] = c;
        if (c > INVALID_HEIGHT)
            anyHeight = true;
    }

    if (!anyHeight)
    {
        Dbg(player, s, "Luftroute verworfen: keine Hoehendaten auf der Strecke.");
        return false;
    }

    // Luecken im Profil mit dem letzten bekannten Wert fuellen.
    float last = sz;
    for (uint32 i = 0; i <= samples; ++i)
    {
        if (ceiling[i] <= INVALID_HEIGHT)
            ceiling[i] = last;
        else
            last = ceiling[i];
    }

    // --- Profil glaetten ---------------------------------------------------
    // Zwei Durchlaeufe mit dem Maximum der Nachbarschaft: die Route steigt
    // dadurch rechtzeitig vor dem Hindernis und sinkt erst dahinter.
    uint32 const smoothSpan = std::max<uint32>(1, uint32(std::ceil(60.0f / step)));

    std::vector<float> smoothed = ceiling;
    for (uint32 pass = 0; pass < 2; ++pass)
    {
        std::vector<float> next = smoothed;
        for (uint32 i = 0; i <= samples; ++i)
        {
            uint32 lo = (i > smoothSpan) ? i - smoothSpan : 0;
            uint32 hi = std::min<uint32>(samples, i + smoothSpan);
            float m = smoothed[i];
            for (uint32 j = lo; j <= hi; ++j)
                m = std::max(m, smoothed[j]);
            next[i] = m;
        }
        smoothed.swap(next);
    }

    // --- Punkte bauen ------------------------------------------------------
    Movement::PointsArray path;
    path.push_back(G3D::Vector3(sx, sy, sz));

    float const clearance = ATConf.flyClearance;

    for (uint32 i = 0; i <= samples; ++i)
    {
        float z = smoothed[i] + clearance;

        // Nicht unnoetig hoch: gemessen am Grund direkt unter dem Punkt.
        float groundHere = ceiling[i];
        if (groundHere > INVALID_HEIGHT && z > groundHere + ATConf.flyMaxHeight)
            z = groundHere + ATConf.flyMaxHeight;

        // Nie unter die aktuelle Hoehe sinken, solange gestiegen wird.
        if (i == 0 && z < sz)
            z = sz;

        path.push_back(G3D::Vector3(px[i], py[i], z));
    }

    // --- Sinkflug ----------------------------------------------------------
    // Nur, wenn das echte Ziel innerhalb dieser Etappe liegt. Sonst endet die
    // Etappe auf Reiseflughoehe und wird neu berechnet.
    bool reachesTarget = (useDist >= distance - 1.0f);

    if (reachesTarget)
    {
        float landZ = BestGroundZ(player, s.destX, s.destY);
        if (landZ <= INVALID_HEIGHT)
            landZ = s.destZ;

        float approach = std::max(10.0f, ATConf.flyDescendDistance);
        if (approach < distance)
        {
            float ax = s.destX - dirX * approach;
            float ay = s.destY - dirY * approach;
            float az = std::max(landZ + clearance * 0.5f, path.back().z * 0.5f + landZ * 0.5f);
            path.push_back(G3D::Vector3(ax, ay, az));
        }

        path.push_back(G3D::Vector3(s.destX, s.destY, landZ + 1.5f));
    }

    if (path.size() < 3)
        return false;

    s.path = path;
    s.idx = 1;
    s.flying = true;
    s.pathIncomplete = !reachesTarget;
    s.lastPathType = 0;

    char b[224];
    std::snprintf(b, sizeof(b),
                  "Luftroute: %u Punkte, %.0f von %.0f yd, Reisehoehe bis %.0f yd ueber Grund.",
                  uint32(path.size()), useDist, distance, clearance);
    Dbg(player, s, b);

    return true;
}
