/*
 * AutoTravel_Search.cpp
 * ---------------------------------------------------------------------------
 * Siehe AutoTravel_Search.h. Alles hier ist reines Detour; Koordinaten kommen und
 * gehen als Weltkoordinaten.
 *
 * Aufbau wie PathGenerator::BuildPolyPath, mit drei Unterschieden:
 *
 *   * eigener Knotenpool (der des Cores hat 1024 Knoten),
 *   * die Suche laeuft in Scheiben ("sliced") und bricht an der Zeitgrenze ab, damit
 *     sie den Weltserver-Thread nie laenger als vorgesehen belegt,
 *   * ein Teilergebnis wird NICHT als Weg ausgegeben. Der Core liefert bei erschoepfter
 *     Suche den Knoten mit der kleinsten Luftlinie zum Ziel -- fuer ein Ziel auf einer
 *     anderen Etage ist das die Sackgasse unter oder ueber dem Ziel, nicht der Weg.
 * ---------------------------------------------------------------------------
 */

#include "AutoTravel_Search.h"

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

namespace AT
{
    namespace
    {
        struct QueryDeleter
        {
            void operator()(dtNavMeshQuery* q) const { dtFreeNavMeshQuery(q); }
        };

        // Dieselben Kosten wie dtQueryFilterExt, den der PathGenerator des Cores benutzt
        // (src/common/Navigation/DetourExtended.cpp): Strecke mal (1 + Steigung in Grad / 100) mal
        // Flaechenkosten. Ohne die Steigung suchte die erweiterte Suche kuerzere, aber steilere Wege
        // als der Core und ersetzte seinen Teilweg durch einen, den er nicht gewaehlt haette.
        class SlopeFilter : public dtQueryFilter
        {
        public:
            float getCost(float const* pa, float const* pb,
                          dtPolyRef const, dtMeshTile const*, dtPoly const*,
                          dtPolyRef const, dtMeshTile const*, dtPoly const* curPoly,
                          dtPolyRef const, dtMeshTile const*, dtPoly const*) const override
            {
                // Detour-Koordinaten (y, z, x): Hoehe ist Index 1.
                float const floorDist = std::sqrt((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[2] - pb[2]) * (pa[2] - pb[2]));
                float const rise = std::fabs(pb[1] - pa[1]);
                float const degrees = std::atan(rise / std::fabs(floorDist)) * 180.0f / 3.14159265358979f;
                float const cost = degrees > 0.0f ? 1.0f + degrees / 100.0f : 1.0f;
                return dtVdist(pa, pb) * cost * getAreaCost(curPoly->getArea());
            }
        };

        // Wie PathGenerator::GetPolyByLocation: erst eine flache, dann eine hohe Suchbox.
        // Die hohe Box faengt Punkte ab, die ueber oder unter der Flaeche liegen. 'dist' ist der
        // Abstand des Punkts zum gefundenen Polygon.
        bool PickPoly(dtNavMeshQuery const& q, dtQueryFilter const& filter, float const* p,
                      dtPolyRef& ref, float& dist)
        {
            float extents[3] = { 3.0f, 5.0f, 3.0f };
            float nearest[3] = { 0.0f, 0.0f, 0.0f };

            ref = 0;
            bool found = dtStatusSucceed(q.findNearestPoly(p, extents, &filter, &ref, nearest)) && ref;
            if (!found)
            {
                extents[1] = 50.0f;
                ref = 0;
                found = dtStatusSucceed(q.findNearestPoly(p, extents, &filter, &ref, nearest)) && ref;
            }
            if (found)
                dist = dtVdist(p, nearest);
            return found;
        }
    }

    char const* NavSearchStatusText(NavSearchStatus status)
    {
        switch (status)
        {
            case NavSearchStatus::Complete:    return "vollstaendig";
            case NavSearchStatus::Partial:     return "keine Verbindung im NavMesh";
            case NavSearchStatus::OutOfNodes:  return "Suchpool erschoepft";
            case NavSearchStatus::TimedOut:    return "Zeitgrenze erreicht";
            case NavSearchStatus::TooLong:     return "Weg zu lang";
            case NavSearchStatus::NoStartPoly: return "am Start liegt kein NavMesh";
            case NavSearchStatus::NoEndPoly:   return "am Ziel liegt kein NavMesh";
            case NavSearchStatus::Failed:      break;
        }
        return "Fehler";
    }

    NavSearchResult NavSearch(dtNavMesh const* nav,
                              float sx, float sy, float sz,
                              float dx, float dy, float dz,
                              NavSearchLimits const& limits)
    {
        NavSearchResult res;
        if (!nav)
            return res;

        auto const t0 = std::chrono::steady_clock::now();
        auto elapsedMs = [&]()
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        };
        auto finish = [&](NavSearchStatus status)
        {
            res.status = status;
            res.ms = elapsedMs();
            return res;
        };

        // Welt (x, y, z) -> Detour (y, z, x).
        float const start[3] = { sy, sz, sx };
        float const end[3]   = { dy, dz, dx };

        std::unique_ptr<dtNavMeshQuery, QueryDeleter> query(dtAllocNavMeshQuery());
        if (!query)
            return finish(NavSearchStatus::Failed);

        int const maxNodes = std::max(64, std::min(limits.maxNodes, 65535));
        if (dtStatusFailed(query->init(nav, maxNodes)))
            return finish(NavSearchStatus::Failed);

        SlopeFilter filter;
        filter.setIncludeFlags(limits.includeFlags);
        filter.setExcludeFlags(limits.excludeFlags);

        dtPolyRef startRef = 0, endRef = 0;
        if (!PickPoly(*query, filter, start, startRef, res.startDist))
            return finish(NavSearchStatus::NoStartPoly);
        if (!PickPoly(*query, filter, end, endRef, res.endDist))
            return finish(NavSearchStatus::NoEndPoly);

        int const maxPolys = std::max(16, limits.maxPolys);
        std::vector<dtPolyRef> polys(size_t(maxPolys), 0);
        int polyCount = 0;

        if (startRef == endRef)
        {
            polys[0] = startRef;
            polyCount = 1;
        }
        else
        {
            if (dtStatusFailed(query->initSlicedFindPath(startRef, endRef, start, end, &filter, 0)))
                return finish(NavSearchStatus::Failed);

            // Scheibenweise suchen: die Zeitgrenze wird nach jeder Scheibe geprueft.
            constexpr int SLICE = 512;
            dtStatus st = DT_IN_PROGRESS;
            int iterations = 0;
            for (;;)
            {
                int done = 0;
                st = query->updateSlicedFindPath(SLICE, &done);
                iterations += done;
                if (!dtStatusInProgress(st))
                    break;
                if (limits.maxMs > 0.0 && elapsedMs() > limits.maxMs)
                {
                    res.nodes = iterations;
                    return finish(NavSearchStatus::TimedOut);
                }
            }
            res.nodes = iterations;

            if (dtStatusFailed(st))
                return finish(NavSearchStatus::Failed);

            bool const outOfNodes = (st & DT_OUT_OF_NODES) != 0;

            dtStatus const fs = query->finalizeSlicedFindPath(polys.data(), &polyCount, maxPolys);
            if (dtStatusFailed(fs) || polyCount <= 0)
                return finish(NavSearchStatus::Failed);

            res.polys = polyCount;

            if (polys[size_t(polyCount - 1)] != endRef || (fs & DT_PARTIAL_RESULT))
            {
                // Ohne Ziel: war der Pool zu klein, ist das Ergebnis nicht aussagekraeftig.
                return finish(outOfNodes ? NavSearchStatus::OutOfNodes
                              : ((fs & DT_BUFFER_TOO_SMALL) ? NavSearchStatus::TooLong
                                                            : NavSearchStatus::Partial));
            }
        }

        res.polys = polyCount;

        int const maxPoints = std::max(16, limits.maxPoints);
        std::vector<float> corners(size_t(maxPoints) * 3, 0.0f);
        int cornerCount = 0;

        // ALL_CROSSINGS: ein Punkt an jeder Polygonkante. Auf Rampen und Treppen aus
        // vielen Polygonen bleibt so das Hoehenprofil erhalten; nur die Ecken wuerden
        // lange Hanglinien ohne Zwischenhoehe liefern.
        dtStatus const ss = query->findStraightPath(start, end, polys.data(), polyCount,
                                                    corners.data(), nullptr, nullptr, &cornerCount, maxPoints,
                                                    DT_STRAIGHTPATH_ALL_CROSSINGS);
        if (dtStatusFailed(ss) || cornerCount < 2)
            return finish(NavSearchStatus::Failed);
        if (ss & DT_BUFFER_TOO_SMALL)
            return finish(NavSearchStatus::TooLong);

        // Detour (y, z, x) -> Welt (x, y, z).
        res.points.reserve(size_t(cornerCount) * 3);
        for (int i = 0; i < cornerCount; ++i)
        {
            float const* c = &corners[size_t(i) * 3];
            res.points.push_back(c[2]);
            res.points.push_back(c[0]);
            res.points.push_back(c[1]);
        }

        return finish(NavSearchStatus::Complete);
    }
}
