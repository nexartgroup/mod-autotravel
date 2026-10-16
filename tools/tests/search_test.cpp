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

#include "AutoTravel_Search.h"

#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourNode.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
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
        AT::NavSearchLimits lim;
        lim.includeFlags = WALK;

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
        AT::NavSearchLimits lim;
        lim.includeFlags = WALK;

        // Der Pool des Cores reicht nicht.
        lim.maxNodes = 1024;
        AT::NavSearchResult r = AT::NavSearch(m.nav, START[0], START[1], START[2],
                                              TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::OutOfNodes);
        CHECK(r.points.empty());

        // Zeitgrenze: eine Scheibe dauert laenger als eine Nanosekunde.
        lim = AT::NavSearchLimits();
        lim.includeFlags = WALK;
        lim.maxMs = 1.0e-6;
        r = AT::NavSearch(m.nav, START[0], START[1], START[2], TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::TimedOut);
        CHECK(r.points.empty());

        // Zu kurzer Polygonweg: der Weg braucht rund 250 Polygone.
        lim = AT::NavSearchLimits();
        lim.includeFlags = WALK;
        lim.maxPolys = 100;
        r = AT::NavSearch(m.nav, START[0], START[1], START[2], TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::TooLong);
        CHECK(r.points.empty());

        // Zu wenige Eckpunkte.
        lim = AT::NavSearchLimits();
        lim.includeFlags = WALK;
        lim.maxPoints = 20;
        r = AT::NavSearch(m.nav, START[0], START[1], START[2], TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::TooLong);
        CHECK(r.points.empty());
    }

    void TestEdgeCases(Mesh const& m)
    {
        AT::NavSearchLimits lim;
        lim.includeFlags = WALK;

        // Kein NavMesh.
        AT::NavSearchResult r = AT::NavSearch(nullptr, 0, 0, 0, 1, 1, 1, lim);
        CHECK(r.status == AT::NavSearchStatus::Failed);

        // Start oder Ziel ausserhalb des NavMesh.
        r = AT::NavSearch(m.nav, -500.0f, -500.0f, 0.0f, TARGET[0], TARGET[1], TARGET[2], lim);
        CHECK(r.status == AT::NavSearchStatus::NoStartPoly);
        r = AT::NavSearch(m.nav, START[0], START[1], START[2], 900.0f, 900.0f, 0.0f, lim);
        CHECK(r.status == AT::NavSearchStatus::NoEndPoly);

        // Anderer Filter: das NavMesh hat nur Flaechen mit Flag 0x01.
        AT::NavSearchLimits other;
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
        AT::NavSearchLimits lim;
        lim.includeFlags = WALK;
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

    std::printf("%d Pruefungen, %d Fehler\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
