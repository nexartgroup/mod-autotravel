/*
 * AutoTravel_Search.h
 * ---------------------------------------------------------------------------
 * Wegsuche auf dem NavMesh mit eigenem, grossem Suchbudget.
 *
 * Der PathGenerator des Cores sucht mit hoechstens 1024 Knoten und liefert hoechstens
 * 74 Polygone. In einer grossen Stadt mit mehreren Etagen reicht das nicht bis zum
 * Ausgang: Detour erschoepft den Knotenpool im Innenraum und gibt dann den besten
 * Knoten nach LUFTLINIE zum Ziel zurueck -- bei einem Ziel eine Etage hoeher die Stelle
 * direkt darunter, eine Sackgasse an der Wand. Diese Suche laeuft mit einem Vielfachen
 * der Knoten und einer Zeitgrenze und liefert nur dann einen Weg, wenn er das Ziel
 * wirklich erreicht.
 *
 * Reine Detour-Funktion: kein Core, kein Spieler. So laeuft sie auch in
 * tools/tests/search_test.cpp gegen ein selbstgebautes NavMesh.
 * ---------------------------------------------------------------------------
 */

#ifndef MOD_AUTOTRAVEL_SEARCH_H
#define MOD_AUTOTRAVEL_SEARCH_H

#include <vector>

class dtNavMesh;

namespace AT
{
    // Der PathGenerator wertet Start oder Ziel, das mehr als so weit (yd) neben dem NavMesh liegt, als
    // "weit weg" (FARFROMPOLY): der Weg ist unvollstaendig, und das Ziel wird auf den naechsten Punkt
    // des Zielpolygons gesetzt.
    constexpr float NAV_FAR_FROM_POLY = 7.0f;

    struct NavSearchLimits
    {
        int            maxNodes  = 16384;   // Suchknoten (Detour: 64..65535); der Core nimmt 1024
        int            maxPolys  = 4096;    // Laenge des Polygonwegs
        int            maxPoints = 2048;    // Eckpunkte des Ergebnisses
        double         maxMs     = 30.0;    // Zeitgrenze in Millisekunden; <= 0: keine
        unsigned short includeFlags = 0;    // wie der Filter des PathGenerators
        unsigned short excludeFlags = 0;
        float          farFromPoly = NAV_FAR_FROM_POLY;
    };

    enum class NavSearchStatus
    {
        Complete,       // der Weg erreicht das Zielpolygon
        Partial,        // die Suche ist erschoepft, ohne das Ziel zu erreichen: keine Verbindung im NavMesh
        OutOfNodes,     // der Suchpool war zu klein; ob es einen Weg gibt, ist offen
        TimedOut,       // die Zeitgrenze wurde erreicht
        TooLong,        // der Weg ist laenger als maxPolys / maxPoints
        NoStartPoly,    // am Start liegt kein Polygon
        NoEndPoly,      // am Ziel liegt kein Polygon
        Failed          // Detour meldet einen Fehler
    };

    struct NavSearchResult
    {
        NavSearchStatus status = NavSearchStatus::Failed;
        std::vector<float> points;   // nur bei Complete: x, y, z je Punkt, Weltkoordinaten
        int    polys = 0;            // Laenge des Polygonwegs
        int    nodes = 0;            // abgearbeitete Suchschritte
        double ms    = 0.0;
        // Abstand von Start und Ziel zum gewaehlten Polygon (yd). Der PathGenerator wertet ein Ziel,
        // das mehr als 7 yd neben dem NavMesh liegt, als unvollstaendig ("FARFROMPOLY"); wer die
        // Suche benutzt, kann das ebenso tun.
        float  startDist = 0.0f;
        float  endDist   = 0.0f;
        // Start oder Ziel lag weiter als farFromPoly neben dem NavMesh: das Ziel des Wegs ist wie im
        // PathGenerator der naechste Punkt des Zielpolygons, nicht die rohe Zielposition.
        bool   endClamped = false;
    };

    // Start und Ziel in WELTKOORDINATEN (x, y, z); intern wird nach Detour (y, z, x) gedreht.
    NavSearchResult NavSearch(dtNavMesh const* nav,
                              float sx, float sy, float sz,
                              float dx, float dy, float dz,
                              NavSearchLimits const& limits);

    char const* NavSearchStatusText(NavSearchStatus status);
}

#endif
