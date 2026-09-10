/*
 * AutoTravel_SC.cpp
 * ---------------------------------------------------------------------------
 * Befehlsschnittstelle und Anbindung an den Core.
 *
 * Alle Befehle beginnen mit ".at". Erzeugt werden sie normalerweise vom Addon;
 * von Hand tippen laesst sich trotzdem jeder davon.
 *
 * Zur Rechtelage: die Unterbefehle koennen nichts, was der Spieler nicht
 * ohnehin darf -- mit EINER Ausnahme. ".at tp" umgeht jede Wegfindung und nimmt
 * beliebige Zielkoordinaten entgegen. In der Vorfassung lag er unter demselben
 * SEC_PLAYER wie alles andere, womit sich jeder Spieler ueberallhin versetzen
 * konnte. Er hat deshalb jetzt eine eigene Rechtepruefung.
 *
 * Zu den Eingabewerten: sie stammen aus einer Chatnachricht. atoi("abc") ist
 * still 0, atof("nan") ergibt NaN -- und NaN pflanzt sich durch die gesamte
 * Wegfindung fort. Deshalb wird jeder Wert geprueft und ungueltige Eingabe
 * abgewiesen, statt sie in eine 0 zu verwandeln.
 */

#include "AutoTravel.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <sstream>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    std::vector<std::string> Split(std::string const& in)
    {
        std::vector<std::string> out;
        std::istringstream iss(in);
        std::string t;
        while (iss >> t)
            out.push_back(t);
        return out;
    }

    std::string JoinFrom(std::vector<std::string> const& v, size_t start)
    {
        std::string r;
        for (size_t i = start; i < v.size(); ++i)
        {
            if (!r.empty())
                r += " ";
            r += v[i];
        }
        return r;
    }
}

class autotravel_commandscript : public CommandScript
{
public:
    autotravel_commandscript() : CommandScript("autotravel_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "at", HandleAt, SEC_PLAYER, Console::No }
        };
        return table;
    }

    // Aufbau der Zielbefehle:
    //
    //   .at start   <uiMapId> <nx> <ny> <hasCalib> <pnx> <pny> <curMap> <cnx> <cny> <Name...>
    //   .at tp      <wie start>
    //   .at resolve <wie start, ohne Name>
    //   .at diag    <wie start, ohne Name>
    //
    // curMap/cnx/cny sind die Karten-ID und die normalisierte Position der
    // Zone, in der der Spieler GERADE steht. Damit kann der Server die
    // Zuordnung Client-ID -> WorldMapArea-ID auch dann pruefen, wenn das Ziel
    // in einer anderen Zone liegt.
    static bool HandleAt(ChatHandler* handler, Tail argsTail)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return false;

        std::vector<std::string> a = Split(std::string(argsTail));
        if (a.empty())
        {
            sAutoTravel->PrintStatus(player);
            return true;
        }

        std::string const& cmd = a[0];

        // --- Handschlag ----------------------------------------------------
        // Das Addon schickt dies beim Login und wartet auf die Antwort, bevor
        // es weitere Befehle sendet. Ohne diesen Handschlag wuerde ein
        // fehlendes Servermodul dazu fuehren, dass der Charakter ".at start"
        // laut im Chat sagt.
        if (cmd == "hello" || cmd == "ping")
        {
            sAutoTravel->SendHello(player);
            return true;
        }

        // --- Zielbefehle ---------------------------------------------------
        if (cmd == "start" || cmd == "tp" || cmd == "resolve" || cmd == "diag")
        {
            if (a.size() < 7)
            {
                sAutoTravel->Msg(player, "Ungueltige Parameter.");
                return true;
            }

            uint32 uiMapId = 0;
            float nx = 0.0f, ny = 0.0f, pnx = 0.0f, pny = 0.0f;
            bool hasCalib = false;

            if (!AT::ParseUInt(a[1], uiMapId) || !uiMapId
                || !AT::ParseNorm(a[2], nx) || !AT::ParseNorm(a[3], ny)
                || !AT::ParseBool(a[4], hasCalib)
                || !AT::ParseNorm(a[5], pnx) || !AT::ParseNorm(a[6], pny))
            {
                sAutoTravel->Msg(player, "Ungueltige Parameter - Befehl abgewiesen.");
                return true;
            }

            if (cmd == "tp")
            {
                if (handler->GetSession()->GetSecurity() < AccountTypes(ATConf.teleportSecurity))
                {
                    sAutoTravel->Msg(player, "Teleport ist dir nicht erlaubt.");
                    return true;
                }
            }

            if (a.size() >= 10)
            {
                uint32 curMap = 0;
                float cnx = 0.0f, cny = 0.0f;
                if (AT::ParseUInt(a[7], curMap) && curMap
                    && AT::ParseNorm(a[8], cnx) && AT::ParseNorm(a[9], cny))
                {
                    sAutoTravel->LearnMapId(player, curMap, cnx, cny);
                }
            }

            std::string name = JoinFrom(a, 10);

            if (cmd == "start")
                sAutoTravel->Start(player, uiMapId, nx, ny, hasCalib, pnx, pny, name);
            else if (cmd == "tp")
                sAutoTravel->Teleport(player, uiMapId, nx, ny, hasCalib, pnx, pny, name);
            else if (cmd == "diag")
                sAutoTravel->Diagnose(player, uiMapId, nx, ny, hasCalib, pnx, pny);
            else
                sAutoTravel->Resolve(player, uiMapId, nx, ny, hasCalib, pnx, pny);

            return true;
        }

        // --- Route uebertragen ---------------------------------------------
        if (cmd == "route")
        {
            // .at route <0=neu|1=anhaengen> <map:nx:ny:art> ...
            if (a.size() < 3)
                return true;

            uint32 mode = 0;
            if (!AT::ParseUInt(a[1], mode))
                return true;

            sAutoTravel->RouteAdd(player, mode == 0, JoinFrom(a, 2));
            return true;
        }

        if (cmd == "rstart")
        {
            // .at rstart <curMap> <cnx> <cny> <Name...>
            if (a.size() >= 4)
            {
                uint32 curMap = 0;
                float cnx = 0.0f, cny = 0.0f;
                if (AT::ParseUInt(a[1], curMap) && curMap
                    && AT::ParseNorm(a[2], cnx) && AT::ParseNorm(a[3], cny))
                {
                    sAutoTravel->LearnMapId(player, curMap, cnx, cny);
                }
                sAutoTravel->RouteStart(player, JoinFrom(a, 4));
            }
            return true;
        }

        // --- Steuerung -----------------------------------------------------
        if (cmd == "stop")
        {
            sAutoTravel->Stop(player, "Reise gestoppt.");
            return true;
        }

        if (cmd == "pause")
        {
            sAutoTravel->PauseByPlayer(player, JoinFrom(a, 1));
            return true;
        }

        if (cmd == "resume")
        {
            sAutoTravel->ResumeByPlayer(player);
            return true;
        }

        if (cmd == "repath")
        {
            sAutoTravel->Repath(player);
            return true;
        }

        if (cmd == "status")
        {
            sAutoTravel->PrintStatus(player);
            return true;
        }

        // --- Auskunft ------------------------------------------------------
        if (cmd == "nodes")
        {
            sAutoTravel->NodeInfo(player);
            return true;
        }

        if (cmd == "taxi")
        {
            sAutoTravel->TaxiInfo(player);
            return true;
        }

        if (cmd == "options" || cmd == "opts")
        {
            sAutoTravel->ListOptions(player, a.size() > 1 ? a[1] : std::string());
            return true;
        }

        // --- Einstellungen -------------------------------------------------
        if (cmd == "debug")
        {
            bool on = true;
            if (a.size() > 1 && !AT::ParseBool(a[1], on))
            {
                sAutoTravel->Msg(player, "Ungueltiger Parameter.");
                return true;
            }
            sAutoTravel->SetDebug(player, on);
            return true;
        }

        if (cmd == "set")
        {
            if (a.size() < 3)
            {
                sAutoTravel->Msg(player, "Verwendung: .at set <schluessel> <wert>");
                return true;
            }

            // Werte, die nur der Serververwalter aendern darf: alles, was
            // andere Spieler mitbetrifft. Die beiden Sitzungswerte (arrival,
            // grace) bleiben fuer jeden offen.
            if (a[1] != "arrival" && a[1] != "grace")
            {
                if (handler->GetSession()->GetSecurity() < SEC_GAMEMASTER)
                {
                    sAutoTravel->Msg(player,
                        "Diese Einstellung gilt fuer den ganzen Server und darf nur ein "
                        "Spielleiter aendern.");
                    return true;
                }
            }

            sAutoTravel->SetOption(player, a[1], a[2]);
            return true;
        }

        sAutoTravel->Msg(player, "Unbekannter Unterbefehl. '.at options' zeigt die Einstellungen.");
        return true;
    }
};

// ---------------------------------------------------------------------------
// Anbindung an den Core
// ---------------------------------------------------------------------------
//
// Bewusst NUR WorldScript. Ein PlayerScript fuer das Ausloggen waere nett, ist
// aber nicht noetig: die Clientkontrolle wird nicht in der Datenbank
// gespeichert, sie steht nach jedem Login wieder beim Client. Verwaiste
// Sitzungen raeumt der Takt selbst ab, sobald der Spieler nicht mehr in der
// Welt ist.
//
// Der Verzicht hat einen zweiten Grund: AzerothCore hat die PlayerScript-Hooks
// zwischenzeitlich von OnLogout auf OnPlayerLogout umbenannt. Ein Modul, das
// sie benutzt, baut je nach Corestand nicht mehr. WorldScript::OnUpdate und
// OnAfterConfigLoad sind dagegen seit Jahren unveraendert.

class autotravel_worldscript : public WorldScript
{
public:
    autotravel_worldscript() : WorldScript("autotravel_worldscript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sAutoTravel->LoadConfig();
    }

    void OnUpdate(uint32 diff) override
    {
        sAutoTravel->Update(diff);
    }
};

void AddSC_autotravel()
{
    new autotravel_commandscript();
    new autotravel_worldscript();
}
