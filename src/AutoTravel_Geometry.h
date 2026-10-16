/*
 * AutoTravel_Geometry.h
 * ---------------------------------------------------------------------------
 * Reine Geometrie fuer Teilwege des PathGenerators. Header-only und ohne Core, damit
 * die Tests (tools/tests/util_test.cpp, search_test.cpp) sie mit echter Detour-Ausgabe
 * pruefen koennen.
 *
 * Hintergrund. Liefert der PathGenerator nur einen Teilweg, ist sein Ende der Knoten mit
 * der kleinsten LUFTLINIE zum Ziel. Liegt das Ziel eine Etage hoeher oder tiefer, ist das
 * die Stelle direkt darunter oder darueber -- eine Wand. Zwei Eigenheiten von Detour
 * machen das schwer zu erkennen:
 *
 *   * dtNavMeshQuery::closestPointOnPolyBoundary gibt die Position UNVERAENDERT zurueck,
 *     auch mit ihrer Hoehe, wenn x/y im Polygon liegen. Liegt unter dem NPC ein Polygon
 *     des Innenraums, fuehrt der Teilweg deshalb scheinbar bis zum NPC: sein letzter Punkt
 *     ist die Zielposition, 139 yd ueber dem Boden, und das letzte Wegstueck steigt
 *     senkrecht dorthin. Der Spline faehrt es ab -- der Charakter wird durch die Decke an
 *     die Oberflaeche versetzt.
 *   * Der letzte Punkt sagt dann nichts ueber das Ende des begehbaren Wegs. Das Stueck zur
 *     Zielposition muss zuerst abgeschnitten werden (TrimDeadTail). Bei einem grossen Polygon
 *     ist es nicht senkrecht, sondern eine lange Schraege: 70 yd zur Seite, 139 yd hoch.
 * ---------------------------------------------------------------------------
 */

#ifndef MOD_AUTOTRAVEL_GEOMETRY_H
#define MOD_AUTOTRAVEL_GEOMETRY_H

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace AT
{
    namespace geometry
    {
        inline float Hypot2(float a, float b) { return std::sqrt(a * a + b * b); }

        struct PartialMeasure
        {
            float start2 = 0.0f, end2 = 0.0f;   // Abstand zum Ziel in der Ebene (Start / Ende des Teilwegs)
            float start3 = 0.0f, end3 = 0.0f;   // ... in 3D
            float dzEnd = 0.0f;                 // Hoehenunterschied zwischen Ende und Ziel
            bool  valid = false;
        };

        inline PartialMeasure Measure(float sx, float sy, float sz, float dx, float dy, float dz,
                                      float ex, float ey, float ez)
        {
            PartialMeasure m;
            m.valid = std::isfinite(sx + sy + sz + dx + dy + dz + ex + ey + ez);
            if (!m.valid)
                return m;
            m.start2 = Hypot2(sx - dx, sy - dy);
            m.end2   = Hypot2(ex - dx, ey - dy);
            m.start3 = Hypot2(m.start2, sz - dz);
            m.end3   = Hypot2(m.end2, ez - dz);
            m.dzEnd  = std::fabs(ez - dz);
            return m;
        }

        // Endet der Teilweg praktisch am Ziel, fehlt nur der letzte Schritt.
        constexpr float AT_TARGET = 12.0f;

        // Ein Wegstueck, das mehr als so viel steigt oder faellt und dabei weniger als
        // 1/VERTICAL_RATIO so weit nach der Seite geht, kann niemand ablaufen.
        constexpr float VERTICAL_MIN   = 12.0f;
        constexpr float VERTICAL_RATIO = 2.5f;

        // Ein Wegstueck, das AUF DER ZIELPOSITION endet (so weit daneben darf der letzte Punkt liegen,
        // nachdem der Core ihn auf die Bodenhoehe gezogen hat), mindestens 20 yd steigt oder faellt
        // und dabei nicht flacher als 1:4 ist.
        constexpr float PIN_XY        = 0.5f;
        constexpr float PIN_Z         = 3.0f;
        constexpr float PIN_MIN_RISE  = 20.0f;
        constexpr float PIN_MIN_SLOPE = 0.25f;
    }

    // Ein Wegstueck von a nach b, das senkrecht steigt oder faellt (Treppen und Rampen sind
    // weit flacher: Steigung 1,2 bis 2).
    inline bool IsVerticalJump(float ax, float ay, float az, float bx, float by, float bz)
    {
        float const horizontal = geometry::Hypot2(bx - ax, by - ay);
        float const dz = std::fabs(bz - az);
        return dz >= geometry::VERTICAL_MIN && dz > geometry::VERTICAL_RATIO * horizontal;
    }

    // Ein Wegstueck von a nach b, das auf der Zielposition (tx, ty, tz) endet und dabei steil
    // genug um mindestens 20 yd steigt oder faellt. Bei einem Teilweg ist das nie das Ende eines
    // begehbaren Wegs: Detour hat den letzten Punkt auf die Zielposition gesetzt (siehe oben),
    // egal wie weit sie ueber oder unter der Flaeche liegt. Auch ein langes Stueck zaehlt: in einem
    // grossen Polygon sind es 70 yd zur Seite und 139 yd hoch, ein Verhaeltnis von 1:2.
    inline bool IsPinnedStep(float ax, float ay, float az, float bx, float by, float bz,
                             float tx, float ty, float tz)
    {
        if (geometry::Hypot2(bx - tx, by - ty) > geometry::PIN_XY || std::fabs(bz - tz) > geometry::PIN_Z)
            return false;
        float const horizontal = geometry::Hypot2(bx - ax, by - ay);
        float const dz = std::fabs(bz - az);
        return dz >= geometry::PIN_MIN_RISE && dz > geometry::PIN_MIN_SLOPE * horizontal;
    }

    // Schneidet das letzte Wegstueck eines Teilwegs ab, wenn man es nicht ablaufen kann: ein
    // senkrechtes (IsVerticalJump) oder eines, das auf die Zielposition festgenagelt ist
    // (IsPinnedStep). Rueckgabe: Zahl der entfernten Punkte (0 oder 1). Bleibt nur ein Punkt
    // uebrig, gibt es nichts zu laufen. 'Points' ist ein Behaelter von Punkten mit x, y, z
    // (std::vector<G3D::Vector3>); (tx, ty, tz) ist das Ziel, das dem Core uebergeben wurde.
    template <class Points>
    inline size_t TrimDeadTail(Points& pts, float tx, float ty, float tz)
    {
        if (pts.size() < 2)
            return 0;
        auto const& a = pts[pts.size() - 2];
        auto const& b = pts[pts.size() - 1];
        if (!IsVerticalJump(a.x, a.y, a.z, b.x, b.y, b.z) && !IsPinnedStep(a.x, a.y, a.z, b.x, b.y, b.z, tx, ty, tz))
            return 0;
        pts.pop_back();
        return 1;
    }

    // Sieht ein Teilweg nach einer Sackgasse aus? (ex, ey, ez) ist sein letzter BEGEHBARER
    // Punkt, also nach TrimDeadTail.
    //
    // PartialEndsUnderTarget: das Ende liegt fast senkrecht unter oder ueber dem Ziel
    // (Etagenunterschied ab 20 yd, in der Ebene hoechstens 30 yd, bei weiten Wegen ein Viertel
    // der Startentfernung, aber nie mehr als 60 yd). Streng genug, um dem Spieler "Sackgasse"
    // zu melden.
    inline bool PartialEndsUnderTarget(float sx, float sy, float sz,
                                       float dx, float dy, float dz,
                                       float ex, float ey, float ez)
    {
        geometry::PartialMeasure const m = geometry::Measure(sx, sy, sz, dx, dy, dz, ex, ey, ez);
        if (!m.valid || m.end3 < geometry::AT_TARGET)
            return false;

        return m.dzEnd >= 20.0f && m.end2 <= std::max(30.0f, std::min(60.0f, 0.25f * m.start2));
    }

    // PartialLooksStuck: das, oder das Ende liegt in der Naehe des Ziels auf einer anderen Hoehe
    // (ab 10 yd, in der Ebene hoechstens 40 yd bis 100 yd je nach Entfernung), oder das Ziel ist in
    // 3D hoechstens 20 % naeher als vom Start. Das loest nur die erweiterte Wegsuche aus; ein
    // langer Weg ueber freies Gelaende, den der PathGenerator wegen seiner Laengengrenze
    // abschneidet, kann dazugehoeren, und die Suche schadet ihm nicht.
    inline bool PartialLooksStuck(float sx, float sy, float sz,
                                  float dx, float dy, float dz,
                                  float ex, float ey, float ez)
    {
        geometry::PartialMeasure const m = geometry::Measure(sx, sy, sz, dx, dy, dz, ex, ey, ez);
        if (!m.valid || m.end3 < geometry::AT_TARGET)
            return false;

        if (PartialEndsUnderTarget(sx, sy, sz, dx, dy, dz, ex, ey, ez))
            return true;

        // andere Hoehe, nicht weit vom Ziel
        if (m.dzEnd >= 10.0f && m.end2 <= std::min(100.0f, std::max(40.0f, 0.25f * m.start2)))
            return true;

        // kaum Fortschritt
        return m.start3 > 1.0f && m.end3 > 0.8f * m.start3;
    }
}

#endif
