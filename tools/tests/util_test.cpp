/*
 * util_test.cpp -- Tests fuer AutoTravel_Util.cpp
 * ---------------------------------------------------------------------------
 * Alles, was der Client schickt, laeuft zuerst durch diese Funktionen. Die
 * Tests decken deshalb vor allem Ausreisser ab: NaN, Unendlich, Ueberlauf,
 * Reste hinter der Zahl, Steuerzeichen, abgeschnittene UTF-8-Zeichen.
 *
 * Bauen und starten: tools/check.sh <pfad-zu-azerothcore>
 * Braucht von AzerothCore nur die Header (Basistypen), nichts wird gelinkt.
 */

#include "AutoTravel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond) \
    do { \
        ++sChecks; \
        if (!(cond)) { ++sFailures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static void TestParseUInt()
{
    uint32 v = 123;
    CHECK(AT::ParseUInt("0", v) && v == 0);
    CHECK(AT::ParseUInt("4294967295", v) && v == 4294967295u);
    CHECK(!AT::ParseUInt("4294967296", v));            // eins zu gross
    CHECK(!AT::ParseUInt("99999999999999999999", v));  // ERANGE
    CHECK(!AT::ParseUInt("", v));
    CHECK(!AT::ParseUInt("abc", v));                   // atoi haette still 0 geliefert
    CHECK(!AT::ParseUInt("12abc", v));                 // Rest hinter der Zahl
    CHECK(!AT::ParseUInt("1.5", v));
    CHECK(!AT::ParseUInt(" 7", v));                    // kein fuehrender Leerraum
    CHECK(!AT::ParseUInt("+7", v));
    CHECK(!AT::ParseUInt("-1", v));
    CHECK(!AT::ParseUInt("-18446744073709551615", v)); // strtoul rechnet das still zu 1 um
    CHECK(!AT::ParseUInt("0x10", v));
}

static void TestParseFloat()
{
    float v = 0.0f;
    CHECK(AT::ParseFloat("1.5", v) && v == 1.5f);
    CHECK(AT::ParseFloat("-0.25", v) && v == -0.25f);
    CHECK(!AT::ParseFloat("nan", v));                  // NaN pflanzt sich durch die Wegfindung fort
    CHECK(!AT::ParseFloat("NaN", v));
    CHECK(!AT::ParseFloat("inf", v));
    CHECK(!AT::ParseFloat("-inf", v));
    CHECK(!AT::ParseFloat("1e999", v));                // ERANGE
    CHECK(!AT::ParseFloat("1e39", v));                 // endlich als double, +inf als float
    CHECK(!AT::ParseFloat("-1e39", v));
    CHECK(AT::ParseFloat("3.0e38", v) && std::isfinite(v));   // knapp unter FLT_MAX
    CHECK(!AT::ParseFloat("", v));
    CHECK(!AT::ParseFloat("1.5x", v));
    CHECK(!AT::ParseFloat(".", v));
    CHECK(!AT::ParseFloat("0x1p3", v));                // Hexfloat
    CHECK(!AT::ParseFloat("infinity", v));
    CHECK(!AT::ParseFloat(" 1", v));
    CHECK(AT::ParseFloat("1e3", v) && v == 1000.0f);
    CHECK(AT::ParseFloat("+2", v) && v == 2.0f);
}

static void TestParseBool()
{
    bool b = false;
    CHECK(AT::ParseBool("1", b) && b);
    CHECK(AT::ParseBool("0", b) && !b);
    CHECK(AT::ParseBool("on", b) && b);
    CHECK(AT::ParseBool("aus", b) && !b);
    CHECK(AT::ParseBool("true", b) && b);
    CHECK(!AT::ParseBool("2", b));
    CHECK(!AT::ParseBool("yes please", b));
    CHECK(!AT::ParseBool("", b));
    CHECK(!AT::ParseBool("-1", b));
}

static void TestParseNorm()
{
    float v = -1.0f;
    CHECK(AT::ParseNorm("0", v) && v == 0.0f);
    CHECK(AT::ParseNorm("1", v) && v == 1.0f);
    CHECK(AT::ParseNorm("0.5", v) && v == 0.5f);
    CHECK(!AT::ParseNorm("1.0001", v));
    CHECK(!AT::ParseNorm("-0.0001", v));
    CHECK(!AT::ParseNorm("nan", v));
}

static void TestSanitize()
{
    // Trenner und Steuerzeichen
    CHECK(AT::SanitizeText("a|b", 48) == "ab");
    CHECK(AT::SanitizeText("|cffff0000rot|r", 48) == "cffff0000rotr");
    CHECK(AT::SanitizeText(std::string("a\nb\tc\x01" "d\x7f" "e"), 48) == "abcde");

    // Leerraum an den Raendern, innen bleibt er
    CHECK(AT::SanitizeText("  Sturmwind Bank  ", 48) == "Sturmwind Bank");
    CHECK(AT::SanitizeText("   ", 48).empty());
    CHECK(AT::SanitizeText("", 48).empty());
    CHECK(AT::SanitizeText("|||", 48).empty());

    // Laengenbegrenzung in Bytes
    CHECK(AT::SanitizeText(std::string(200, 'x'), 48).size() == 48);
    CHECK(AT::SanitizeText("abcdef", 6) == "abcdef");
    CHECK(AT::SanitizeText("abcdefg", 6) == "abcdef");

    // UTF-8: "Ä" = C3 84. Bei maxBytes 3 darf nach "ab" nichts halbes stehen bleiben.
    std::string r = AT::SanitizeText("ab\xC3\x84" "cd", 3);
    CHECK(r == "ab");
    // Passt das Zeichen ganz hinein, bleibt es.
    CHECK(AT::SanitizeText("ab\xC3\x84" "cd", 4) == "ab\xC3\x84");
    // Dreibytezeichen "€" = E2 82 AC an der Grenze
    CHECK(AT::SanitizeText("a\xE2\x82\xAC" "b", 3) == "a");
    CHECK(AT::SanitizeText("a\xE2\x82\xAC" "b", 4) == "a\xE2\x82\xAC");

    // Ergebnis enthaelt nie ein '|' oder Steuerzeichen, egal was hereinkommt
    std::string nasty;
    for (int i = 0; i < 256; ++i)
        nasty.push_back(char(i));
    std::string clean = AT::SanitizeText(nasty, 300);
    bool ok = true;
    for (unsigned char c : clean)
        if (c < 0x20 || c == 0x7F || c == '|')
            ok = false;
    CHECK(ok);
}

static void TestIdentifier()
{
    CHECK(AT::IsSafeIdentifier("acore_playerbots"));
    CHECK(AT::IsSafeIdentifier("db$1"));
    CHECK(AT::IsSafeIdentifier("A"));
    CHECK(!AT::IsSafeIdentifier(""));
    CHECK(!AT::IsSafeIdentifier("a b"));
    CHECK(!AT::IsSafeIdentifier("a`b"));
    CHECK(!AT::IsSafeIdentifier("a'b"));
    CHECK(!AT::IsSafeIdentifier("a;drop table x"));
    CHECK(!AT::IsSafeIdentifier("a.b"));
    CHECK(AT::IsSafeIdentifier("acore-playerbots"));   // gueltiger Datenbankname mit Bindestrich
    CHECK(!AT::IsSafeIdentifier("a\\b"));
    CHECK(!AT::IsSafeIdentifier("a\nb"));
    CHECK(!AT::IsSafeIdentifier("a\"b"));
    CHECK(!AT::IsSafeIdentifier(std::string(65, 'a')));
    CHECK(AT::IsSafeIdentifier(std::string(64, 'a')));
}

static void TestMisc()
{
    CHECK(std::fabs(AT::Dist2D(0, 0, 3, 4) - 5.0f) < 1e-5f);
    CHECK(AT::PathTypeName(0) == "BLANK");
    CHECK(AT::PathTypeName(0x01 | 0x04).find("NORMAL") != std::string::npos);
    CHECK(AT::PathTypeName(0x01 | 0x04).find("INCOMPLETE") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Spline-Pakete
// ---------------------------------------------------------------------------
//
// Bildet WriteLinearPath / ByteBuffer::appendPackXYZ des Cores nach (11/11/10
// Bit, 0,25 yd, Abstand zur Mitte von erstem und letztem Punkt) und liest die
// Werte so zurueck, wie es ein Client mit vorzeichenbehafteten Bitfeldern tut.
// Damit pruefen die Tests nicht nur die Grenzen, sondern, dass alles, was
// SplineFitsPacket durchlaesst, wirklich unversehrt ankommt -- und was es
// abweist, tatsaechlich zerstoert wuerde (der Fehler, der Charaktere ueber die
// Karte "fliegen" liess).

static float UnpackSigned(uint32 raw, int bits)
{
    int v = int(raw & ((1u << bits) - 1));
    if (v & (1 << (bits - 1)))
        v -= (1 << bits);
    return float(v) * 0.25f;
}

static Movement::PointsArray PacketRoundTrip(Movement::PointsArray const& pts)
{
    Movement::PointsArray out;
    if (pts.size() < 3)
        return pts;

    G3D::Vector3 const mid = (pts.front() + pts.back()) * 0.5f;
    out.push_back(pts.front());
    for (size_t i = 1; i + 1 < pts.size(); ++i)
    {
        G3D::Vector3 const off = mid - pts[i];
        uint32 const px = uint32(int(off.x / 0.25f)) & 0x7FFu;
        uint32 const py = uint32(int(off.y / 0.25f)) & 0x7FFu;
        uint32 const pz = uint32(int(off.z / 0.25f)) & 0x3FFu;
        out.push_back(G3D::Vector3(mid.x - UnpackSigned(px, 11),
                                   mid.y - UnpackSigned(py, 11),
                                   mid.z - UnpackSigned(pz, 10)));
    }
    out.push_back(pts.back());
    return out;
}

static float MaxDeviation(Movement::PointsArray const& a, Movement::PointsArray const& b)
{
    float worst = 0.0f;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        worst = std::max(worst, (a[i] - b[i]).length());
    return worst;
}

// Gerade Strecke mit einem Punkt je 'step' yd.
static Movement::PointsArray Line(G3D::Vector3 const& a, G3D::Vector3 const& b, float step)
{
    Movement::PointsArray out;
    float const len = (b - a).length();
    uint32 const n = std::max<uint32>(1, uint32(std::ceil(len / step)));
    for (uint32 i = 0; i <= n; ++i)
        out.push_back(a + (b - a) * (float(i) / float(n)));
    return out;
}

static void TestSplinePacket()
{
    using G3D::Vector3;

    // Der gemeldete Abschnitt: rund 750 yd Luftlinie ueber den See.
    Movement::PointsArray lake = Line(Vector3(-5702.6f, -3125.3f, 315.9f),
                                      Vector3(-5700.3f, -3871.3f, 331.6f), 3.0f);
    CHECK(!AT::SplineFitsPacket(lake));
    CHECK(MaxDeviation(lake, PacketRoundTrip(lake)) > 100.0f);       // der Client sah einen anderen Weg

    // Der ebenfalls gemeldete 279-yd-Abschnitt war in Ordnung.
    Movement::PointsArray ok = Line(Vector3(-5447.6f, -3121.6f, 348.1f),
                                    Vector3(-5714.6f, -3118.5f, 315.8f), 3.0f);
    CHECK(AT::SplineFitsPacket(ok));
    CHECK(MaxDeviation(ok, PacketRoundTrip(ok)) < 0.5f);             // 0,25-yd-Raster je Achse

    // Grenzfaelle: knapp innerhalb und knapp ausserhalb.
    Movement::PointsArray edgeIn = Line(Vector3(0, 0, 0), Vector3(2 * 215.0f, 0, 0), 3.0f);
    CHECK(AT::SplineFitsPacket(edgeIn));
    CHECK(MaxDeviation(edgeIn, PacketRoundTrip(edgeIn)) < 0.5f);
    Movement::PointsArray edgeOut = Line(Vector3(0, 0, 0), Vector3(2 * 230.0f, 0, 0), 3.0f);
    CHECK(!AT::SplineFitsPacket(edgeOut));

    // Hoehe: +-128 yd sind hart, die Grenze liegt bei 100.
    Movement::PointsArray tall = Line(Vector3(0, 0, 0), Vector3(30, 0, 260), 3.0f);
    CHECK(!AT::SplineFitsPacket(tall));
    Movement::PointsArray low = Line(Vector3(0, 0, 0), Vector3(30, 0, 180), 3.0f);
    CHECK(AT::SplineFitsPacket(low));
    CHECK(MaxDeviation(low, PacketRoundTrip(low)) < 0.5f);

    // Ein Wegepunkt, der weit abseits liegt (Hin- und Rueckweg), zaehlt auch.
    Movement::PointsArray uturn = Line(Vector3(0, 0, 0), Vector3(400, 0, 0), 3.0f);
    Movement::PointsArray back = Line(Vector3(400, 0, 0), Vector3(2, 0, 0), 3.0f);
    uturn.insert(uturn.end(), back.begin() + 1, back.end());
    CHECK(!AT::SplineFitsPacket(uturn));

    // Ausreisser und Sonderfaelle.
    Movement::PointsArray nan2 = ok;
    nan2[5].z = std::numeric_limits<float>::quiet_NaN();
    CHECK(!AT::SplineFitsPacket(nan2));
    Movement::PointsArray two;
    two.push_back(Vector3(0, 0, 0));
    two.push_back(Vector3(5000, 0, 0));
    CHECK(AT::SplineFitsPacket(two));                                // nichts zu packen
    CHECK(AT::SplineFitsPacket(Movement::PointsArray()));
}

static void TestSplitLongSegments()
{
    using G3D::Vector3;

    // Ein 746-yd-Segment wird in Stuecke von hoechstens 40 yd geteilt, die
    // Hoehe verteilt sich linear, Endpunkt bleibt erhalten.
    Movement::PointsArray path;
    path.push_back(Vector3(0, 0, 100));                 // Index 0: Start
    path.push_back(Vector3(0, -746, 130));              // Index 1: Ziel
    Vector3 const origin(0, 0, 100);

    CHECK(AT::SplitLongSegments(path, 1, origin, AT::SPLINE_MAX_SEGMENT));
    CHECK(path.size() == 20);                           // 746 / 40 -> 19 Stuecke, 18 neue Punkte + 2 alte
    CHECK(path.front().z == 100.0f);                    // Davorliegendes unberuehrt
    CHECK(path.back().y == -746.0f && path.back().z == 130.0f);

    float longest = 0.0f;
    bool monotone = true;
    for (size_t i = 2; i < path.size(); ++i)
    {
        longest = std::max(longest, AT::Dist2D(path[i].x, path[i].y, path[i - 1].x, path[i - 1].y));
        if (path[i].z + 1e-4f < path[i - 1].z)
            monotone = false;
    }
    CHECK(longest <= AT::SPLINE_MAX_SEGMENT + 0.01f);
    CHECK(monotone);

    // Ein zweiter Durchgang aendert nichts mehr.
    size_t const before = path.size();
    CHECK(!AT::SplitLongSegments(path, 1, origin, AT::SPLINE_MAX_SEGMENT));
    CHECK(path.size() == before);

    // Teilen ab der Mitte: das erste verbleibende Segment beginnt bei 'origin'
    // (der Spielerposition), nicht beim vorigen Pfadpunkt.
    Movement::PointsArray p2;
    p2.push_back(Vector3(0, 0, 0));
    p2.push_back(Vector3(10, 0, 0));
    p2.push_back(Vector3(10, 200, 0));
    CHECK(AT::SplitLongSegments(p2, 2, Vector3(10, 0, 0), 40.0f));
    CHECK(p2.size() == 3 + 4);                          // 200 / 40 -> 5 Stuecke, 4 neue Punkte
    CHECK(p2[0].x == 0.0f && p2[1].x == 10.0f);

    // Kurze Wege und Randfaelle.
    Movement::PointsArray shortPath;
    shortPath.push_back(Vector3(0, 0, 0));
    shortPath.push_back(Vector3(30, 0, 0));
    CHECK(!AT::SplitLongSegments(shortPath, 1, Vector3(0, 0, 0), 40.0f));
    CHECK(!AT::SplitLongSegments(shortPath, 5, Vector3(0, 0, 0), 40.0f));   // from hinter dem Ende
    CHECK(!AT::SplitLongSegments(shortPath, 1, Vector3(0, 0, 0), 0.0f));    // unsinnige Laenge

    // Nach dem Teilen passt jede Gruppe aufeinanderfolgender Punkte, die
    // LaunchChunk nehmen darf (hier: 12 Punkte), in die Paketkodierung.
    Movement::PointsArray big;
    big.push_back(Vector3(0, 0, 0));
    big.push_back(Vector3(0, -5000, 0));
    AT::SplitLongSegments(big, 1, Vector3(0, 0, 0), AT::SPLINE_MAX_SEGMENT);
    bool fitsEverywhere = true;
    for (size_t i = 1; i + 12 <= big.size(); ++i)
    {
        Movement::PointsArray window(big.begin() + i, big.begin() + i + 12);
        if (!AT::SplineFitsPacket(window))
            fitsEverywhere = false;
    }
    CHECK(fitsEverywhere);
}

int main()
{
    TestParseUInt();
    TestParseFloat();
    TestParseBool();
    TestParseNorm();
    TestSanitize();
    TestIdentifier();
    TestMisc();
    TestSplinePacket();
    TestSplitLongSegments();

    std::printf("%d Pruefungen, %d Fehler\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
