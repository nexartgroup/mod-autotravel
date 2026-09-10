/*
 * AutoTravel_Path.cpp
 * ---------------------------------------------------------------------------
 * Wegfindung ueber das NavMesh des Servers, Bewertung der gefundenen Wege und
 * die Suche um Hindernisse herum.
 *
 * Grundgedanke: der PathGenerator liefert EINEN begehbaren Weg. Begehbar heisst
 * aber nicht sinnvoll. Er fuehrt bereitwillig senkrecht einen Berg hinauf, wenn
 * das die kuerzeste Verbindung ist. Ein Mensch geht dort herum.
 *
 * Deshalb werden mehrere Kandidaten erzeugt und bewertet:
 *
 *   * dieselbe Zielstelle mit verschiedenen Zielhoehen (Steg, Bruecke, Etage)
 *   * geglaettet und als Eckpunkte
 *   * bei erkanntem Berganstieg zusaetzlich vier Wege seitlich daran vorbei
 *
 * Die Bewertung rechnet durchgehend in Yards, damit sie mit der Wegstrecke
 * vergleichbar bleibt. Ein Umweg von 30 Yards, der einen steilen Anstieg
 * vermeidet, gewinnt; ein Umweg von 3 Kilometern nicht.
 */

#include "AutoTravel.h"

#include "GridDefines.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    constexpr float AT_PI = 3.14159265358979323846f;
}

// ---------------------------------------------------------------------------
// Ein einzelner Pathfinding-Versuch
// ---------------------------------------------------------------------------

bool AutoTravelMgr::TryPathBetween(Player* player,
                                   float sx, float sy, float sz,
                                   float dx, float dy, float dz,
                                   bool straight, ATPathResult& out) const
{
    out.valid = false;
    out.points.clear();
    out.type = PATHFIND_NOPATH;
    out.incomplete = false;
    out.score = 1.0e30f;

    if (!player)
        return false;

    PathGenerator gen(player);
    gen.SetUseStraightPath(straight);

    bool built = gen.CalculatePath(sx, sy, sz, dx, dy, dz, false);

    out.type = uint32(gen.GetPathType());
    out.points = gen.GetPath();

    if (ATConf.debug)
    {
        // Vorsicht: diese Funktion laeuft auch fuer Spieler OHNE Sitzung
        // (".at diag"). Die Vorfassung griff hier mit _sessions.at() zu und
        // warf dabei std::out_of_range -- ein Absturz des Weltservers durch
        // einen harmlosen Diagnosebefehl.
        ATSession const* s = Find(player);
        if (s)
        {
            char b[288];
            std::snprintf(b, sizeof(b),
                          "PathGenerator: %.1f/%.1f/%.1f -> %.1f/%.1f/%.1f | "
                          "built=%u type=0x%X (%s) Punkte=%u",
                          sx, sy, sz, dx, dy, dz, built ? 1u : 0u, out.type,
                          AT::PathTypeName(out.type).c_str(), uint32(out.points.size()));
            Dbg(player, *s, b);
        }
    }

    if (!built || out.points.size() < 2)
        return false;

    if (out.type & PATHFIND_NOPATH)
        return false;

    if (out.type & PATHFIND_NOT_USING_PATH)
        return false;

    // Eine reine Abkuerzung ohne NavMesh ist eine Luftlinie. Auf kurze Distanz
    // ist das harmlos (Treppenabsatz, Tuerschwelle), auf lange fuehrt sie quer
    // durch Berge.
    if ((out.type & PATHFIND_SHORTCUT) && !(out.type & PATHFIND_NORMAL))
    {
        if (AT::Dist2D(sx, sy, dx, dy) > 40.0f)
            return false;
    }

    // --- Plausibilitaet der NavMesh-Punkte ---------------------------------
    //
    // Geprueft wird, ob an jeder Stelle IRGENDEINE Flaeche zur Pfadhoehe passt.
    // Eine Bruecke ist eine gueltige Flaeche ueber dem Gelaende; wer nur gegen
    // die Gelaendehoehe prueft, verwirft in Sturmwind jeden Steg und jede
    // Kanalbruecke -- und damit die ganze Route.
    //
    // Ein einzelner unpassender Punkt kippt deshalb nichts. Verworfen wird
    // erst, wenn ein Viertel des Pfades nirgends aufliegt.
    Map* map = player->GetMap();
    if (!map)
        return false;

    uint32 phase = player->GetPhaseMask();
    uint32 offSurface = 0;
    std::vector<float> planes;

    for (size_t i = 0; i < out.points.size(); ++i)
    {
        G3D::Vector3 const& p = out.points[i];
        bool onSurface = false;

        FindGroundPlanes(player, p.x, p.y, p.z, planes);
        for (float pl : planes)
        {
            if (p.z > pl - 5.0f && p.z < pl + 8.0f)
            {
                onSurface = true;
                break;
            }
        }

        if (!onSurface)
        {
            float ground = map->GetHeight(phase, p.x, p.y, p.z + 5.0f, true, 200.0f);
            if (ground <= INVALID_HEIGHT)
                ground = BestGroundZ(player, p.x, p.y);

            if (ground > INVALID_HEIGHT && p.z > ground - 5.0f && p.z < ground + 8.0f)
                onSurface = true;
        }

        if (!onSurface)
            ++offSurface;
    }

    if (offSurface * 4 > out.points.size())
        return false;

    out.incomplete = (out.type & PATHFIND_INCOMPLETE) != 0;
    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Streckenlaenge
// ---------------------------------------------------------------------------

float AutoTravelMgr::PathDistance(Movement::PointsArray const& path) const
{
    if (path.size() < 2)
        return 0.0f;

    float distance = 0.0f;
    for (size_t i = 1; i < path.size(); ++i)
        distance += AT::Dist2D(path[i].x, path[i].y, path[i - 1].x, path[i - 1].y);
    return distance;
}

// ---------------------------------------------------------------------------
// Klippen im Pfad zaehlen
// ---------------------------------------------------------------------------
//
// Zwischen zwei NavMesh-Punkten kann ein Absatz liegen, der begehbar ist (das
// NavMesh laesst Faelle bis zu einer gewissen Hoehe zu), fuer den Spieler aber
// Fallschaden bedeutet. Der Spline faehrt so etwas ohne Ruecksicht ab.
//
// Erkannt wird das an einem Hoehensturz ueber eine sehr kurze horizontale
// Strecke. Ein Hang faellt ueber viele Yards, eine Klippe auf einmal.

uint32 AutoTravelMgr::CountCliffs(Player* player, Movement::PointsArray const& path) const
{
    if (path.size() < 2)
        return 0;

    uint32 cliffs = 0;
    float const limit = std::max(4.0f, ATConf.maxStepDown * 2.0f);

    for (size_t i = 1; i < path.size(); ++i)
    {
        float horizontal = AT::Dist2D(path[i].x, path[i].y, path[i - 1].x, path[i - 1].y);
        float drop = path[i - 1].z - path[i].z;

        if (drop <= limit)
            continue;

        // Faellt es steiler als eine begehbare Rampe, ist es ein Absturz.
        if (horizontal < 0.01f || drop / horizontal > ATConf.maxWalkSlope * 2.0f)
        {
            // Im Wasser ist ein Sprung folgenlos.
            float lvl = 0.0f, grd = 0.0f;
            if (WaterSurface(player, path[i].x, path[i].y, path[i].z + 2.0f, lvl, grd))
                continue;
            ++cliffs;
        }
    }

    return cliffs;
}

// ---------------------------------------------------------------------------
// Bewertung eines Weges
// ---------------------------------------------------------------------------
//
// Grundkosten sind die Wegstrecke in Yards. Alles Weitere sind Aufschlaege in
// derselben Einheit, damit sich Umweg und Gelaende gegeneinander abwaegen
// lassen.

float AutoTravelMgr::ScoreNaturalPath(Player* player,
                                      Movement::PointsArray const& path,
                                      bool incomplete) const
{
    if (path.size() < 2)
        return 1.0e30f;

    float score = PathDistance(path);

    if (incomplete)
        score += ATConf.incompletePathPenalty;

    // --- Einzelne Steigungen ----------------------------------------------
    for (size_t i = 1; i < path.size(); ++i)
    {
        float horizontal = AT::Dist2D(path[i].x, path[i].y, path[i - 1].x, path[i - 1].y);
        if (horizontal < 0.01f)
            continue;

        float dz = std::fabs(path[i].z - path[i - 1].z);
        float slope = dz / horizontal;

        if (slope > ATConf.slopeStart)
            score += horizontal * (slope - ATConf.slopeStart) * ATConf.slopePenalty;
        if (slope > ATConf.slopeStrong)
            score += horizontal * (slope - ATConf.slopeStrong) * ATConf.steepSlopePenalty;
        if (slope > ATConf.slopeExtreme)
            score += horizontal * (slope - ATConf.slopeExtreme) * ATConf.extremeSlopePenalty;
    }

    // --- Anhaltender Anstieg ----------------------------------------------
    //
    // Erkennt einen Berg, der aus vielen einzeln harmlosen Segmenten besteht.
    //
    // Zwei Fehler der Vorfassung sind hier behoben:
    //
    //   1. Das Fenster startete an JEDEM Punkt. Damit hing die Strafe an der
    //      Punktdichte statt an der Geometrie: derselbe Hang kostete 0 bei
    //      2-Yard-Abtastung und 1000 bei 20-Yard-Abtastung. Geglaettete Pfade
    //      bekamen also gar keine Bergstrafe -- ausgerechnet die, die
    //      normalerweise benutzt werden.
    //   2. Das Fenster war mit 10 Yards zu kurz. Ueber 10 Yards steigt auch ein
    //      langer Berg nur ein bis zwei Yards und blieb unter der Schwelle.
    //
    // Jetzt: Fenster fester Laenge, gesetzt in festen Abstaenden entlang der
    // STRECKE. Damit ist die Strafe von der Punktdichte unabhaengig.
    {
        float const window = std::max(10.0f, ATConf.elevationWindow);
        float const stride = std::max(5.0f, window * 0.5f);

        std::vector<float> along(path.size(), 0.0f);
        for (size_t i = 1; i < path.size(); ++i)
            along[i] = along[i - 1] + AT::Dist2D(path[i].x, path[i].y, path[i - 1].x, path[i - 1].y);

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

    // --- Scharfe Richtungswechsel -----------------------------------------
    if (path.size() >= 3)
    {
        for (size_t i = 1; i + 1 < path.size(); ++i)
        {
            float ax = path[i].x - path[i - 1].x;
            float ay = path[i].y - path[i - 1].y;
            float bx = path[i + 1].x - path[i].x;
            float by = path[i + 1].y - path[i].y;

            float lenA = std::sqrt(ax * ax + ay * ay);
            float lenB = std::sqrt(bx * bx + by * by);
            if (lenA < 0.01f || lenB < 0.01f)
                continue;

            float dot = (ax * bx + ay * by) / (lenA * lenB);
            dot = std::max(-1.0f, std::min(1.0f, dot));
            float angle = std::acos(dot) * 180.0f / AT_PI;

            if (angle > ATConf.turnPenaltyStart)
                score += (angle - ATConf.turnPenaltyStart) * ATConf.turnPenalty;
            if (angle > ATConf.turnPenaltyStrong)
                score += (angle - ATConf.turnPenaltyStrong) * ATConf.strongTurnPenalty;
            if (angle > ATConf.turnPenaltyExtreme)
                score += (angle - ATConf.turnPenaltyExtreme) * ATConf.extremeTurnPenalty;
        }
    }

    // --- Klippen -----------------------------------------------------------
    if (ATConf.cliffPenalty > 0.0f)
        score += float(CountCliffs(player, path)) * ATConf.cliffPenalty;

    // --- Wasser ------------------------------------------------------------
    //
    // Schwimmen ist langsam und im Zweifel gefaehrlich. Ein Weg am Ufer
    // entlang darf deshalb laenger sein als der Weg quer durch den See.
    if (ATConf.waterPenalty > 0.0f && ATConf.swim)
    {
        float swimYards = 0.0f;
        for (size_t i = 1; i < path.size(); ++i)
        {
            float lvl = 0.0f, grd = 0.0f;
            if (WaterSurface(player, path[i].x, path[i].y, path[i].z + 2.0f, lvl, grd))
                swimYards += AT::Dist2D(path[i].x, path[i].y, path[i - 1].x, path[i - 1].y);
        }
        score += swimYards * ATConf.waterPenalty;
    }

    return score;
}

// ---------------------------------------------------------------------------
// Sieht der Weg nach einem Berganstieg aus?
// ---------------------------------------------------------------------------

bool AutoTravelMgr::HasMountainClimb(Movement::PointsArray const& path) const
{
    if (path.size() < 2)
        return false;

    float const window = std::max(5.0f, ATConf.elevationWindow);

    std::vector<float> along(path.size(), 0.0f);
    for (size_t i = 1; i < path.size(); ++i)
        along[i] = along[i - 1] + AT::Dist2D(path[i].x, path[i].y, path[i - 1].x, path[i - 1].y);

    float const total = along.back();
    float const stride = std::max(5.0f, window * 0.5f);
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

        float travelled = along[endIdx] - along[startIdx];
        if (travelled < 10.0f)
            continue;

        float gain = path[endIdx].z - path[startIdx].z;
        if (gain < ATConf.contourTriggerElevation)
            continue;

        if (gain / travelled >= ATConf.contourTriggerSlope)
            return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Einzelne Z-Ausreisser korrigieren
// ---------------------------------------------------------------------------
//
// Steigt oder faellt die Route ueber mehrere Punkte hinweg gleichmaessig, ist
// der Hoehenunterschied echt -- Treppe, Rampe, Hang. Weicht dagegen EIN Punkt
// stark ab, waehrend die Nachbarn davor und danach auf einer Linie liegen, hat
// die Hoehenabfrage dort vermutlich eine darunterliegende Flaeche erwischt.

void AutoTravelMgr::FixPathZOutliers(Player* player, Movement::PointsArray& path) const
{
    if (path.size() < 3)
        return;

    std::vector<float> planes;

    for (size_t i = 1; i + 1 < path.size(); ++i)
    {
        G3D::Vector3 const& a = path[i - 1];
        G3D::Vector3&       b = path[i];
        G3D::Vector3 const& c = path[i + 1];

        float da = AT::Dist2D(b.x, b.y, a.x, a.y);
        float dc = AT::Dist2D(c.x, c.y, b.x, b.y);
        if (da + dc < 0.5f)
            continue;

        float t = da / (da + dc);
        float expected = a.z + (c.z - a.z) * t;

        if (std::fabs(b.z - expected) <= ATConf.zOutlierTolerance)
            continue;                     // passt zum Verlauf: echte Steigung

        FindGroundPlanes(player, b.x, b.y, expected, planes);

        float best = INVALID_HEIGHT;
        float bestDelta = 1.0e30f;
        for (float pl : planes)
        {
            float d = std::fabs(pl - expected);
            if (d < bestDelta)
            {
                bestDelta = d;
                best = pl;
            }
        }

        if (best > INVALID_HEIGHT && bestDelta < ATConf.zOutlierTolerance)
            b.z = best;
    }
}

// ---------------------------------------------------------------------------
// Weg seitlich am Hindernis vorbei
// ---------------------------------------------------------------------------
//
//             Berg
//              /\.
//             /  \.
//   A -------P1  P2------- B
//
// Zwei Zwischenpunkte auf derselben Seite, damit der Weg am Hang ENTLANG
// laeuft statt ihn nur einmal zu streifen. Getestet werden vier Kandidaten
// (links/rechts, nah/weit), also acht Suchpunkte.

bool AutoTravelMgr::BuildContourCandidate(Player* player, ATSession const& s,
                                          float offset, bool left,
                                          ATPathResult& out) const
{
    out.valid = false;

    float startX = player->GetPositionX();
    float startY = player->GetPositionY();
    float startZ = player->GetPositionZ();

    float dx = s.destX - startX;
    float dy = s.destY - startY;
    float distance = std::sqrt(dx * dx + dy * dy);

    if (distance < 20.0f)
        return false;

    float dirX = dx / distance;
    float dirY = dy / distance;

    float sideX = left ? -dirY :  dirY;
    float sideY = left ?  dirX : -dirX;

    float p1Progress = std::max(0.05f, std::min(0.90f, ATConf.contourFirstProgress));
    float p2Progress = std::max(p1Progress + 0.10f, std::min(0.95f, ATConf.contourSecondProgress));

    float p1X = startX + dx * p1Progress + sideX * offset;
    float p1Y = startY + dy * p1Progress + sideY * offset;
    float p2X = startX + dx * p2Progress + sideX * offset;
    float p2Y = startY + dy * p2Progress + sideY * offset;

    float p1Z = BestGroundZ(player, p1X, p1Y);
    float p2Z = BestGroundZ(player, p2X, p2Y);

    if (p1Z <= INVALID_HEIGHT || p2Z <= INVALID_HEIGHT)
        return false;

    float bestScore = 1.0e30f;
    Movement::PointsArray best;
    uint32 bestType = PATHFIND_NOPATH;
    bool bestIncomplete = false;

    for (uint8 pass = 0; pass < 2; ++pass)
    {
        bool straight = (pass == 1);

        ATPathResult a, b, c;
        if (!TryPathBetween(player, startX, startY, startZ, p1X, p1Y, p1Z, straight, a))
            continue;
        if (!TryPathBetween(player, p1X, p1Y, p1Z, p2X, p2Y, p2Z, straight, b))
            continue;
        if (!TryPathBetween(player, p2X, p2Y, p2Z, s.destX, s.destY, s.destZ, straight, c))
            continue;

        Movement::PointsArray combined;
        for (auto const& p : a.points)
            combined.push_back(p);
        for (size_t i = 1; i < b.points.size(); ++i)     // P1 nicht doppelt
            combined.push_back(b.points[i]);
        for (size_t i = 1; i < c.points.size(); ++i)     // P2 nicht doppelt
            combined.push_back(c.points[i]);

        if (combined.size() < 2)
            continue;

        bool combinedIncomplete = a.incomplete || b.incomplete || c.incomplete;

        // Der Umweg darf nicht ins Absurde wachsen.
        if (PathDistance(combined) > distance * ATConf.contourMaxDistanceFactor)
            continue;

        float candidateScore = ATConf.naturalPathing
            ? ScoreNaturalPath(player, combined, combinedIncomplete)
            : PathDistance(combined);

        if (!straight)
            candidateScore -= 0.01f;      // bei Gleichstand die glatte Fassung

        if (combinedIncomplete)
            candidateScore += ATConf.incompletePathPenalty;

        if (candidateScore < bestScore)
        {
            bestScore = candidateScore;
            best = combined;
            bestType = a.type | b.type | c.type;
            bestIncomplete = combinedIncomplete;
        }
    }

    if (best.empty())
        return false;

    out.points = best;
    out.type = bestType;
    out.incomplete = bestIncomplete;
    out.score = bestScore;
    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Pfad zur aktuellen Etappe berechnen
// ---------------------------------------------------------------------------

bool AutoTravelMgr::CalculatePath(Player* player, ATSession& s)
{
    // Fliegen wird zuerst geprueft: eine Luftroute macht die gesamte
    // Bodenwegfindung ueberfluessig.
    if (ShouldFly(player, s) && BuildAirPath(player, s))
        return true;

    Map* map = player->GetMap();
    uint32 phase = player->GetPhaseMask();
    float pz = player->GetPositionZ();

    // --- Zielhoehen sammeln -----------------------------------------------
    //
    // Das Ziel kann auf einem Steg, einer Bruecke, einer Galerie oder schlicht
    // auf dem Gelaende liegen. Fuer jede Moeglichkeit wird ein Kandidat
    // erzeugt; welcher davon begehbar ist, entscheidet der PathGenerator.
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
    addZ(BestGroundZ(player, s.destX, s.destY));
    addZ(map->GetHeight(phase, s.destX, s.destY, MAX_HEIGHT));
    addZ(map->GetHeight(phase, s.destX, s.destY, pz + 5.0f,   true, 400.0f));
    addZ(map->GetHeight(phase, s.destX, s.destY, pz + 40.0f,  true, 400.0f));
    addZ(map->GetHeight(phase, s.destX, s.destY, pz + 120.0f, true, 400.0f));
    addZ(map->GetHeight(phase, s.destX, s.destY, pz + 300.0f, true, 400.0f));

    // --- Alle direkten Kandidaten bewerten ---------------------------------
    //
    // Bewusst NICHT beim ersten gueltigen Treffer abbrechen: der erste gueltige
    // ist haeufig der schlechteste, weil die Zielhoehen von oben nach unten
    // durchprobiert werden.
    ATPathResult best;
    best.score = 1.0e30f;
    float bestDestZ = s.destZ;

    for (uint8 pass = 0; pass < 2; ++pass)
    {
        bool straight = (pass == 1);

        for (uint8 i = 0; i < zn; ++i)
        {
            ATPathResult cand;
            if (!TryPathBetween(player,
                                player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                                s.destX, s.destY, zc[i], straight, cand))
                continue;

            cand.score = ATConf.naturalPathing
                ? ScoreNaturalPath(player, cand.points, cand.incomplete)
                : PathDistance(cand.points);

            if (!straight)
                cand.score -= 0.01f;

            if (cand.score < best.score)
            {
                best = cand;
                bestDestZ = zc[i];
            }

            char b[256];
            std::snprintf(b, sizeof(b),
                          "Direktkandidat %-10s Z=%8.2f Distanz=%7.1f Score=%9.1f %s Punkte=%u",
                          straight ? "Eckpunkte" : "Geglaettet", zc[i],
                          PathDistance(cand.points), cand.score,
                          cand.incomplete ? "TEILWEG" : "vollstaendig",
                          uint32(cand.points.size()));
            Dbg(player, s, b);
        }
    }

    // --- Contour-Suche -----------------------------------------------------
    //
    // Nur wenn der beste bisherige Weg tatsaechlich wie ein Berganstieg
    // aussieht. Andernfalls waeren das acht zusaetzliche Pfadsuchen ohne Grund.
    if (ATConf.contourProbing && best.valid && HasMountainClimb(best.points))
    {
        Dbg(player, s, "Berganstieg erkannt - Contour-Suche wird gestartet.");

        float const offsets[2] = { ATConf.contourNarrowOffset, ATConf.contourWideOffset };

        for (uint8 side = 0; side < 2; ++side)
        {
            bool left = (side == 0);
            for (uint8 oi = 0; oi < 2; ++oi)
            {
                ATPathResult contour;
                if (!BuildContourCandidate(player, s, offsets[oi], left, contour))
                    continue;

                char b[256];
                std::snprintf(b, sizeof(b), "Contour %s %.0f yd: Distanz=%.1f Score=%.1f Punkte=%u",
                              left ? "links" : "rechts", offsets[oi],
                              PathDistance(contour.points), contour.score,
                              uint32(contour.points.size()));
                Dbg(player, s, b);

                if (contour.score < best.score)
                {
                    best = contour;
                    Dbg(player, s, left ? "Contour-Pfad links gewinnt."
                                        : "Contour-Pfad rechts gewinnt.");
                }
            }
        }
    }

    if (best.valid)
    {
        s.destZ = bestDestZ;
        s.lastPathType = best.type;
        s.path = best.points;
        FixPathZOutliers(player, s.path);
        s.idx = 1;
        s.pathIncomplete = best.incomplete;
        s.flying = false;

        char b[256];
        std::snprintf(b, sizeof(b),
                      "Bester Pfad: Score=%.1f Distanz=%.1f Type=0x%X (%s) Punkte=%u %s",
                      best.score, PathDistance(best.points), best.type,
                      AT::PathTypeName(best.type).c_str(), uint32(best.points.size()),
                      best.incomplete ? "TEILWEG" : "vollstaendig");
        Dbg(player, s, b);
        return true;
    }

    // --- Rueckfallebene: Ziel leicht versetzen -----------------------------
    //
    // Kommt vor, wenn das Ziel selbst nicht begehbar ist: mitten in einer Wand,
    // auf einem Dach, im Wasser ueber einem Riff. Dann tut es die naechste
    // erreichbare Stelle daneben.
    static float const RINGS[3] = { 12.0f, 30.0f, 60.0f };

    for (uint8 r = 0; r < 3; ++r)
    {
        for (uint8 a = 0; a < 8; ++a)
        {
            float ang = float(a) * (2.0f * AT_PI / 8.0f);
            float x = s.destX + std::cos(ang) * RINGS[r];
            float y = s.destY + std::sin(ang) * RINGS[r];
            float z = BestGroundZ(player, x, y);

            if (z <= INVALID_HEIGHT)
                continue;

            ATPathResult cand;
            bool ok = TryPathBetween(player,
                                     player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                                     x, y, z, false, cand);
            if (!ok)
                ok = TryPathBetween(player,
                                    player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                                    x, y, z, true, cand);
            if (!ok)
                continue;

            char b[192];
            std::snprintf(b, sizeof(b),
                          "Ziel um %.0f yd versetzt - die urspruengliche Stelle ist nicht begehbar.",
                          RINGS[r]);
            Dbg(player, s, b);

            s.destX = x;
            s.destY = y;
            s.destZ = z;
            s.lastPathType = cand.type;
            s.path = cand.points;
            FixPathZOutliers(player, s.path);
            s.idx = 1;
            s.pathIncomplete = cand.incomplete;
            s.flying = false;
            return true;
        }
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Kein Pfad. %u Zielhoehen und 24 Nachbarpunkte geprueft. Ziel %.1f / %.1f / %.1f",
                  zn, s.destX, s.destY, s.destZ);
    Dbg(player, s, buf);

    return false;
}
