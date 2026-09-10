/*
 * AutoTravel_Session.cpp
 * ---------------------------------------------------------------------------
 * Der Ablauf einer Reise: Takt, Zustandswechsel und die Uebergabe zwischen
 * Spieler und Autopilot.
 *
 * Die Uebergabe ist der Kern dieses Moduls und bewusst einfach gehalten:
 *
 *   Der Autopilot faehrt  ->  er haelt die Clientkontrolle
 *   Irgendetwas passiert  ->  er gibt sie SOFORT zurueck
 *
 * "Irgendetwas" ist der Pausenknopf im Addon, ein Kampf, der Tod, ein
 * Kartenwechsel, ein Taxiflug, ein Transport oder das Ausloggen. In jedem
 * dieser Faelle steuert der Spieler; das Ziel bleibt gespeichert.
 *
 * Zurueck uebernimmt der Autopilot nur auf ausdrueckliche Ansage. Beim Kampf
 * macht er das nach der eingestellten Wartezeit von selbst; nach einer
 * Handpause erst, wenn das Addon meldet, dass der Spieler eine Weile keine
 * Eingabe mehr gemacht hat. Diese Ruhe-Erkennung sitzt im Client, weil nur er
 * Maus und Fenster sieht.
 */

#include "AutoTravel.h"

#include "GridDefines.h"
#include "Map.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------------
// Takt
// ---------------------------------------------------------------------------

void AutoTravelMgr::Update(uint32 diff)
{
    if (!ATConf.enable)
        return;

    _tick += diff;
    if (_tick < ATConf.updateIntervalMs)
        return;

    uint32 d = _tick;
    _tick = 0;

    for (auto it = _sessions.begin(); it != _sessions.end(); )
    {
        Player* player = ObjectAccessor::FindPlayer(it->first);
        if (!player || !player->IsInWorld())
        {
            it = _sessions.erase(it);
            continue;
        }

        if (it->second.state == AT_IDLE)
        {
            ++it;                          // reine Options- oder Debugsitzung
            continue;
        }

        UpdateSession(player, it->second, d);

        if (it->second.state == AT_IDLE
            && !it->second.debug
            && it->second.arrivalOverride <= 0.0f
            && it->second.graceOverride == 0)
        {
            it = _sessions.erase(it);
        }
        else
            ++it;
    }
}

// ---------------------------------------------------------------------------
// Abschluss
// ---------------------------------------------------------------------------

void AutoTravelMgr::Finish(Player* player, ATSession& s, std::string const& text, bool ok)
{
    HaltMovement(player, s);
    s.state = ok ? AT_ARRIVED : AT_FAILED;
    PushStatus(player, s);
    if (!text.empty())
        Msg(player, text);
    s.state = AT_IDLE;
    PushStatus(player, s);
}

// ---------------------------------------------------------------------------
// Uebergabe an den Spieler
// ---------------------------------------------------------------------------

void AutoTravelMgr::PauseByPlayer(Player* player, std::string const& why)
{
    ATSession* s = Find(player);
    if (!s || s->state == AT_IDLE)
    {
        Msg(player, "AutoTravel ist nicht aktiv.");
        return;
    }

    if (s->state == AT_PLAYER_CONTROL)
        return;                            // schon uebergeben, nichts zu tun

    HaltMovement(player, *s);

    s->pausedByPlayer = true;
    s->handoverTimer = 0;
    s->state = AT_PLAYER_CONTROL;
    s->path.clear();
    s->idx = 0;
    s->flying = false;
    s->offMeshHits = 0;

    Msg(player, why.empty()
        ? "Du hast uebernommen. Das Ziel bleibt gespeichert."
        : why);
    PushStatus(player, *s);
}

void AutoTravelMgr::ResumeByPlayer(Player* player)
{
    ATSession* s = Find(player);
    if (!s || s->state == AT_IDLE)
    {
        Msg(player, "AutoTravel ist nicht aktiv.");
        return;
    }

    if (s->state != AT_PLAYER_CONTROL)
        return;

    if (!player->IsAlive())
    {
        Msg(player, "Du bist tot - die Reise geht erst nach der Wiederbelebung weiter.");
        return;
    }
    if (player->IsInCombat())
    {
        s->state = AT_COMBAT_PAUSED;
        s->pausedByPlayer = false;
        s->combatTimer = 0;
        return;
    }

    s->pausedByPlayer = false;
    s->handoverTimer = 0;
    s->repathAttempts = 0;
    s->mountTried = false;
    s->rescueCount = 0;
    s->state = AT_CALCULATE_PATH;

    Msg(player, "Autopilot uebernimmt wieder.");
    PushStatus(player, *s);
}

// Prueft alles, was die Kontrolle an den Spieler zurueckgeben muss.
// Rueckgabe true = die Sitzung ist fuer diesen Takt fertig behandelt.
bool AutoTravelMgr::CheckHandover(Player* player, ATSession& s, uint32 diff)
{
    // --- Kampf -------------------------------------------------------------
    if (ATConf.pauseInCombat && player->IsInCombat())
    {
        if (s.state != AT_COMBAT_PAUSED)
        {
            // Kontrolle sofort zurueck: der Spieler (oder sein Playerbot) muss
            // sich wehren koennen. Der Playerbot wird bewusst NICHT
            // abgeschaltet -- das entscheidet das Addon, nicht dieses Modul.
            HaltMovement(player, s);
            s.state = AT_COMBAT_PAUSED;
            s.combatTimer = 0;
            s.path.clear();
            s.idx = 0;
            s.flying = false;
            Msg(player, "Kampf - Autopilot pausiert, du hast die Kontrolle.");
            Dbg(player, s, "Ziel bleibt gespeichert: " + s.destName);
            PushStatus(player, s);
        }
        else
            s.combatTimer = 0;
        return true;
    }

    // --- Kampf vorbei ------------------------------------------------------
    if (s.state == AT_COMBAT_PAUSED)
    {
        if (!ATConf.resumeAfterCombat)
        {
            Finish(player, s, "Kampf beendet - die Fortsetzung ist abgeschaltet.", false);
            return true;
        }

        s.combatTimer += diff;
        uint32 grace = s.graceOverride ? s.graceOverride : ATConf.combatGraceMs;
        if (s.combatTimer < grace)
            return true;

        // Hatte der Spieler vor dem Kampf von Hand pausiert, bleibt es dabei.
        if (s.pausedByPlayer)
        {
            s.state = AT_PLAYER_CONTROL;
            s.handoverTimer = 0;
            PushStatus(player, s);
            return true;
        }

        // Mit Addon: nicht sofort wieder zugreifen. Der Spieler hat gerade
        // gekaempft, hat vermutlich noch die Hand an der Maus und will
        // vielleicht pluendern, essen oder nachsehen, was da los war. Der
        // Autopilot geht deshalb in die Uebergabe und wartet, bis das Addon
        // Ruhe meldet -- dieselbe Logik wie nach einer Handpause.
        //
        // Ohne Addon gaebe es niemanden, der diese Meldung schicken koennte.
        // Dann wird wie bisher direkt weitergefahren.
        if (_addonPlayers.find(player->GetGUID()) != _addonPlayers.end())
        {
            s.state = AT_PLAYER_CONTROL;
            s.pausedByPlayer = false;
            s.handoverTimer = 0;
            s.repathAttempts = 0;
            s.mountTried = false;
            Msg(player, "Kampf beendet. Der Autopilot uebernimmt wieder, sobald du ihn laesst.");
            PushStatus(player, s);
            return true;
        }

        Msg(player, "Kampf beendet - der Weg wird von hier aus neu berechnet.");
        s.state = AT_CALCULATE_PATH;
        s.repathAttempts = 0;
        s.mountTried = false;
        PushStatus(player, s);
        return true;
    }

    // --- Handpause ---------------------------------------------------------
    if (s.state == AT_PLAYER_CONTROL)
    {
        s.handoverTimer += diff;

        // Sicherheitsnetz: meldet sich das Addon gar nicht mehr (Reload,
        // Absturz, deaktiviert), soll die Reise nicht ewig als "aktiv" in der
        // Sitzungsliste haengen.
        if (s.handoverTimer >= ATConf.handoverTimeoutMs)
        {
            Finish(player, s, "Autopilot beendet - seit langem keine Rueckmeldung.", false);
            return true;
        }

        // Statusmeldung im Ruhezustand seltener senden.
        s.statusTimer += diff;
        if (s.statusTimer >= 3000)
        {
            s.statusTimer = 0;
            PushStatus(player, s);
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Bodenkontakt
// ---------------------------------------------------------------------------
//
// Zwei Stoerungen mit demselben Bild: der Charakter huepft in der Fallanimation
// und laeuft dabei geradeaus weiter.
//
//   zu tief -> durch den Boden gefallen, kommt nie wieder hoch
//   zu hoch -> haengt in der Luft und sinkt langsam ab
//
// Bezugsgroesse ist NICHT die rohe Bodenhoehe, sondern die Reisehoehe: beim
// Schwimmen ist das die Wasseroberflaeche, sonst der Boden. Ohne das waere
// jeder Schwimmzug ein Fehlalarm, weil der Seegrund weit unten liegt.
//
// Rueckgabe true = es wurde eingegriffen, dieser Takt ist fertig.

bool AutoTravelMgr::CheckGroundContact(Player* player, ATSession& s)
{
    if (!ATConf.rescueUnderMesh)
        return false;
    if (s.flying || player->IsInFlight() || player->CanFly())
        return false;
    if (!s.IsDriving())
        return false;

    float px = player->GetPositionX();
    float py = player->GetPositionY();
    float pz = player->GetPositionZ();

    // --- Bezugshoehe bestimmen ---------------------------------------------
    //
    // Frueher wurde von pz+2 aus abwaerts gesucht. Auf Treppen und Rampen ist
    // das falsch: rutscht der Charakter mehr als zwei Yards durch die Stufe,
    // liegt pz+2 UNTER der Stufe, und die Suche findet als naechste Flaeche das
    // Rohgelaende - in Sturmwind rund 35 Yards tiefer unter der Stadt. Die
    // Rettung hat den Charakter dann genau dorthin gesetzt.
    //
    // Verlaesslicher ist der Pfad selbst: seine Punkte stammen aus dem NavMesh
    // und liegen auf der richtigen begehbaren Ebene.
    float refBase = pz;
    bool haveRef = false;

    if (!s.path.empty())
    {
        float best = 1.0e30f;
        for (size_t i = 0; i < s.path.size(); ++i)
        {
            float dx = s.path[i].x - px;
            float dy = s.path[i].y - py;
            float d2 = dx * dx + dy * dy;
            if (d2 < best)
            {
                best = d2;
                refBase = s.path[i].z;
                haveRef = true;
            }
        }
    }

    if (!haveRef && s.hasGoodZ)
    {
        refBase = s.lastGoodZ;
        haveRef = true;
    }

    float const range = std::max(4.0f, ATConf.rescueSearchRange);

    float ground = player->GetMap()->GetHeight(player->GetPhaseMask(), px, py,
                                               refBase + 2.0f, true, range);
    if (ground <= INVALID_HEIGHT)
        ground = player->GetMap()->GetHeight(player->GetPhaseMask(), px, py,
                                             pz + 2.0f, true, range);
    if (ground <= INVALID_HEIGHT)
        ground = BestGroundZ(player, px, py);

    float ref = (ground > INVALID_HEIGHT) ? TravelZ(player, px, py, ground) : INVALID_HEIGHT;

    // Liegt die gefundene Flaeche weit unter der Pfadhoehe, ist es ein anderes
    // Stockwerk. Dann lieber gar nicht eingreifen als in den Keller versetzen.
    if (haveRef && ref > INVALID_HEIGHT && ref < refBase - range)
    {
        Dbg(player, s, "Rettung abgebrochen: die gefundene Flaeche liegt eine Etage tiefer.");
        s.offMeshHits = 0;
        return false;
    }

    if (ref > INVALID_HEIGHT && std::fabs(pz - ref) <= ATConf.underMeshDepth)
    {
        s.lastGoodZ = pz;
        s.hasGoodZ = true;
    }

    bool tooLow  = (ref > INVALID_HEIGHT) && (pz < ref - ATConf.underMeshDepth);
    bool tooHigh = (ref > INVALID_HEIGHT) && (pz > ref + ATConf.aboveMeshHeight);

    if (!tooLow && !tooHigh)
    {
        s.offMeshHits = 0;
        return false;
    }

    // Mehrere Messungen hintereinander, damit ein Sprung, eine Rampe oder ein
    // Punkt unter einer Bruecke nicht faelschlich ausloest.
    if (++s.offMeshHits < 3)
        return false;

    s.offMeshHits = 0;
    ++s.rescueCount;

    float target = ref + 0.5f;
    HaltMovement(player, s);
    player->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
    player->NearTeleportTo(px, py, target, player->GetOrientation());
    player->SetFallInformation(0, target);

    char b[256];
    std::snprintf(b, sizeof(b),
                  "Kein Bodenkontakt (%.1f statt %.1f, %s) - zurueckgesetzt und neu berechnet.",
                  pz, target, tooLow ? "unter der Flaeche" : "in der Luft");
    Msg(player, b);

    s.path.clear();
    s.idx = 0;
    s.state = AT_CALCULATE_PATH;

    if (s.rescueCount >= ATConf.maxRescues)
        Finish(player, s, "Zu oft ohne Bodenkontakt - der Autopilot gibt auf.", false);

    return true;
}

// ---------------------------------------------------------------------------
// Transporte: Zeppelin, Schiff, Tiefenbahn
// ---------------------------------------------------------------------------
//
// Ein Transport bewegt sich unter dem Charakter weg. Weder die Wegfindung noch
// die Bodenrettung noch die Feststeck-Erkennung ergeben dort einen Sinn -- alle
// drei wuerden sofort und dauernd ausloesen.
//
// Deshalb: sobald der Charakter auf einem Transport steht, uebergibt der
// Autopilot vollstaendig und wartet. Steigt der Charakter wieder aus, wird von
// der neuen Position aus neu gerechnet. Das deckt Zeppelin, Schiff und
// Tiefenbahn gleichermassen ab, ohne fuer jedes Fahrzeug Sonderwissen zu
// brauchen.
//
// Rueckgabe true = dieser Takt ist fertig behandelt.

bool AutoTravelMgr::CheckTransport(Player* player, ATSession& s, uint32 diff)
{
    if (!ATConf.useTransports)
        return false;

    bool onTransport = !player->m_movementInfo.transport.guid.IsEmpty();

    if (onTransport)
    {
        if (s.state != AT_WAIT_TRANSPORT)
        {
            HaltMovement(player, s);
            s.state = AT_WAIT_TRANSPORT;
            s.transportGuid = player->m_movementInfo.transport.guid;
            s.waitTimer = 0;
            s.path.clear();
            s.idx = 0;
            s.flying = false;
            Msg(player, "An Bord - AutoTravel wartet auf die Ankunft.");
            PushStatus(player, s);
        }
        else
            s.waitTimer = 0;

        s.statusTimer += diff;
        if (s.statusTimer >= 2000)
        {
            s.statusTimer = 0;
            PushStatus(player, s);
        }
        return true;
    }

    // --- Gerade ausgestiegen ----------------------------------------------
    if (s.state == AT_WAIT_TRANSPORT && !s.transportGuid.IsEmpty())
    {
        s.transportGuid.Clear();
        s.waitTimer = 0;
        Msg(player, "Angekommen - die Reise wird fortgesetzt.");

        // Die Etappe, die zum Anleger gefuehrt hat, ist damit erledigt.
        if (s.legIdx < s.route.size() && s.route[s.legIdx].kind == AT_LEG_TRANSPORT)
        {
            if (!AdvanceLeg(player, s))
            {
                Finish(player, s, "Ziel erreicht - " + s.destName + ".", true);
                return true;
            }
        }
        else
        {
            s.state = AT_CALCULATE_PATH;
            s.path.clear();
            s.idx = 0;
        }

        s.repathAttempts = 0;
        s.mountTried = false;
        PushStatus(player, s);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Eine Sitzung, ein Takt
// ---------------------------------------------------------------------------

void AutoTravelMgr::UpdateSession(Player* player, ATSession& s, uint32 diff)
{
    // --- Taxiflug ----------------------------------------------------------
    // Waehrend eines Fluges wird NICHTS angefasst. Insbesondere darf hier kein
    // StopMoving() laufen: das loescht den Spline des
    // FlightPathMovementGenerator. Der Spieler sitzt dann zwar auf dem Greifen,
    // fliegt aber nicht und kann stattdessen zu Fuss herumlaufen.
    if (player->IsInFlight())
    {
        if (s.controlTaken)
            ReleaseControl(player, s);     // ohne StopMoving

        s.wasInFlight = true;
        if (s.state != AT_WAIT_TAXI)
        {
            s.state = AT_WAIT_TAXI;
            Msg(player, "Flug laeuft - AutoTravel wartet auf die Landung.");
            PushStatus(player, s);
        }
        return;
    }

    // --- Transporte --------------------------------------------------------
    if (CheckTransport(player, s, diff))
        return;

    // --- Harte Abbruchbedingungen -----------------------------------------
    if (player->IsBeingTeleported())
        return;                            // im naechsten Takt weiterschauen

    if (player->GetMapId() != s.mapId)
    {
        // Kartenwechsel gehoert bei Portalen und Schiffen zum Ablauf. Steht als
        // naechstes eine Sonderverbindung an, ist das kein Fehler.
        HaltMovement(player, s);

        bool expected = (s.state == AT_WAIT_MANUAL || s.state == AT_WAIT_TAXI
                         || s.state == AT_WAIT_TRANSPORT);

        if (expected && s.legIdx + 1 < s.route.size())
        {
            s.mapId = player->GetMapId();
            if (AdvanceLeg(player, s))
            {
                Msg(player, "Neue Karte erreicht - die Reise wird fortgesetzt.");
                PushStatus(player, s);
                return;
            }
        }

        Finish(player, s, "Karte gewechselt - Reise beendet.", false);
        return;
    }

    if (!player->IsAlive())
    {
        HaltMovement(player, s);
        if (ATConf.resumeAfterDeath)
        {
            if (s.state != AT_PLAYER_CONTROL)
            {
                s.state = AT_PLAYER_CONTROL;
                s.pausedByPlayer = true;
                s.handoverTimer = 0;
                Msg(player, "Du bist gestorben. Das Ziel bleibt gespeichert.");
                PushStatus(player, s);
            }
            return;
        }
        Finish(player, s, "Du bist gestorben - Reise beendet.", false);
        return;
    }

    if (player->GetVehicle())
    {
        HaltMovement(player, s);
        return;
    }

    // --- AFK-Kennzeichen -----------------------------------------------------
    //
    // Der Client setzt es nach ein paar Minuten ohne Tastendruck von selbst und
    // schickt es als CHAT_MSG_AFK an den Server. Waehrend der Autopilot faehrt,
    // ist das schlicht falsch: der Charakter legt Strecke zurueck. Sichtbar
    // wird der Unterschied spaetestens im Schlachtfeld, wo ein AFK-Kennzeichen
    // zum Hinauswurf fuehrt.
    //
    // Verhindern laesst sich das Setzen nicht -- die Entscheidung faellt im
    // Client, und ein Addon kann seinen Leerlaufzaehler nicht zuruecksetzen.
    // Loeschen laesst es sich hier aber sofort wieder.
    //
    // Bewusst NICHT waehrend AT_PLAYER_CONTROL: dort steuert der Spieler, und
    // wenn er wirklich weggeht, soll das auch so angezeigt werden. Sobald der
    // Autopilot uebernimmt, faellt das Kennzeichen im naechsten Takt weg.
    if (ATConf.suppressAfk && s.state != AT_PLAYER_CONTROL && player->isAFK())
    {
        player->ToggleAFK();
        Dbg(player, s, "AFK-Kennzeichen geloescht - der Autopilot faehrt gerade.");
    }

    // --- Uebergabe: Kampf und Handpause ------------------------------------
    if (CheckHandover(player, s, diff))
        return;

    // --- Bodenkontakt ------------------------------------------------------
    if (CheckGroundContact(player, s))
        return;

    // --- Untergetaucht -----------------------------------------------------
    // Normalfall ist Schwimmen an der Oberflaeche; die Pfadpunkte liegen dafuer
    // knapp unter dem Wasserspiegel. Bleibt der Charakter trotzdem laenger
    // untergetaucht, wird abgebrochen, bevor die Luft ausgeht.
    if (player->IsUnderWater() && s.IsDriving())
    {
        s.underwaterTimer += diff;
        if (!ATConf.swim || s.underwaterTimer > ATConf.maxUnderwaterMs)
        {
            Finish(player, s, ATConf.swim
                ? "Zu lange unter Wasser - Reise beendet, bevor die Luft ausgeht."
                : "Der Weg fuehrt durch Wasser, Schwimmen ist aber abgeschaltet.", false);
            return;
        }
    }
    else
        s.underwaterTimer = 0;

    // --- Statusausgabe -----------------------------------------------------
    s.statusTimer += diff;
    if (s.statusTimer >= 1000)
    {
        s.statusTimer = 0;
        PushStatus(player, s);
    }

    // --- Ankunft an der aktuellen Etappe -----------------------------------
    bool lastLeg = (s.legIdx + 1 >= s.route.size());
    float radius = lastLeg ? ArrivalDist(s) : ATConf.legDistance;

    // Beim Fliegen ist der Zielradius groesser, sonst kreist der Charakter.
    if (s.flying && !lastLeg)
        radius = std::max(radius, 25.0f);

    // Vor einem Flugmeister muss der Radius dagegen KLEINER sein: der Core
    // laesst den Abflug nur innerhalb von zwei Interaktionsdistanzen zu. Mit
    // dem normalen Etappenradius von 15 Yards wuerde der Flug jedes Mal mit
    // "zu weit weg" abgelehnt.
    if (!lastLeg && s.legIdx < s.route.size() && s.route[s.legIdx].kind == AT_LEG_TAXI)
        radius = std::min(radius, ATConf.taxiBoardDistance);

    float dist = player->GetExactDist2d(s.destX, s.destY);

    if (dist <= radius && s.state != AT_MOUNTING && s.state != AT_WAIT_TAXI)
    {
        ATLeg const& cur = s.route[s.legIdx];

        // --- Flugmeister erreicht ------------------------------------------
        if (!lastLeg && cur.kind == AT_LEG_TAXI)
        {
            HaltMovement(player, s);
            if (StartTaxi(player, s, cur))
            {
                s.state = AT_WAIT_TAXI;
                s.wasInFlight = false;
                s.waitTimer = 0;
                PushStatus(player, s);
                return;
            }

            // Flug nicht moeglich (kein Geld, Punkt unbekannt, NPC weg):
            // dann eben zu Fuss weiter.
            Msg(player, "Der Flug kam nicht zustande - es geht zu Fuss weiter.");
            if (!AdvanceLeg(player, s))
            {
                Finish(player, s, "Ziel erreicht - " + s.destName + ".", true);
                return;
            }
            PushStatus(player, s);
            return;
        }

        // --- Anleger oder Portal erreicht ----------------------------------
        if (!lastLeg && (cur.kind == AT_LEG_TRANSPORT || cur.kind == AT_LEG_PORTAL
                         || cur.kind == AT_LEG_MANUAL))
        {
            HaltMovement(player, s);
            s.state = (cur.kind == AT_LEG_TRANSPORT) ? AT_WAIT_TRANSPORT : AT_WAIT_MANUAL;
            s.waitTimer = 0;
            s.wasInFlight = false;

            std::string where = cur.nextName;
            if (where.empty() && s.legIdx + 1 < s.route.size())
                where = s.route[s.legIdx + 1].name;
            if (where.empty())
                where = "Richtung " + s.destName;

            char mb[352];
            std::snprintf(mb, sizeof(mb),
                          "Weiter per %s nach: %s. Nimm die Verbindung - AutoTravel macht "
                          "danach von selbst weiter.",
                          ATLegKindName(cur.kind), where.c_str());
            Msg(player, mb);
            PushStatus(player, s);
            return;
        }

        // --- Normale Etappe ------------------------------------------------
        HaltMovement(player, s);

        if (AdvanceLeg(player, s))
        {
            PushStatus(player, s);
            return;
        }

        // Beim Fliegen zum Abschluss absteigen, damit der Charakter nicht in
        // der Luft stehen bleibt.
        if (player->IsMounted() && player->CanFly())
            player->Dismount();

        Finish(player, s, "Ziel erreicht - " + s.destName + ".", true);
        return;
    }

    // --- Zustandsmaschine --------------------------------------------------
    switch (s.state)
    {
        // -------------------------------------------------------------------
        case AT_WAIT_TAXI:
        {
            if (s.wasInFlight)
            {
                s.wasInFlight = false;
                Msg(player, "Gelandet - die Reise wird fortgesetzt.");
                if (!AdvanceLeg(player, s))
                {
                    Finish(player, s, "Ziel erreicht - " + s.destName + ".", true);
                    return;
                }
                PushStatus(player, s);
                return;
            }

            // Kein Flug entstanden: nach kurzer Zeit einfach weiterlaufen.
            s.waitTimer += diff;
            if (s.waitTimer > 8000)
            {
                s.waitTimer = 0;
                s.state = AT_CALCULATE_PATH;
            }
            return;
        }

        // -------------------------------------------------------------------
        case AT_WAIT_MANUAL:
        case AT_WAIT_TRANSPORT:
        {
            s.waitTimer += diff;

            // Fortsetzen, sobald der Charakter in der Naehe des naechsten
            // Punktes auftaucht. Das deckt Portale, Schiffe und Zeppeline mit
            // ab, auch wenn sie keinen Kartenwechsel ausloesen.
            if (s.legIdx + 1 < s.route.size())
            {
                ATLeg const& nxt = s.route[s.legIdx + 1];
                if (nxt.resolved && nxt.mapId == player->GetMapId()
                    && player->GetExactDist2d(nxt.wx, nxt.wy) < 100.0f)
                {
                    Msg(player, "Verbindung genutzt - die Reise wird fortgesetzt.");
                    if (!AdvanceLeg(player, s))
                    {
                        Finish(player, s, "Ziel erreicht - " + s.destName + ".", true);
                        return;
                    }
                    PushStatus(player, s);
                    return;
                }
            }

            if (s.waitTimer > ATConf.transportWaitMs)
            {
                Msg(player, "Die Verbindung kam nicht zustande - der Weg wird ohne sie gesucht.");
                if (!AdvanceLeg(player, s))
                {
                    Finish(player, s, "Kein weiterer Stuetzpunkt - Reise beendet.", false);
                    return;
                }
                PushStatus(player, s);
            }
            return;
        }

        // -------------------------------------------------------------------
        case AT_MOUNTING:
        {
            s.mountTimer += diff;

            if (player->IsMounted())
            {
                Dbg(player, s, player->CanFly() ? "Flugmount aktiv." : "Reittier aktiv.");
                s.state = AT_CALCULATE_PATH;
                return;
            }

            if (s.mountTimer > 6000 || !player->HasUnitState(UNIT_STATE_CASTING))
            {
                Dbg(player, s, "Aufsitzen fehlgeschlagen - weiter zu Fuss.");
                s.state = AT_CALCULATE_PATH;
            }
            return;
        }

        // -------------------------------------------------------------------
        case AT_TAKEOFF:
        {
            // Reserviert fuer einen eigenen Steigflug. Der Steigflug ist Teil
            // der Luftroute, deshalb geht es hier direkt weiter.
            s.state = AT_CALCULATE_PATH;
            return;
        }

        // -------------------------------------------------------------------
        case AT_CALCULATE_PATH:
        {
            // In Gebaeuden absitzen: sonst steckt der Charakter in Tuerrahmen
            // fest und die Wegfindung schlaegt reihenweise fehl.
            if (ATConf.dismountIndoors && player->IsMounted()
                && !player->IsOutdoors() && !player->CanFly())
            {
                player->Dismount();
                player->RemoveAurasByType(SPELL_AURA_MOUNTED);
                Dbg(player, s, "Innenraum erkannt - abgestiegen.");
            }

            if (ATConf.autoMount && !s.mountTried && !player->IsMounted())
            {
                s.mountTried = true;
                if (TryMount(player, s))
                    return;
            }

            if (!CalculatePath(player, s))
            {
                ++s.repathAttempts;

                if (s.repathAttempts >= ATConf.maxRepathAttempts)
                {
                    HaltMovement(player, s);

                    // Nicht die ganze Reise wegwerfen: die naechste Etappe ist
                    // oft von hier aus erreichbar.
                    if (s.legIdx + 1 < s.route.size())
                    {
                        char sb[192];
                        std::snprintf(sb, sizeof(sb),
                                      "Stuetzpunkt %u ist nicht erreichbar - er wird uebersprungen.",
                                      uint32(s.legIdx + 1));
                        Msg(player, sb);
                        if (AdvanceLeg(player, s))
                        {
                            PushStatus(player, s);
                            return;
                        }
                    }

                    if (s.lastPathType & PATHFIND_NOT_USING_PATH)
                        Finish(player, s,
                               "Fuer diese Kartenkachel sind keine mmaps geladen. Der Server kann "
                               "hier keinen Weg berechnen; eine Luftlinie fuehrte quer durch Berge "
                               "und wird abgelehnt.", false);
                    else
                        Finish(player, s,
                               "Kein begehbarer Weg gefunden. '/at diag' im Addon zeigt, woran es "
                               "liegt.", false);
                    return;
                }

                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "Pfadberechnung fehlgeschlagen (Versuch %u von %u).",
                              s.repathAttempts, ATConf.maxRepathAttempts);
                Dbg(player, s, buf);
                return;                    // im naechsten Takt erneut versuchen
            }

            s.repathAttempts = 0;
            s.stuckTimer = 0;
            s.lastX = player->GetPositionX();
            s.lastY = player->GetPositionY();
            s.lastZ = player->GetPositionZ();

            char buf[160];
            std::snprintf(buf, sizeof(buf), "Pfad gefunden: %u Punkte%s%s",
                          uint32(s.path.size()),
                          s.pathIncomplete ? ", Teilweg" : "",
                          s.flying ? ", Luftroute" : "");
            Dbg(player, s, buf);

            s.state = AT_TRAVELING;
            LaunchChunk(player, s);
            PushStatus(player, s);
            return;
        }

        // -------------------------------------------------------------------
        case AT_TRAVELING:
        {
            // --- Feststecken ---------------------------------------------
            if (ATConf.stuckDetection)
            {
                s.stuckTimer += diff;
                if (s.stuckTimer >= ATConf.stuckTimeoutMs)
                {
                    float dx = player->GetPositionX() - s.lastX;
                    float dy = player->GetPositionY() - s.lastY;
                    float dz = player->GetPositionZ() - s.lastZ;
                    float moved = std::sqrt(dx * dx + dy * dy + dz * dz);

                    s.stuckTimer = 0;
                    s.lastX = player->GetPositionX();
                    s.lastY = player->GetPositionY();
                    s.lastZ = player->GetPositionZ();

                    if (moved < ATConf.stuckMinDistance)
                    {
                        ++s.repathAttempts;

                        char buf[192];
                        std::snprintf(buf, sizeof(buf),
                                      "Festgefahren (%.1f yd in %u ms) - Versuch %u von %u.",
                                      moved, ATConf.stuckTimeoutMs,
                                      s.repathAttempts, ATConf.maxRepathAttempts);
                        Msg(player, buf);

                        if (s.repathAttempts >= ATConf.maxRepathAttempts)
                        {
                            Finish(player, s, "Kein Fortschritt moeglich - der Autopilot gibt auf.", false);
                            return;
                        }

                        HaltMovement(player, s);
                        s.path.clear();
                        s.idx = 0;
                        s.flying = false;
                        s.state = AT_CALCULATE_PATH;
                        return;
                    }
                }
            }

            // Spline noch aktiv?
            if (!player->movespline->Finalized())
                return;

            if (s.idx < s.path.size())
            {
                LaunchChunk(player, s);
                return;
            }

            // Teilstueck zu Ende gelaufen -> von hier aus neu rechnen
            s.path.clear();
            s.idx = 0;
            s.flying = false;
            s.state = AT_CALCULATE_PATH;
            Dbg(player, s, "Teilstrecke beendet - neue Pfadberechnung.");
            return;
        }

        // -------------------------------------------------------------------
        default:
            s.state = AT_IDLE;
            return;
    }
}
