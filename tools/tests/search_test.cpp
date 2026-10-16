/*
 * search_test.cpp -- Tests fuer AutoTravel_Search.cpp gegen ein selbstgebautes NavMesh
 * ---------------------------------------------------------------------------
 * Das Modell ist eine verkleinerte Exodar:
 *
 *   * ein grosser Innenraum (48 x 48 Felder zu 4 yd, rund 2300 Polygone) auf Hoehe 0,
 *   * eine lange Rampe an seiner fernen Ecke, die um 140 yd ansteigt,
 *   * eine Oberflaeche in 140 yd Hoehe, die ueber der Rampe liegt und zurueck bis
 *     direkt UEBER den Innenraum fuehrt,
 *   * das Ziel steht auf dieser Oberflaeche, senkrecht ueber einer Stelle im Innenraum.
 *
 * Der Start liegt im Innenraum. Der kuerzeste Weg fuehrt durch den ganzen Innenraum zur
 * Rampe, hinauf und auf der Oberflaeche zurueck: rund 950 yd. Die Luftlinie ist 150 yd.
 *
 * Der erste Test haelt fest, was der Core daraus macht (Knotenpool 1024, 74 Polygone):
 * die Suche erschoepft den Pool im Innenraum und liefert den Knoten mit der kleinsten
 * Luftlinie zum Ziel -- die Stelle direkt UNTER dem Ziel. Das ist die Sackgasse an der
 * Wand, in die AutoTravel in der Exodar gelaufen ist. Die uebrigen Tests pruefen, dass
 * AT::NavSearch den Ausgang findet, und dass seine Grenzen greifen.
 *
 * Bauen und starten: tools/check.sh <pfad-zu-azerothcore> (Detour kommt aus deps/).
 * ---------------------------------------------------------------------------
 */

#include "AutoTravel_Geometry.h"
#include "AutoTravel_Search.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourNode.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond) \
    do { \
        ++sChecks; \
        if (!(cond)) { ++sFailures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

namespace
{
    constexpr int   GRID  = 48;       // Felder je Seite im Innenraum
    constexpr float SQ    = 4.0f;     // Seitenlaenge eines Feldes (yd)
    constexpr float TOP   = 140.0f;   // Hoehe der Oberflaeche
    constexpr unsigned short WALK = 0x01;

    struct Mesh
    {
        dtNavMesh* nav = nullptr;
        int polyCount = 0;
        float xTop = 0.0f;
        ~Mesh() { if (nav) dtFreeNavMesh(nav); }
    };

    // Ein Feld zwischen (x0,y0) und (x1,y1), Weltkoordinaten; Hoehe an x0 und an x1.
    struct Builder
    {
        float cs = 0.5f, ch = 0.25f;
        float bmin[3] = { -8.0f, -10.0f, -8.0f };      // Detour: x = Welt-y, y = Hoehe, z = Welt-x
        std::vector<unsigned short> verts;
        std::map<std::tuple<int, int, int>, int> index;
        std::vector<std::array<int, 4>> quads;

        int Vertex(float wx, float wy, float wz)
        {
            int const qx = int(std::lround((wy - bmin[0]) / cs));
            int const qy = int(std::lround((wz - bmin[1]) / ch));
            int const qz = int(std::lround((wx - bmin[2]) / cs));
            auto const key = std::make_tuple(qx, qy, qz);
            auto it = index.find(key);
            if (it != index.end())
                return it->second;
            int const idx = int(verts.size() / 3);
            verts.push_back((unsigned short)qx);
            verts.push_back((unsigned short)qy);
            verts.push_back((unsigned short)qz);
            index[key] = idx;
            return idx;
        }

        // Die Windung ist die, die Recast erzeugt (rueckwaerts zur Reihenfolge a, b, c, d).
        void Quad(float x0, float y0, float x1, float y1, float hAtX0, float hAtX1)
        {
            int const a = Vertex(x0, y0, hAtX0);
            int const b = Vertex(x0, y1, hAtX0);
            int const c = Vertex(x1, y1, hAtX1);
            int const d = Vertex(x1, y0, hAtX1);
            quads.push_back({ a, d, c, b });
        }
    };

    // Baut aus den Vierecken das NavMesh (eine Kachel).
    bool Finish(Builder& b, Mesh& m)
    {
        int const polyCount = int(b.quads.size());
        int const nvp = 4;
        std::vector<unsigned short> polys(size_t(polyCount) * nvp * 2, 0xffff);
        std::map<std::pair<int, int>, int> edges;
        for (int p = 0; p < polyCount; ++p)
            for (int e = 0; e < 4; ++e)
                edges[{ b.quads[p][e], b.quads[p][(e + 1) % 4] }] = p;
        for (int p = 0; p < polyCount; ++p)
        {
            for (int e = 0; e < 4; ++e)
            {
                polys[size_t(p) * nvp * 2 + e] = (unsigned short)b.quads[p][e];
                auto it = edges.find({ b.quads[p][(e + 1) % 4], b.quads[p][e] });
                if (it != edges.end())
                    polys[size_t(p) * nvp * 2 + nvp + e] = (unsigned short)it->second;
            }
        }
        std::vector<unsigned short> flags(size_t(polyCount), WALK);
        std::vector<unsigned char> areas(size_t(polyCount), 1);

        float maxv[3] = { 0.0f, 0.0f, 0.0f };
        for (size_t i = 0; i < b.verts.size(); i += 3)
        {
            maxv[0] = std::fmax(maxv[0], b.verts[i] * b.cs + b.bmin[0]);
            maxv[1] = std::fmax(maxv[1], b.verts[i + 1] * b.ch + b.bmin[1]);
            maxv[2] = std::fmax(maxv[2], b.verts[i + 2] * b.cs + b.bmin[2]);
        }

        dtNavMeshCreateParams p;
        std::memset(&p, 0, sizeof(p));
        p.verts = b.verts.data();
        p.vertCount = int(b.verts.size() / 3);
        p.polys = polys.data();
        p.polyFlags = flags.data();
        p.polyAreas = areas.data();
        p.polyCount = polyCount;
        p.nvp = nvp;
        for (int i = 0; i < 3; ++i)
        {
            p.bmin[i] = b.bmin[i];
            p.bmax[i] = maxv[i] + 1.0f;
        }
        p.walkableHeight = 2.0f;
        p.walkableRadius = 0.5f;
        p.walkableClimb = 1.0f;
        p.cs = b.cs;
        p.ch = b.ch;
        p.buildBvTree = true;

        unsigned char* data = nullptr;
        int dataSize = 0;
        if (!dtCreateNavMeshData(&p, &data, &dataSize))
            return false;

        dtNavMeshParams np;
        std::memset(&np, 0, sizeof(np));
        for (int i = 0; i < 3; ++i)
            np.orig[i] = b.bmin[i];
        np.tileWidth = p.bmax[0] - p.bmin[0];
        np.tileHeight = p.bmax[2] - p.bmin[2];
        np.maxTiles = 1;
        np.maxPolys = 8192;

        m.nav = dtAllocNavMesh();
        if (!m.nav || dtStatusFailed(m.nav->init(&np)))
            return false;
        if (dtStatusFailed(m.nav->addTile(data, dataSize, DT_TILE_FREE_DATA, 0, nullptr)))
            return false;
        m.polyCount = polyCount;
        return true;
    }

    bool BuildMesh(Mesh& m, bool withIsland)
    {
        Builder b;

        for (int i = 0; i < GRID; ++i)
            for (int j = 0; j < GRID; ++j)
                b.Quad(i * SQ, j * SQ, (i + 1) * SQ, (j + 1) * SQ, 0.0f, 0.0f);

        // Rampe entlang +x an der Ecke y in [0,4], 2 yd Anstieg je Feld.
        float const x = GRID * SQ;
        int const steps = int(TOP / 2.0f);
        for (int k = 0; k < steps; ++k)
            b.Quad(x + k * SQ, 0.0f, x + (k + 1) * SQ, SQ, 2.0f * k, 2.0f * (k + 1));
        m.xTop = x + steps * SQ;

        // Absatz oben entlang +y, dann ein Gang zurueck entlang -x ueber den Innenraum.
        int const landing = 24;
        for (int k = 0; k < landing; ++k)
            b.Quad(m.xTop, k * SQ, m.xTop + SQ, (k + 1) * SQ, TOP, TOP);
        float const yEnd = landing * SQ;
        int const back = int((m.xTop - 60.0f) / SQ);
        for (int k = 0; k < back; ++k)
        {
            float const x1 = m.xTop - k * SQ;
            b.Quad(x1 - SQ, yEnd - SQ, x1, yEnd, TOP, TOP);
        }

        // Eine Insel ohne Verbindung (Ziel, das es nicht zu erreichen gibt).
        if (withIsland)
            b.Quad(600.0f, 0.0f, 604.0f, 4.0f, 0.0f, 0.0f);

        return Finish(b, m);
    }

    // Zwei Wege zwischen links und rechts (11 x 5 Felder zu 4 yd):
    //
    //   Reihe 0:  der kuerzere Weg, mit einem steilen Huegel in den Spalten 4..6 (HUMP yd hoch)
    //   Reihe 4:  ein flacher Umweg, in den Spalten 0 und 10 ueber Reihen 1..3 angebunden
    //
    // Start und Ziel liegen in Reihe 1 an den beiden Enden. Ohne Steigungskosten ist der Huegel
    // kuerzer; mit den Kosten des Cores (Strecke mal 1 + Grad / 100) ist der Umweg billiger.
    constexpr float HUMP = 8.0f;

    bool BuildHumpMesh(Mesh& m, bool withDetour)
    {
        Builder b;
        for (int c = 0; c < 11; ++c)
        {
            float const x0 = c * SQ, x1 = (c + 1) * SQ;
            float h0 = 0.0f, h1 = 0.0f;
            if (c == 4)      { h0 = 0.0f; h1 = HUMP; }
            else if (c == 5) { h0 = HUMP; h1 = HUMP; }
            else if (c == 6) { h0 = HUMP; h1 = 0.0f; }
            b.Quad(x0, 0.0f, x1, SQ, h0, h1);
            if (withDetour)
                b.Quad(x0, 4 * SQ, x1, 5 * SQ, 0.0f, 0.0f);
        }
        for (int r = 1; r < 4; ++r)
        {
            b.Quad(0.0f, r * SQ, SQ, (r + 1) * SQ, 0.0f, 0.0f);
            b.Quad(10 * SQ, r * SQ, 11 * SQ, (r + 1) * SQ, 0.0f, 0.0f);
        }
        return Finish(b, m);
    }

    // Ein einziges grosses Polygon (100 x 100 yd) als Innenraum und, 140 yd darueber, ein kleines
    // Polygon fuer das Ziel. Die beiden sind nicht verbunden: der Core liefert einen Teilweg, der im
    // grossen Polygon endet, und Detour setzt dessen letzten Punkt auf die Zielposition -- 70 yd
    // zur Seite und 139 yd hoch, kein senkrechter Sprung mehr.
    bool BuildBigRoomMesh(Mesh& m)
    {
        Builder b;
        b.Quad(0.0f, 0.0f, 100.0f, 100.0f, 0.0f, 0.0f);
        b.Quad(80.0f, 20.0f, 84.0f, 24.0f, TOP, TOP);
        return Finish(b, m);
    }

    // Start im Innenraum, Ziel auf der Oberflaeche senkrecht ueber ihm (Weltkoordinaten).
    constexpr float START[3]  = { 60.0f, 120.0f, 0.0f };
    constexpr float TARGET[3] = { 98.0f, 94.0f, TOP };

    float Dist2D(float ax, float ay, float bx, float by)
    {
        return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
    }

    float Length(std::vector<float> const& pts)
    {
        float len = 0.0f;
        for (size_t i = 3; i + 2 < pts.size(); i += 3)
        {
            float const dx = pts[i] - pts[i - 3], dy = pts[i + 1] - pts[i - 2], dz = pts[i + 2] - pts[i - 1];
            len += std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        return len;
    }

    // Grenzen fuer die Tests: ohne Zeitgrenze, damit ein langsamer Rechner nicht zu TimedOut fuehrt.
    AT::NavSearchLimits Limits()
    {
        AT::NavSearchLimits lim;
        lim.includeFlags = WALK;
        lim.maxMs = 0.0;
        return lim;
    }

    // Was der Core tut: dtNavMeshQuery mit 1024 Knoten, findPath mit hoechstens 74 Polygonen.
    void TestCoreLimitEndsUnderTarget(Mesh const& m)
    {
        dtNavMeshQuery* q = dtAllocNavMeshQuery();
        CHECK(q && dtStatusSucceed(q->init(m.nav, 1024)));

        dtQueryFilter filter;
        filter.setIncludeFlags(WALK);
        filter.setExcludeFlags(0);

        float const s[3] = { START[1], START[2], START[0] };     // Detour: (y, z, x)
        float const e[3] = { TARGET[1], TARGET[2], TARGET[0] };
        float const ext[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef sr = 0, er = 0;
        float near[3];
        q->findNearestPoly(s, ext, &filter, &sr, near);
        q->findNearestPoly(e, ext, &filter, &er, near);
        CHECK(sr != 0 && er != 0 && sr != er);

        dtPolyRef path[74];
        int n = 0;
        dtStatus const st = q->findPath(sr, er, s, e, &filter, path, &n, 74);

        CHECK(n > 0);
        CHECK((st & DT_PARTIAL_RESULT) != 0);            // das Ziel wird nicht erreicht
        CHECK((st & DT_OUT_OF_NODES) != 0);              // weil der Pool von 1024 Knoten leer ist
        CHECK(path[n - 1] != er);

        float closest[3];
        q->closestPointOnPoly(path[n - 1], e, closest, nullptr);
        // Ende des Teilwegs: im Innenraum (Hoehe 0), fast senkrecht unter dem Ziel.
        CHECK(std::fabs(closest[1]) < 1.0f);
        CHECK(Dist2D(closest[2], closest[0], TARGET[0], TARGET[1]) < 6.0f);

        dtFreeNavMeshQuery(q);
    }

    void TestSearchFindsTheWayOut(Mesh const& m)
    {
        AT::NavSearchLimits const lim = Limits();

        AT::NavSearchResult r = AT::NavSearch(m.nav, START[0], START[1], START[2],
                                              TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::Complete);
        CHECK(r.points.size() >= 6 && r.points.size() % 3 == 0);
        if (r.status != AT::NavSearchStatus::Complete || r.points.size() < 6)
            return;

        size_t const n = r.points.size();
        // Anfang und Ende
        CHECK(Dist2D(r.points[0], r.points[1], START[0], START[1]) < 0.5f);
        CHECK(std::fabs(r.points[2] - START[2]) < 0.5f);
        CHECK(Dist2D(r.points[n - 3], r.points[n - 2], TARGET[0], TARGET[1]) < 0.5f);
        CHECK(std::fabs(r.points[n - 1] - TARGET[2]) < 0.5f);

        // Der Weg fuehrt ueber die Rampe: er erreicht jede Zwischenhoehe weit ausserhalb des
        // Innenraums, und seine Hoehe wechselt nie schlagartig (hoechstens 3 yd je Punkt).
        float maxZ = -1.0e9f, maxX = -1.0e9f;
        float maxJump = 0.0f;
        bool onRamp = false;
        for (size_t i = 0; i + 2 < n; i += 3)
        {
            maxZ = std::fmax(maxZ, r.points[i + 2]);
            maxX = std::fmax(maxX, r.points[i]);
            if (r.points[i] > 220.0f && r.points[i + 2] > 20.0f && r.points[i + 2] < 120.0f)
                onRamp = true;
            if (i >= 3)
                maxJump = std::fmax(maxJump, std::fabs(r.points[i + 2] - r.points[i - 1]));
        }
        CHECK(maxZ > TOP - 1.0f);
        CHECK(maxX > m.xTop - 6.0f);
        CHECK(onRamp);
        CHECK(maxJump < 3.0f);

        // Laenge: durch den Innenraum zur Rampe (rund 170), die Rampe (rund 290), der Absatz und der Gang
        // zurueck (rund 480). Eine Luftlinie waere 150.
        float const len = Length(r.points);
        CHECK(len > 800.0f && len < 1100.0f);
        CHECK(r.polys > 200);
        CHECK(r.nodes > 1024);      // mehr Knoten, als der Core zur Verfuegung hat
    }

    void TestLimits(Mesh const& m)
    {
        AT::NavSearchLimits lim = Limits();

        // Der Pool des Cores reicht nicht.
        lim.maxNodes = 1024;
        AT::NavSearchResult r = AT::NavSearch(m.nav, START[0], START[1], START[2],
                                              TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::OutOfNodes);
        CHECK(r.points.empty());

        // Zeitgrenze: eine Scheibe dauert laenger als eine Nanosekunde.
        lim = Limits();
        lim.maxMs = 1.0e-6;
        r = AT::NavSearch(m.nav, START[0], START[1], START[2], TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::TimedOut);
        CHECK(r.points.empty());

        // Zu kurzer Polygonweg: der Weg braucht rund 250 Polygone.
        lim = Limits();
        lim.maxPolys = 100;
        r = AT::NavSearch(m.nav, START[0], START[1], START[2], TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::TooLong);
        CHECK(r.points.empty());

        // Zu wenige Eckpunkte.
        lim = Limits();
        lim.maxPoints = 20;
        r = AT::NavSearch(m.nav, START[0], START[1], START[2], TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::TooLong);
        CHECK(r.points.empty());
    }

    void TestEdgeCases(Mesh const& m)
    {
        AT::NavSearchLimits const lim = Limits();

        // Kein NavMesh.
        AT::NavSearchResult r = AT::NavSearch(nullptr, 0, 0, 0, 1, 1, 1, lim);
        CHECK(r.status == AT::NavSearchStatus::Failed);

        // Start oder Ziel ausserhalb des NavMesh.
        r = AT::NavSearch(m.nav, -500.0f, -500.0f, 0.0f, TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::NoStartPoly);
        r = AT::NavSearch(m.nav, START[0], START[1], START[2], 900.0f, 900.0f, 0.0f, lim);
        CHECK(r.status == AT::NavSearchStatus::NoEndPoly);

        // Anderer Filter: das NavMesh hat nur Flaechen mit Flag 0x01.
        AT::NavSearchLimits other = Limits();
        other.includeFlags = 0x02;
        r = AT::NavSearch(m.nav, START[0], START[1], START[2], TARGET[0], TARGET[1], TARGET[2], other);
        CHECK(r.status == AT::NavSearchStatus::NoStartPoly);

        // Start und Ziel im selben Polygon: ein Weg aus zwei Punkten.
        r = AT::NavSearch(m.nav, 60.0f, 60.0f, 0.0f, 61.0f, 61.5f, 0.0f, lim);
        CHECK(r.status == AT::NavSearchStatus::Complete);
        CHECK(r.points.size() == 6);

        // Ein Ziel auf der Insel: der Suchraum ist erschoepft, ohne dass der Pool leer wurde.
        r = AT::NavSearch(m.nav, START[0], START[1], START[2], 602.0f, 2.0f, 0.0f, lim);
        CHECK(r.status == AT::NavSearchStatus::Partial);
        CHECK(r.points.empty());
        CHECK(r.nodes > 2000);

        // Text fuer jeden Zustand vorhanden.
        CHECK(std::strlen(AT::NavSearchStatusText(AT::NavSearchStatus::Complete)) > 0);
        CHECK(std::strlen(AT::NavSearchStatusText(AT::NavSearchStatus::Partial)) > 0);
        CHECK(std::strlen(AT::NavSearchStatusText(AT::NavSearchStatus::OutOfNodes)) > 0);
        CHECK(std::strlen(AT::NavSearchStatusText(AT::NavSearchStatus::TimedOut)) > 0);
        CHECK(std::strlen(AT::NavSearchStatusText(AT::NavSearchStatus::TooLong)) > 0);
        CHECK(std::strlen(AT::NavSearchStatusText(AT::NavSearchStatus::NoStartPoly)) > 0);
        CHECK(std::strlen(AT::NavSearchStatusText(AT::NavSearchStatus::NoEndPoly)) > 0);
        CHECK(std::strlen(AT::NavSearchStatusText(AT::NavSearchStatus::Failed)) > 0);
    }

    // Eine kurze Strecke auf einer Etage: der erweiterte Suchweg ist dort genauso gut.
    void TestShortRouteSameFloor(Mesh const& m)
    {
        AT::NavSearchLimits const lim = Limits();
        AT::NavSearchResult r = AT::NavSearch(m.nav, 20.0f, 20.0f, 0.0f, 150.0f, 170.0f, 0.0f, lim);
        CHECK(r.status == AT::NavSearchStatus::Complete);
        if (r.status == AT::NavSearchStatus::Complete)
        {
            float const direct = std::sqrt(130.0f * 130.0f + 150.0f * 150.0f);
            float const len = Length(r.points);
            // Nahezu die Luftlinie. Der Weg bleibt im Polygonkorridor der Suche und ist deshalb
            // um ein, zwei Prozent laenger als die Gerade.
            CHECK(len >= direct - 0.5f && len < direct * 1.03f);
        }
    }

    // ---------------------------------------------------------------------------------------
    // Der Teilweg des Cores mit echter Detour-Ausgabe
    // ---------------------------------------------------------------------------------------

    struct Pt { float x, y, z; };

    struct QueryDeleter
    {
        void operator()(dtNavMeshQuery* q) const { dtFreeNavMeshQuery(q); }
    };

    struct CorePath
    {
        bool built = false;             // ein Weg mit mindestens zwei Punkten, nicht NOPATH / SHORT
        bool partial = false;           // findPath: DT_PARTIAL_RESULT
        bool outOfNodes = false;        // findPath: DT_OUT_OF_NODES
        bool reachesTarget = false;     // das letzte Polygon ist das Zielpolygon
        int polys = 0;
        std::vector<Pt> pts;            // Weltkoordinaten, wie der PathGenerator sie weitergibt
    };

    // ---- PathGenerator::FixupCorridor / GetSteerTarget / FindSmoothPath (AzerothCore), ohne
    // ---- Steigungspruefung und Off-Mesh-Verbindungen. Wortgetreu uebernommen, damit der Test den
    // ---- geglaetteten Weg zeigt, den der Core als ERSTEN Kandidaten liefert.

    uint32_t FixupCorridor(dtPolyRef* path, uint32_t npath, uint32_t maxPath, dtPolyRef const* visited,
                           uint32_t nvisited)
    {
        int32_t furthestPath = -1;
        int32_t furthestVisited = -1;

        for (int32_t i = int32_t(npath) - 1; i >= 0; --i)
        {
            bool found = false;
            for (int32_t j = int32_t(nvisited) - 1; j >= 0; --j)
            {
                if (path[i] == visited[j])
                {
                    furthestPath = i;
                    furthestVisited = j;
                    found = true;
                }
            }
            if (found)
                break;
        }

        if (furthestPath == -1 || furthestVisited == -1)
            return npath;

        uint32_t const req = nvisited - uint32_t(furthestVisited);
        uint32_t const orig = uint32_t(furthestPath + 1) < npath ? uint32_t(furthestPath + 1) : npath;
        uint32_t size = npath > orig ? npath - orig : 0;
        if (req + size > maxPath)
            size = maxPath - req;
        if (size)
            std::memmove(path + req, path + orig, size * sizeof(dtPolyRef));
        for (uint32_t i = 0; i < req; ++i)
            path[i] = visited[(nvisited - 1) - i];
        return req + size;
    }

    // Detour-Koordinaten (y, z, x): Index 1 ist die Hoehe.
    bool InRangeYZX(float const* v1, float const* v2, float r, float h)
    {
        float const dx = v2[0] - v1[0];
        float const dy = v2[1] - v1[1];
        float const dz = v2[2] - v1[2];
        return (dx * dx + dz * dz) < r * r && std::fabs(dy) < h;
    }

    bool GetSteerTarget(dtNavMeshQuery const* q, float const* startPos, float const* endPos, float minTargetDist,
                        dtPolyRef const* path, uint32_t pathSize, float* steerPos, unsigned char& steerPosFlag)
    {
        constexpr uint32_t MAX_STEER_POINTS = 3;
        float steerPath[MAX_STEER_POINTS * 3];
        unsigned char steerPathFlags[MAX_STEER_POINTS];
        dtPolyRef steerPathPolys[MAX_STEER_POINTS];
        int nsteerPath = 0;
        dtStatus const st = q->findStraightPath(startPos, endPos, path, int(pathSize), steerPath, steerPathFlags,
                                                steerPathPolys, &nsteerPath, int(MAX_STEER_POINTS));
        if (!nsteerPath || dtStatusFailed(st))
            return false;

        int ns = 0;
        while (ns < nsteerPath)
        {
            if ((steerPathFlags[ns] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) ||
                !InRangeYZX(&steerPath[ns * 3], startPos, minTargetDist, 1000.0f))
                break;
            ++ns;
        }
        if (ns >= nsteerPath)
            return false;

        std::memcpy(steerPos, &steerPath[ns * 3], 3 * sizeof(float));
        steerPos[1] = startPos[1];      // keep Z value
        steerPosFlag = steerPathFlags[ns];
        return true;
    }

    dtStatus FindSmoothPath(dtNavMeshQuery const* q, dtQueryFilter const* filter,
                            float const* startPos, float const* endPos,
                            dtPolyRef const* polyPath, uint32_t polyPathSize,
                            float* smoothPath, int* smoothPathSize, uint32_t maxSmoothPathSize)
    {
        constexpr float STEP = 4.0f;        // SMOOTH_PATH_STEP_SIZE
        constexpr float SLOP = 0.3f;        // SMOOTH_PATH_SLOP
        constexpr uint32_t MAX_PATH = 74;

        *smoothPathSize = 0;
        uint32_t nsmoothPath = 0;

        dtPolyRef polys[MAX_PATH];
        std::memcpy(polys, polyPath, sizeof(dtPolyRef) * polyPathSize);
        uint32_t npolys = polyPathSize;

        float iterPos[3], targetPos[3];
        if (polyPathSize > 1)
        {
            if (dtStatusFailed(q->closestPointOnPolyBoundary(polys[0], startPos, iterPos)))
                return DT_FAILURE;
            if (dtStatusFailed(q->closestPointOnPolyBoundary(polys[npolys - 1], endPos, targetPos)))
                return DT_FAILURE;
        }
        else
        {
            std::memcpy(iterPos, startPos, sizeof(iterPos));
            std::memcpy(targetPos, endPos, sizeof(targetPos));
        }

        std::memcpy(&smoothPath[nsmoothPath * 3], iterPos, sizeof(iterPos));
        ++nsmoothPath;

        while (npolys && nsmoothPath < maxSmoothPathSize)
        {
            float steerPos[3];
            unsigned char steerPosFlag = 0;
            if (!GetSteerTarget(q, iterPos, targetPos, SLOP, polys, npolys, steerPos, steerPosFlag))
                break;

            bool const endOfPath = (steerPosFlag & DT_STRAIGHTPATH_END) != 0;

            float delta[3];
            dtVsub(delta, steerPos, iterPos);
            float len = std::sqrt(dtVdot(delta, delta));
            if (endOfPath && len < STEP)
                len = 1.0f;
            else
                len = STEP / len;

            float moveTgt[3];
            dtVmad(moveTgt, iterPos, delta, len);

            float result[3];
            constexpr int MAX_VISIT_POLY = 16;
            dtPolyRef visited[MAX_VISIT_POLY];
            int nvisited = 0;
            if (dtStatusFailed(q->moveAlongSurface(polys[0], iterPos, moveTgt, filter, result, visited,
                                                   &nvisited, MAX_VISIT_POLY)))
                return DT_FAILURE;
            npolys = FixupCorridor(polys, npolys, MAX_PATH, visited, uint32_t(nvisited));

            q->getPolyHeight(polys[0], result, &result[1]);
            result[1] += 0.5f;
            std::memcpy(iterPos, result, sizeof(iterPos));

            if (endOfPath && InRangeYZX(iterPos, steerPos, SLOP, 1.0f))
            {
                std::memcpy(iterPos, targetPos, sizeof(iterPos));
                if (nsmoothPath < maxSmoothPathSize)
                {
                    std::memcpy(&smoothPath[nsmoothPath * 3], iterPos, sizeof(iterPos));
                    ++nsmoothPath;
                }
                break;
            }

            if (nsmoothPath < maxSmoothPathSize)
            {
                std::memcpy(&smoothPath[nsmoothPath * 3], iterPos, sizeof(iterPos));
                ++nsmoothPath;
            }
        }

        *smoothPathSize = int(nsmoothPath);
        return nsmoothPath < 74 ? DT_SUCCESS : DT_FAILURE;      // "this is most likely a loop"
    }

    // PathGenerator::BuildPolyPath / BuildPointPath ohne den Core: findPath mit Knotenpool 1024 und
    // hoechstens 74 Polygonen, dann je nach 'smooth' FindSmoothPath (der erste Kandidat des Moduls)
    // oder findStraightPath (der zweite), mit den Sonderfaellen von BuildPointPath.
    CorePath CoreBuild(Mesh const& m, float const* S, float const* T, bool smooth)
    {
        CorePath out;
        std::unique_ptr<dtNavMeshQuery, QueryDeleter> q(dtAllocNavMeshQuery());
        if (!q || dtStatusFailed(q->init(m.nav, 1024)))
            return out;

        dtQueryFilter filter;
        filter.setIncludeFlags(WALK);
        filter.setExcludeFlags(0);

        float const s[3] = { S[1], S[2], S[0] };     // Detour: (y, z, x)
        float const e[3] = { T[1], T[2], T[0] };
        float const ext[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef sr = 0, er = 0;
        float near[3];
        q->findNearestPoly(s, ext, &filter, &sr, near);
        q->findNearestPoly(e, ext, &filter, &er, near);
        if (!sr || !er)
            return out;

        dtPolyRef path[74];
        int n = 0;
        dtStatus const st = q->findPath(sr, er, s, e, &filter, path, &n, 74);
        if (dtStatusFailed(st) || n <= 0)
            return out;

        float pts[74 * 3];
        int pc = 0;
        dtStatus rs;
        if (smooth)
            rs = FindSmoothPath(q.get(), &filter, s, e, path, uint32_t(n), pts, &pc, 74);
        else
            rs = q->findStraightPath(s, e, path, n, pts, nullptr, nullptr, &pc, 74);

        out.partial = (st & DT_PARTIAL_RESULT) != 0;
        out.outOfNodes = (st & DT_OUT_OF_NODES) != 0;
        out.reachesTarget = path[n - 1] == er;
        out.polys = n;

        if (n == 1 && pc == 1)
        {
            // Sonderfall: ein Polygon, ein Punkt: das Ziel wird angehaengt.
            pts[3] = e[0]; pts[4] = e[1]; pts[5] = e[2];
            pc = 2;
        }
        else if (pc < 2 || dtStatusFailed(rs))
            return out;                 // PATHFIND_NOPATH
        else if (pc >= 74)
            return out;                 // PATHFIND_SHORT

        out.built = true;
        for (int i = 0; i < pc; ++i)
            out.pts.push_back({ pts[i * 3 + 2], pts[i * 3], pts[i * 3 + 1] });     // -> Welt (x, y, z)
        return out;
    }

    struct Variant
    {
        char const* label;
        float s[3];
        float t[3];
    };

    // Prueft einen Teilweg des Cores gegen die Erkennung und gegen die erweiterte Suche, so wie
    // TryPathBetween beide benutzt. 'findable': die erweiterte Suche findet einen durchgehenden Weg.
    // Rueckgabe: ob der letzte Punkt des Cores auf die Zielposition festgenagelt war.
    bool CheckCorePartial(Mesh const& m, char const* label, float const* S, float const* T, bool smooth,
                          bool findable)
    {
        bool const verbose = std::getenv("SEARCH_TEST_VERBOSE") != nullptr;

        CorePath const c = CoreBuild(m, S, T, smooth);
        CHECK(c.built);
        // Der Core liefert hier einen Teilweg: das Zielpolygon wird nicht erreicht.
        CHECK(c.partial);
        CHECK(!c.reachesTarget);
        if (!c.built)
            return false;

        Pt const last = c.pts.back();
        bool const pinned = Dist2D(last.x, last.y, T[0], T[1]) < 0.01f && std::fabs(last.z - T[2]) < 0.01f;

        std::vector<Pt> trimmed = c.pts;
        size_t const cut = AT::TrimDeadTail(trimmed, T[0], T[1], T[2]);
        CHECK(!trimmed.empty());
        Pt const end = trimmed.back();

        // Ein festgenagelter letzter Punkt wird abgeschnitten, ein anderer bleibt.
        CHECK(cut == (pinned ? 1u : 0u));

        // Ohne das Abschneiden sieht ein festgenagelter Teilweg aus, als kaeme er an: der Fehler.
        bool const stuckRaw = AT::PartialLooksStuck(S[0], S[1], S[2], T[0], T[1], T[2], last.x, last.y, last.z);
        if (pinned)
            CHECK(!stuckRaw);

        // Danach schlaegt die Erkennung an (TryPathBetween: suspicious, stuckEnd): die erweiterte Suche
        // laeuft, und endet sie ohne Weg, erfaehrt der Spieler, dass das Ziel auf einer anderen Etage liegt.
        bool const stuck = AT::PartialLooksStuck(S[0], S[1], S[2], T[0], T[1], T[2], end.x, end.y, end.z);
        bool const under = AT::PartialEndsUnderTarget(S[0], S[1], S[2], T[0], T[1], T[2], end.x, end.y, end.z);
        CHECK(cut == 1 || stuck);
        CHECK(cut == 1 || under);
        // Das Ende liegt auf der unteren Etage, nicht beim Ziel.
        CHECK(std::fabs(end.z - T[2]) > 100.0f);

        if (verbose)
            std::printf("  [%s %s] polys=%d pts=%zu pinned=%d last=(%.1f %.1f %.1f) cut=%zu end=(%.1f %.1f %.1f) "
                        "stuck=%d under=%d\n",
                        smooth ? "glatt" : "Ecken", label, c.polys, c.pts.size(), (int)pinned, last.x, last.y, last.z, cut,
                        end.x, end.y, end.z, (int)stuck, (int)under);

        // Die erweiterte Suche kennt den Weg: ueber die Rampe auf die Oberflaeche.
        AT::NavSearchResult const r = AT::NavSearch(m.nav, S[0], S[1], S[2], T[0], T[1], T[2], Limits());
        if (findable)
        {
            CHECK(r.status == AT::NavSearchStatus::Complete);
            if (r.status == AT::NavSearchStatus::Complete && r.points.size() >= 6)
            {
                size_t const n = r.points.size();
                CHECK(Dist2D(r.points[n - 3], r.points[n - 2], T[0], T[1]) < 0.5f);
                CHECK(std::fabs(r.points[n - 1] - T[2]) < 0.5f);
            }
        }
        else
        {
            CHECK(r.status == AT::NavSearchStatus::Partial);
            CHECK(r.points.empty());
        }
        return pinned;
    }

    // Detour setzt den letzten Punkt eines Teilwegs auf die ZIELPOSITION, wenn x/y des Ziels im
    // letzten Polygon liegen (closestPointOnPolyBoundary gibt dann die Position unveraendert zurueck).
    // Der Teilweg "endet" damit 139 yd ueber dem Boden, und der Spline faehrt das senkrechte letzte
    // Stueck ab. Mit echter Detour-Ausgabe auf dem Modell, fuer Ziele und Spieler an vielen Stellen.
    void TestPinnedVertex(Mesh const& m)
    {
        struct Variant { char const* label; float s[3]; float t[3]; };
        Variant const variants[] = {
            // Spieler im Innenraum, Ziel auf der Oberflaeche an verschiedenen Stellen darueber
            { "Ziel ueber dem Innenraum (98,94)",    { 60.0f, 120.0f, 0.0f }, { 98.0f, 94.0f, TOP } },
            { "Ziel (62,94)",                        { 60.0f, 120.0f, 0.0f }, { 62.0f, 94.0f, TOP } },
            { "Ziel (80,94)",                        { 60.0f, 120.0f, 0.0f }, { 80.0f, 94.0f, TOP } },
            { "Ziel (130,94)",                       { 60.0f, 120.0f, 0.0f }, { 130.0f, 94.0f, TOP } },
            { "Ziel (181,94)",                       { 60.0f, 120.0f, 0.0f }, { 181.0f, 94.0f, TOP } },
            { "Ziel (189,94)",                       { 60.0f, 120.0f, 0.0f }, { 189.0f, 94.0f, TOP } },
            { "Ziel hinter dem Rand (196,94)",       { 60.0f, 120.0f, 0.0f }, { 196.0f, 94.0f, TOP } },
            // Spieler schon an der Sackgasse
            { "Spieler direkt unter dem Ziel",       { 98.0f, 92.0f, 0.0f },  { 98.0f, 94.0f, TOP } },
            { "Spieler 2 yd daneben",                { 100.0f, 94.5f, 0.0f }, { 98.0f, 94.0f, TOP } },
            { "Spieler 8 yd daneben",                { 90.0f, 94.0f, 0.0f },  { 98.0f, 94.0f, TOP } },
            { "Spieler 28 yd daneben",               { 70.0f, 94.0f, 0.0f },  { 98.0f, 94.0f, TOP } },
        };

        // Beide Fassungen, die das Modul fuer jeden Kandidaten rechnet: geglaettet (FindSmoothPath) und
        // Eckpunkte (findStraightPath).
        int pinnedCount[2] = { 0, 0 };
        for (int pass = 0; pass < 2; ++pass)
            for (Variant const& v : variants)
                if (CheckCorePartial(m, v.label, v.s, v.t, pass == 0, true))
                    ++pinnedCount[pass];
        // Das Modell muss den Fall wirklich enthalten: ohne festgenagelte Punkte prueft der Test nichts.
        CHECK(pinnedCount[0] >= 5);
        CHECK(pinnedCount[1] >= 5);

        // Ein grosses Polygon: das Stueck zum festgenagelten Ziel ist eine lange Schraege, die
        // IsVerticalJump durchlaesst (73 yd zur Seite, 140 hoch).
        Mesh room;
        CHECK(BuildBigRoomMesh(room));
        if (room.nav)
        {
            float const S[3] = { 10.0f, 10.0f, 0.0f }, T[3] = { 82.0f, 22.0f, TOP };
            CorePath const c = CoreBuild(room, S, T, false);
            CHECK(c.built && c.pts.size() == 2);
            if (c.built && c.pts.size() == 2)
            {
                CHECK(!AT::IsVerticalJump(c.pts[0].x, c.pts[0].y, c.pts[0].z, c.pts[1].x, c.pts[1].y, c.pts[1].z));
                CHECK(Dist2D(c.pts[1].x, c.pts[1].y, T[0], T[1]) < 0.01f && std::fabs(c.pts[1].z - T[2]) < 0.01f);
                std::vector<Pt> p = c.pts;
                CHECK(AT::TrimDeadTail(p, T[0], T[1], T[2]) == 1);
                CHECK(p.size() == 1);
            }
            // Eine Verbindung gibt es nicht: die Suche meldet es, statt einen Weg zu erfinden.
            AT::NavSearchResult const r = AT::NavSearch(room.nav, S[0], S[1], S[2], T[0], T[1], T[2], Limits());
            CHECK(r.status == AT::NavSearchStatus::Partial);
            CHECK(r.points.empty());
        }
    }

    // Hoechste Hoehe auf dem Weg, den ein PLAIN dtQueryFilter (nur Strecke) findet.
    float PlainMaxZ(Mesh const& m, float const* S, float const* T)
    {
        std::unique_ptr<dtNavMeshQuery, QueryDeleter> q(dtAllocNavMeshQuery());
        if (!q || dtStatusFailed(q->init(m.nav, 2048)))
            return -1.0e9f;
        dtQueryFilter filter;
        filter.setIncludeFlags(WALK);
        filter.setExcludeFlags(0);
        float const s[3] = { S[1], S[2], S[0] }, e[3] = { T[1], T[2], T[0] }, ext[3] = { 3.0f, 5.0f, 3.0f };
        dtPolyRef sr = 0, er = 0;
        float near[3];
        q->findNearestPoly(s, ext, &filter, &sr, near);
        q->findNearestPoly(e, ext, &filter, &er, near);
        dtPolyRef path[256];
        int n = 0;
        if (dtStatusFailed(q->findPath(sr, er, s, e, &filter, path, &n, 256)) || n <= 0)
            return -1.0e9f;
        float pts[256 * 3];
        int pc = 0;
        q->findStraightPath(s, e, path, n, pts, nullptr, nullptr, &pc, 256, DT_STRAIGHTPATH_ALL_CROSSINGS);
        float maxZ = -1.0e9f;
        for (int i = 0; i < pc; ++i)
            maxZ = std::fmax(maxZ, pts[i * 3 + 1]);
        return maxZ;
    }

    float MaxZ(std::vector<float> const& pts)
    {
        float maxZ = -1.0e9f;
        for (size_t i = 2; i < pts.size(); i += 3)
            maxZ = std::fmax(maxZ, pts[i]);
        return maxZ;
    }

    // Die erweiterte Suche waehlt wie der Core: ein steiler Huegel kostet mehr als seine Laenge.
    void TestSlopeCost()
    {
        float const S[3] = { 2.0f, 6.0f, 0.0f }, T[3] = { 42.0f, 6.0f, 0.0f };
        AT::NavSearchLimits const lim = Limits();

        Mesh both;
        CHECK(BuildHumpMesh(both, true));
        if (both.nav)
        {
            // Mit der reinen Strecke als Kosten ist der Huegel der kuerzere Weg ...
            CHECK(PlainMaxZ(both, S, T) > HUMP - 0.5f);

            // ... die Suche des Moduls rechnet wie der Core und nimmt den flachen Umweg.
            AT::NavSearchResult const r = AT::NavSearch(both.nav, S[0], S[1], S[2], T[0], T[1], T[2], lim);
            CHECK(r.status == AT::NavSearchStatus::Complete);
            CHECK(r.points.size() >= 6);
            CHECK(MaxZ(r.points) < 0.5f);
            // Der Umweg fuehrt bis Reihe 4 (y 16..20); der gespannte Weg streift ihre Innenkante.
            float maxY = -1.0e9f;
            for (size_t i = 1; i < r.points.size(); i += 3)
                maxY = std::fmax(maxY, r.points[i]);
            CHECK(maxY > 15.0f);
        }

        // Gibt es nur den Huegel, geht die Suche darueber: die Steigung kostet, sie sperrt nichts.
        Mesh humpOnly;
        CHECK(BuildHumpMesh(humpOnly, false));
        if (humpOnly.nav)
        {
            AT::NavSearchResult const r = AT::NavSearch(humpOnly.nav, S[0], S[1], S[2], T[0], T[1], T[2], lim);
            CHECK(r.status == AT::NavSearchStatus::Complete);
            CHECK(MaxZ(r.points) > HUMP - 0.5f);
        }
    }
}

int main()
{
    Mesh m;
    if (!BuildMesh(m, true))
    {
        std::printf("FAIL NavMesh konnte nicht gebaut werden\n");
        return 1;
    }
    CHECK(m.polyCount > 2400);

    TestCoreLimitEndsUnderTarget(m);
    TestSearchFindsTheWayOut(m);
    TestLimits(m);
    TestEdgeCases(m);
    TestShortRouteSameFloor(m);
    TestPinnedVertex(m);
    TestSlopeCost();

    std::printf("%d Pruefungen, %d Fehler\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
