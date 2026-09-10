/*
 * AutoTravel_Terrain.cpp
 * ---------------------------------------------------------------------------
 * Alles, was mit der Frage "wo ist hier eigentlich der Boden" zu tun hat.
 *
 * Das ist in World of Warcraft keine triviale Frage. An ein und derselben
 * Stelle koennen mehrere begehbare Flaechen uebereinander liegen:
 *
 *      Z = 104   Handelsdistrikt (Strassenniveau)
 *      Z =  98   Kanalbruecke
 *      Z =  60   Kanalgrund
 *      Z =  25   Rohgelaende unter der Stadt
 *
 * Map::GetHeight() liefert immer nur EINE davon, naemlich die erste, die von
 * der uebergebenen Starthoehe aus nach unten gefunden wird. Wer die falsche
 * erwischt, setzt den Charakter in den Keller.
 *
 * Deshalb arbeitet dieses Modul konsequent mit einer LISTE von Flaechen und
 * waehlt daraus anhand des Routenverlaufs, nicht anhand der Spielerposition.
 * Die Spielerposition ist genau dann unbrauchbar, wenn man sie am dringendsten
 * braucht: wenn der Charakter gerade durch eine Treppenstufe gerutscht ist.
 */

#include "AutoTravel.h"

#include "GridDefines.h"
#include "Map.h"
#include "Player.h"

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Beste Oberflaeche an einer Stelle
// ---------------------------------------------------------------------------
//
// Map::GetHeight(x, y, MAX_HEIGHT) liefert NUR die Gelaendehoehe: die
// VMap-Abfrage sucht ab dem uebergebenen Z hoechstens DEFAULT_HEIGHT_SEARCH
// (50 yd) nach unten, und von 100000 aus findet sie nichts Gebautes. Unter
// Sturmwind liegt das Rohgelaende rund 35 Yards unter dem Stadtboden -- genau
// deshalb landete ein Teleport frueher "unter der Stadt".
//
// Deshalb wird mit mehreren realistischen Startpunkten und grossem Suchfenster
// abgetastet und die Flaeche genommen, die der eigenen Hoehe am naechsten liegt.

float AutoTravelMgr::BestGroundZ(Player* player, float x, float y) const
{
    if (!player)
        return INVALID_HEIGHT;

    Map* map = player->GetMap();
    if (!map)
        return INVALID_HEIGHT;

    uint32 phase = player->GetPhaseMask();
    float pz = player->GetPositionZ();

    float const starts[7] =
    {
        pz + 300.0f, pz + 120.0f, pz + 40.0f, pz + 5.0f,
        pz - 10.0f,  pz - 40.0f,  pz - 150.0f
    };

    float best = INVALID_HEIGHT;
    float bestDiff = 1.0e9f;

    for (uint8 i = 0; i < 7; ++i)
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

// ---------------------------------------------------------------------------
// Alle Etagen an einer Stelle
// ---------------------------------------------------------------------------
//
// Die Vorfassung suchte hoechstens ZWEI Ebenen. Das reicht fuer einen Torbogen,
// nicht aber fuer die Stellen, an denen es wirklich darauf ankommt: die
// Bank von Sturmwind, das Wirtshaus mit Galerie, die Rampen der Tiefenbahn,
// Ironforge mit drei uebereinanderliegenden Ringen.
//
// Jetzt wird von oben nach unten durchgetastet, bis entweder die eingestellte
// Zahl an Etagen gefunden ist oder das Suchfenster verlassen wurde.
//
// Wichtig ist das enge Fenster: gesucht wird ausschliesslich um die ERWARTETE
// Laufhoehe herum. Eine Flaeche 40 Yards tiefer ist keine Alternative, sondern
// ein anderes Stockwerk, und darf gar nicht erst in die Auswahl geraten.

void AutoTravelMgr::FindGroundPlanes(Player* player, float x, float y, float probeZ,
                                     std::vector<float>& out) const
{
    out.clear();

    if (!player)
        return;

    Map* map = player->GetMap();
    if (!map)
        return;

    uint32 phase = player->GetPhaseMask();

    // Nach oben grosszuegiger als nach unten: eine Rampe steigt an, ein
    // Absacken um mehr als ein paar Yards ist dagegen fast immer ein Fehlgriff.
    float const searchAbove = std::max(6.0f, ATConf.aboveMeshHeight);
    float const searchBelow = std::max(3.0f, ATConf.maxStepDown);

    // Abstand, mit dem unterhalb einer gefundenen Flaeche erneut gesucht wird.
    // Ohne ihn liefert GetHeight() dieselbe Flaeche immer wieder.
    constexpr float PLANE_EPSILON = 0.35f;

    uint32 const wanted = std::max<uint32>(1u, std::min<uint32>(8u, ATConf.groundPlanes));

    float searchZ = probeZ + searchAbove;
    float const floor = probeZ - searchBelow;

    for (uint32 i = 0; i < wanted; ++i)
    {
        // Suchfenster: von searchZ bis hinab zur Untergrenze.
        float window = searchZ - floor;
        if (window <= 0.0f)
            break;

        float h = map->GetHeight(phase, x, y, searchZ, true, window);

        if (h <= INVALID_HEIGHT)
            break;
        if (h < floor || h > probeZ + searchAbove)
            break;

        // Duplikate vermeiden (numerisches Rauschen der VMap-Abfrage).
        bool dup = false;
        for (float p : out)
        {
            if (std::fabs(p - h) < PLANE_EPSILON)
            {
                dup = true;
                break;
            }
        }
        if (dup)
            break;

        out.push_back(h);
        searchZ = h - PLANE_EPSILON;
    }
}

// ---------------------------------------------------------------------------
// Etagenwahl anhand des Routenverlaufs
// ---------------------------------------------------------------------------
//
// Bezug ist die ROUTE, nicht die Spielerposition. Die Punkte der Route stammen
// aus dem NavMesh und liegen damit auf der begehbaren Flaeche, unabhaengig
// davon, wo der Charakter gerade steckt.
//
// Zwei Regeln:
//
//   1. Der Abstand zur erwarteten Hoehe wird SYMMETRISCH bewertet. Die
//      Vorfassung bestrafte nur Spruenge nach oben (Schutz gegen Torboegen) --
//      ein Sturz auf eine tiefere Flaeche war kostenlos, und genau darueber
//      blieb der Charakter unter der Treppe.
//   2. Der Uebergang von der zuletzt akzeptierten Flaeche darf nicht steiler
//      als MaxWalkSlope sein. Was steiler waere, ist keine Lauffleche, sondern
//      ein anderes Stockwerk.

float AutoTravelMgr::SelectGroundPlaneOnRoute(std::vector<float> const& planes,
                                              float expectedZ, float prevPlane,
                                              float horizontalStep) const
{
    if (planes.empty())
        return INVALID_HEIGHT;

    float const step = std::max(0.5f, horizontalStep);

    // Was auf diesem Stueck ueberhaupt an Hoehenaenderung begehbar ist.
    // Nach oben zaehlt zusaetzlich die erlaubte Stufe (Treppen), nach unten
    // der erlaubte Absatz.
    float const maxUp   = step * ATConf.maxWalkSlope + ATConf.maxStepUp;
    float const maxDown = step * ATConf.maxWalkSlope + ATConf.maxStepDown;

    float best = INVALID_HEIGHT;
    float bestScore = 1.0e30f;

    for (float candidate : planes)
    {
        float score = std::fabs(candidate - expectedZ);

        if (prevPlane > INVALID_HEIGHT)
        {
            float change = candidate - prevPlane;
            float limit = (change >= 0.0f) ? maxUp : maxDown;
            float excess = std::fabs(change) - limit;
            if (excess > 0.0f)
                score += 500.0f + excess * 100.0f;
        }

        if (score < bestScore)
        {
            bestScore = score;
            best = candidate;
        }
    }

    // Bleibt nur eine unplausible Flaeche uebrig, ist die Routenhoehe die
    // bessere Auskunft als ein falsches Stockwerk. Lieber einmal nicht
    // korrigieren als in den Keller versetzen.
    if (best <= INVALID_HEIGHT)
        return expectedZ;

    float const tolerance = std::max(maxUp, maxDown) + 2.0f;
    if (std::fabs(best - expectedZ) > tolerance)
        return expectedZ;

    return best;
}

// ---------------------------------------------------------------------------
// Wasser
// ---------------------------------------------------------------------------
//
// Die Pfadpunkte des NavMesh liegen im Wasser am GRUND. Faehrt der Spline sie
// unveraendert ab, laeuft der Charakter ueber den Seeboden und ertrinkt -- bei
// abgegebener Steuerung kann er nicht selbst auftauchen.
//
// Deshalb wird jeder Punkt gegen die Wasseroberflaeche geprueft und, wo noetig,
// knapp darunter gelegt. Damit schwimmt der Charakter an der Oberflaeche
// entlang, die Luft laeuft nicht ab, und die Schwimmanimation stimmt.

bool AutoTravelMgr::WaterSurface(Player* player, float x, float y, float probeZ,
                                 float& level, float& ground) const
{
    level = INVALID_HEIGHT;
    ground = INVALID_HEIGHT;

    if (!ATConf.swim || !player)
        return false;

    Map* map = player->GetMap();
    if (!map)
        return false;

    float g = INVALID_HEIGHT;
    float lvl = map->GetWaterOrGroundLevel(player->GetPhaseMask(), x, y, probeZ, &g);

    if (lvl <= INVALID_HEIGHT || g <= INVALID_HEIGHT)
        return false;

    ground = g;

    // Pfuetze oder Furt: da wird gelaufen, nicht geschwommen.
    if (lvl - g < ATConf.minSwimDepth)
        return false;

    level = lvl;
    return true;
}

float AutoTravelMgr::TravelZ(Player* player, float x, float y, float groundZ) const
{
    float lvl = 0.0f;
    float grd = 0.0f;
    if (WaterSurface(player, x, y, groundZ + 2.0f, lvl, grd))
    {
        float swimZ = lvl - ATConf.swimSurfaceOffset;
        if (swimZ > groundZ)
            return swimZ;
    }
    return groundZ;
}

// ---------------------------------------------------------------------------
// Hoechster Punkt zwischen zwei Stellen
// ---------------------------------------------------------------------------
//
// Wird fuer die Luftroute gebraucht: um zu wissen, wie hoch geflogen werden
// muss, damit der Berg dazwischen nicht im Weg ist. Abgetastet wird das
// Gelaende inklusive Bauwerken, in der eingestellten Schrittweite.
//
// Rueckgabe INVALID_HEIGHT, wenn auf der ganzen Strecke keine Hoehe ermittelbar
// war -- dann ist Fliegen dort nicht sinnvoll planbar.

float AutoTravelMgr::TerrainCeilingBetween(Player* player, float ax, float ay,
                                           float bx, float by, float step) const
{
    if (!player)
        return INVALID_HEIGHT;

    Map* map = player->GetMap();
    if (!map)
        return INVALID_HEIGHT;

    uint32 phase = player->GetPhaseMask();

    float dist = AT::Dist2D(ax, ay, bx, by);
    uint32 samples = uint32(std::ceil(dist / std::max(1.0f, step)));
    if (samples < 1)
        samples = 1;
    if (samples > 400)
        samples = 400;                        // Deckel gegen Rechenausreisser

    float ceiling = INVALID_HEIGHT;

    for (uint32 i = 0; i <= samples; ++i)
    {
        float t = float(i) / float(samples);
        float x = ax + (bx - ax) * t;
        float y = ay + (by - ay) * t;

        // Von weit oben suchen: fuer die Frage "wie hoch ist das Hindernis"
        // ist die OBERSTE Flaeche die richtige, nicht die naechstgelegene.
        float h = map->GetHeight(phase, x, y, MAX_HEIGHT);
        float v = map->GetHeight(phase, x, y, 800.0f, true, 1000.0f);
        if (v > h)
            h = v;

        if (h > ceiling)
            ceiling = h;
    }

    return ceiling;
}
