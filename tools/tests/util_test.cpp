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

int main()
{
    TestParseUInt();
    TestParseFloat();
    TestParseBool();
    TestParseNorm();
    TestSanitize();
    TestIdentifier();
    TestMisc();

    std::printf("%d Pruefungen, %d Fehler\n", sChecks, sFailures);
    return sFailures ? 1 : 0;
}
