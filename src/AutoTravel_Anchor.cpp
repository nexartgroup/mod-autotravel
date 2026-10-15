/*
 * AutoTravel_Anchor.cpp
 * ---------------------------------------------------------------------------
 * Zielanker: die genaue Position eines NPCs, Objekts oder Questziels.
 *
 * Das Addon liefert nur Kartenkoordinaten. Die Etage -- Z -- steht darin nicht, und
 * wo mehrere Flaechen uebereinanderliegen, raet die Hoehensuche nach der Position des
 * SPIELERS. Die Exodar liegt rund 130 yd unter dem Gelaende darueber: wer draussen
 * steht (oder auf der falschen Etage), bekommt die Oberflaeche als Ziel, und der Weg
 * zu Ganaar fuehrt aus der Stadt hinaus.
 *
 * Ist das Ziel aber ein NPC, ein Objekt oder ein Questziel, kennt der Server seine
 * Position genau: sie steht in den Spawndaten des ObjectMgr. Dieses Modul sucht zum
 * Kartenpunkt den passenden Spawn und liefert dessen Position samt Etage.
 *
 * Woran ein Spawn als Anker gilt (Merkmale, siehe AT::AnchorFlag):
 *
 *   Dienst  NPC mit Haendler-, Lehrer-, Questgeber-, Flug-, Gasthaus-, Bank- ...
 *           Flag, oder ein nutzbares Objekt (Briefkasten, Amboss, Schmiede ...).
 *           Nur-Gespraech zaehlt nicht: Wachen und Passanten sind keine Ziele, und ein
 *           Reh auf dem Gelaende ueber der Exodar steht 1,5 yd neben einem Lehrer 150 yd
 *           darunter.
 *   Name    der englische Name in der Datenbank entspricht dem Label, das das Addon aus
 *           Carbonite mitschickt. Carbonite zeigt englische Namen, auch wenn der Client
 *           deutsch ist; deshalb wird gegen creature_template.name verglichen, nicht
 *           gegen die Namen der Lokalisierung.
 *   Quest   Ziel einer offenen Quest des Spielers (kill / interact), Kill-Credit-Kreatur
 *           dafuer, oder der Abgabe-NPC einer fertigen Quest. Das ist, worauf
 *           Carbonites Autotracking zeigt. Gegenstandsziele und Gebietsziele haben
 *           keinen eigenen Spawn und fallen weg.
 *
 * Auswahl und Etagen: AT::PickAnchor (AutoTravel_Util.cpp, ohne Core getestet). Liegt
 * kein Kandidat innerhalb seiner Toleranz am Kartenpunkt -- anderes Phasing, verschobener
 * Spawn, ein Punkt mitten im Gelaende --, gilt das alte Verfahren, als gaebe es dieses
 * Modul nicht.
 *
 * Quelle der Kandidaten sind die Spawn-Mengen des Cores je Karte und Gitter
 * (ObjectMgr::GetGridObjectGuids): sie kennen Spielereignisse, die einen Spawn ein- und
 * ausschalten (31 % der Dienst-NPCs gibt es nur waehrend eines Ereignisses). Ein
 * eigener Index waere nach dem Start veraltet; die Abfrage kostet rund zehn
 * Mikrosekunden.
 * ---------------------------------------------------------------------------
 */

#include "AutoTravel.h"

#include "Creature.h"
#include "CreatureData.h"
#include "GameObject.h"
#include "GameObjectData.h"
#include "GridDefines.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PoolMgr.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "UnitDefines.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace
{
    // NPCs, zu denen Spieler gehen wollen. GOSSIP allein fehlt mit Absicht (siehe oben),
    // ebenso SPELLCLICK und PLAYER_VEHICLE: das sind Fahrzeuge und Reittiere.
    constexpr uint32 SERVICE_NPC_FLAGS =
        UNIT_NPC_FLAG_QUESTGIVER | UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS |
        UNIT_NPC_FLAG_TRAINER_PROFESSION | UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_VENDOR_AMMO |
        UNIT_NPC_FLAG_VENDOR_FOOD | UNIT_NPC_FLAG_VENDOR_POISON | UNIT_NPC_FLAG_VENDOR_REAGENT |
        UNIT_NPC_FLAG_REPAIR | UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_SPIRITHEALER |
        UNIT_NPC_FLAG_SPIRITGUIDE | UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_BANKER |
        UNIT_NPC_FLAG_PETITIONER | UNIT_NPC_FLAG_TABARDDESIGNER | UNIT_NPC_FLAG_BATTLEMASTER |
        UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_STABLEMASTER | UNIT_NPC_FLAG_GUILD_BANKER |
        UNIT_NPC_FLAG_MAILBOX;

    // GAMEOBJECT_TYPE_SPELL_FOCUS: Amboss und Schmiede sind keine eigenen Typen, sondern
    // Spell-Focus-Objekte mit dieser focusId (Berufsziele).
    constexpr uint32 FOCUS_ANVIL = 1;
    constexpr uint32 FOCUS_FORGE = 3;

    // WAYPOINT_MOTION_TYPE (Movement/MotionMaster.h): der Spawn ist nur der Startpunkt.
    constexpr uint8 MOTION_WAYPOINT = 2;

    // Liegt das Gelaende mehr als so viel ueber dem Ziel, ist es "verdeckt": ein Gebaeude
    // oder eine Stadt unter der Erde. Dort darf die Hoehensuche nicht auf die Oberflaeche
    // ausweichen.
    constexpr float COVERED_MARGIN = 15.0f;

    // Eine Lauffleche, die hoechstens so weit vom Spawn-Z entfernt liegt, ersetzt es:
    // Kreaturen stehen leicht ueber oder unter dem Boden.
    constexpr float PLANE_SNAP = 4.0f;

    // Die Ankersuche rechnet auf dem Weltserver-Thread; so viele Spawns pruefen wir
    // hoechstens (ein Gitter hat im Mittel 61, hoechstens rund 760 Eintraege).
    constexpr size_t MAX_SPAWNS_SCANNED = 8000;

    struct QuestTargets
    {
        std::unordered_set<uint32> creatures;     // offene Ziele (Kreatur)
        std::unordered_set<uint32> objects;       // offene Ziele (Objekt)
        std::unordered_set<uint32> ready;         // Quests, die abgegeben werden koennen
    };

    // Was der Spieler gerade an Questzielen hat. Rein aus dem Questlog: Carbonite
    // schickt keine Quest-ID mit, und der Server braucht sie auch nicht.
    void CollectQuestTargets(Player* player, QuestTargets& out)
    {
        for (auto const& kv : player->getQuestStatusMap())
        {
            Quest const* q = sObjectMgr->GetQuestTemplate(kv.first);
            if (!q)
                continue;

            QuestStatusData const& st = kv.second;
            if (st.Status == QUEST_STATUS_COMPLETE)
            {
                out.ready.insert(kv.first);
                continue;
            }
            if (st.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                int32 const req = q->RequiredNpcOrGo[i];
                if (!req)
                    continue;
                if (st.CreatureOrGOCount[i] >= q->RequiredNpcOrGoCount[i])
                    continue;                       // dieses Ziel ist schon erfuellt

                if (req > 0)
                    out.creatures.insert(uint32(req));
                else
                    out.objects.insert(uint32(-req));
            }
        }
    }

    bool EndsReadyQuest(QuestRelationBounds const& b, std::unordered_set<uint32> const& ready)
    {
        for (auto it = b.first; it != b.second; ++it)
            if (ready.count(it->second))
                return true;
        return false;
    }

    // Beschreibt einen Kandidaten fuer ".at diag".
    struct CandInfo
    {
        std::string name;
        uint32 entry = 0;
        uint64 spawnId = 0;
        bool object = false;
    };

    // Hoechstens ein Eintrag je Gitter-Id, aufsteigend.
    void GridIdsAround(float x, float y, float radius, std::vector<uint32>& out)
    {
        out.clear();
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                out.push_back(Acore::ComputeGridCoord(x + sx * radius, y + sy * radius).GetId());
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
}

bool AutoTravelMgr::FindAnchor(Player* player, uint32 mapId, float x, float y,
                               std::string const& label, ATAnchor& out,
                               std::vector<std::string>* report) const
{
    out = ATAnchor();

    auto note = [&](std::string const& line)
    {
        if (report)
            report->push_back(line);
    };

    if (!player || !ATConf.useTargetAnchor)
    {
        note("Anker: aus (AutoTravel.UseTargetAnchor = 0)");
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;

    Map* map = player->GetMap();
    bool const sameMap = map && map->GetId() == mapId;

    // Die Karte des Ziels hat dieselben Spawn-Mengen wie jede andere; nur Phase,
    // Spawngruppen und Pools lassen sich auf einer fremden Karte nicht pruefen.
    uint8 const spawnMode = sameMap ? map->GetSpawnMode() : 0;

    AT::AnchorRules rules;
    rules.radiusInteractive = ATConf.anchorRadius;
    rules.radiusName        = ATConf.anchorNameRadius;
    rules.radiusQuest       = ATConf.anchorQuestRadius;
    rules.floorTolerance    = ATConf.anchorFloorTolerance;

    float const maxRadius = std::max({ rules.radiusInteractive, rules.radiusName, rules.radiusQuest });
    if (maxRadius <= 0.0f)
        return false;

    QuestTargets quest;
    CollectQuestTargets(player, quest);

    std::vector<uint32> grids;
    GridIdsAround(x, y, maxRadius, grids);

    uint32 const phase = player->GetPhaseMask();

    std::vector<AT::AnchorCand> cands;
    std::vector<CandInfo> infos;
    size_t scanned = 0, inRadius = 0, dropped = 0;

    auto consider = [&](AT::AnchorCand const& c, CandInfo const& info)
    {
        cands.push_back(c);
        infos.push_back(info);
    };

    for (uint32 gridId : grids)
    {
        CellObjectGuids const& guids = sObjectMgr->GetGridObjectGuids(uint16(mapId), spawnMode, gridId);

        // --- Kreaturen -----------------------------------------------------
        for (ObjectGuid::LowType const& guid : guids.creatures)
        {
            if (++scanned > MAX_SPAWNS_SCANNED)
                break;

            CreatureData const* d = sObjectMgr->GetCreatureData(guid);
            if (!d || !d->dbData || d->mapid != mapId)
                continue;

            float const px = d->posX, py = d->posY, pz = d->posZ;
            if (AT::Dist2D(px, py, x, y) > maxRadius)
                continue;
            ++inRadius;

            if (!(d->phaseMask & phase))
            {
                ++dropped;
                continue;
            }
            if (sameMap)
            {
                if (!map->IsSpawnGroupActive(d->spawnGroupId)
                    || (d->poolId && !map->GetPoolData().IsSpawnedObject<Creature>(uint32(guid))))
                {
                    ++dropped;
                    continue;
                }
            }

            uint32 flags = 0;
            std::string name;
            uint32 mainEntry = 0;
            uint32 const entries[3] = { d->id, d->id2, d->id3 };
            for (uint32 e : entries)
            {
                if (!e)
                    continue;
                CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(e);
                if (!t)
                    continue;
                if (t->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER)
                    continue;
                if (t->Name.compare(0, 5, "[DND]") == 0)
                    continue;

                if (!mainEntry)
                {
                    mainEntry = e;
                    name = t->Name;
                }

                uint32 npcflag = 0, unitFlags = 0, dynFlags = 0;
                ObjectMgr::ChooseCreatureFlags(t, npcflag, unitFlags, dynFlags, d);
                if (npcflag & SERVICE_NPC_FLAGS)
                    flags |= AT::ANCHOR_INTERACTIVE;

                if (!label.empty() && (AT::LabelMatchesName(label, t->Name)
                                       || (t->SubName.size() >= 6 && AT::LabelMatchesName(label, t->SubName))))
                    flags |= AT::ANCHOR_NAME;

                // Ziel einer offenen Quest (auch ueber einen Kill-Credit-Eintrag), oder der
                // Abgabe-NPC einer fertigen Quest.
                if (quest.creatures.count(e)
                    || (t->KillCredit[0] && quest.creatures.count(t->KillCredit[0]))
                    || (t->KillCredit[1] && quest.creatures.count(t->KillCredit[1]))
                    || (!quest.ready.empty()
                        && EndsReadyQuest(sObjectMgr->GetCreatureQuestInvolvedRelationBounds(e), quest.ready)))
                    flags |= AT::ANCHOR_QUEST;
            }

            if (!(flags & (AT::ANCHOR_INTERACTIVE | AT::ANCHOR_NAME | AT::ANCHOR_QUEST)))
                continue;
            if (d->movementType == MOTION_WAYPOINT)
                flags |= AT::ANCHOR_MOVER;

            AT::AnchorCand c;
            c.spawnId = guid;
            c.entry = mainEntry;
            c.flags = flags;
            c.x = px;
            c.y = py;
            c.z = pz;
            CandInfo info;
            info.name = name;
            info.entry = mainEntry;
            info.spawnId = guid;
            consider(c, info);
        }

        // --- Objekte -------------------------------------------------------
        for (ObjectGuid::LowType const& guid : guids.gameobjects)
        {
            if (++scanned > MAX_SPAWNS_SCANNED)
                break;

            GameObjectData const* d = sObjectMgr->GetGameObjectData(guid);
            if (!d || !d->dbData || d->mapid != mapId)
                continue;

            float const px = d->posX, py = d->posY, pz = d->posZ;
            if (AT::Dist2D(px, py, x, y) > maxRadius)
                continue;
            ++inRadius;

            if (!(d->phaseMask & phase))
            {
                ++dropped;
                continue;
            }
            if (sameMap)
            {
                if (!map->IsSpawnGroupActive(d->spawnGroupId)
                    || (d->poolId && !map->GetPoolData().IsSpawnedObject<GameObject>(uint32(guid))))
                {
                    ++dropped;
                    continue;
                }
            }

            GameObjectTemplate const* t = sObjectMgr->GetGameObjectTemplate(d->id);
            if (!t)
                continue;

            uint32 flags = AT::ANCHOR_OBJECT;
            bool const useful = (t->type == GAMEOBJECT_TYPE_QUESTGIVER
                                 || t->type == GAMEOBJECT_TYPE_MAILBOX
                                 || t->type == GAMEOBJECT_TYPE_MEETINGSTONE
                                 || t->type == GAMEOBJECT_TYPE_GUILD_BANK
                                 || (t->type == GAMEOBJECT_TYPE_SPELL_FOCUS
                                     && (t->spellFocus.focusId == FOCUS_ANVIL
                                         || t->spellFocus.focusId == FOCUS_FORGE)));
            if (useful && ATConf.anchorObjects)
                flags |= AT::ANCHOR_INTERACTIVE;

            if (!label.empty() && AT::LabelMatchesName(label, t->name))
                flags |= AT::ANCHOR_NAME;

            if (quest.objects.count(d->id)
                || (!quest.ready.empty()
                    && EndsReadyQuest(sObjectMgr->GetGOQuestInvolvedRelationBounds(d->id), quest.ready)))
                flags |= AT::ANCHOR_QUEST;

            if (!(flags & (AT::ANCHOR_INTERACTIVE | AT::ANCHOR_NAME | AT::ANCHOR_QUEST)))
                continue;

            AT::AnchorCand c;
            c.spawnId = guid;
            c.entry = d->id;
            c.flags = flags;
            c.x = px;
            c.y = py;
            c.z = pz;
            CandInfo info;
            info.name = t->name;
            info.entry = d->id;
            info.spawnId = guid;
            info.object = true;
            consider(c, info);
        }
    }

    char b[320];
    std::snprintf(b, sizeof(b),
                  "Anker-Suche: %u Spawns in %u Gitter(n) geprueft, %u im Radius %.0f yd, %u wegen Phase/Gruppe/Pool verworfen, "
                  "%u Kandidaten | Label \"%s\" | offene Questziele: %u Kreaturen, %u Objekte, abgabebereit: %u",
                  uint32(scanned), uint32(grids.size()), uint32(inRadius), maxRadius, uint32(dropped),
                  uint32(cands.size()), AT::SanitizeText(label, 60).c_str(),
                  uint32(quest.creatures.size()), uint32(quest.objects.size()), uint32(quest.ready.size()));
    note(b);

    AT::AnchorPick pick;
    if (!AT::PickAnchor(cands, x, y, rules, pick))
    {
        note("Anker: keiner -- kein NPC, Objekt oder Questziel nahe genug am Kartenpunkt; "
             "es gilt die Hoehensuche nach der Spielerposition.");
        return false;
    }

    AT::AnchorCand const& best = cands[pick.index];

    out.info.found = true;
    out.info.source = best.flags;
    out.info.entry = best.entry;
    out.info.dist2D = pick.dist2D;
    out.info.floors = pick.floors;
    out.info.name = infos[pick.index].name;
    out.isObject = infos[pick.index].object;
    out.x = best.x;
    out.y = best.y;
    out.z = best.z;

    // x/y auf die Spawnposition setzen: der PathGenerator findet ein Ziel nur bis 3 yd
    // seitlich, und der Radius der Ankunft soll sich auf den NPC beziehen. Ein Questziel
    // ohne Dienst (ein Tier, das getoetet wird) bleibt dagegen, wo der Kartenpunkt ist --
    // der Spawn ist dort nur eines von vielen, und er laeuft herum.
    out.info.snappedXY = ATConf.anchorSnapXY
        && (best.flags & (AT::ANCHOR_INTERACTIVE | AT::ANCHOR_NAME)) != 0;

    // Gleichwertige andere Etagen (Rueckfall, wenn zur ersten kein Weg fuehrt). Sie tragen
    // ihre eigene Spawnposition: ein Punkt ueber dem Hauptanker muss auf der anderen Etage
    // nicht begehbar sein (Ruestungsanbau der Exodar: 9 yd zwischen den beiden Haendlern).
    float altX[2] = { 0.0f, 0.0f };
    float altY[2] = { 0.0f, 0.0f };
    float altZ[2] = { 0.0f, 0.0f };
    uint32 altCount = pick.altCount;
    for (uint32 i = 0; i < altCount; ++i)
    {
        altX[i] = cands[pick.alt[i]].x;
        altY[i] = cands[pick.alt[i]].y;
        altZ[i] = cands[pick.alt[i]].z;
    }

    // Z auf eine begehbare Flaeche legen und feststellen, ob das Ziel verdeckt ist.
    // Beides geht nur auf der Karte des Spielers (nur dort ist das Gelaende geladen).
    if (sameMap)
    {
        auto snapToPlane = [&](float px, float py, float z)
        {
            std::vector<float> planes;
            FindGroundPlanes(player, px, py, z, planes);
            float result = z;
            float bestDiff = PLANE_SNAP + 0.001f;
            for (float p : planes)
            {
                float const diff = std::fabs(p - z);
                if (diff < bestDiff)
                {
                    bestDiff = diff;
                    result = p;
                }
            }
            return result;
        };

        out.z = snapToPlane(out.x, out.y, out.z);
        for (uint32 i = 0; i < altCount; ++i)
            altZ[i] = snapToPlane(altX[i], altY[i], altZ[i]);

        float const terrain = map->GetHeight(phase, out.x, out.y, MAX_HEIGHT);
        out.info.covered = terrain > INVALID_HEIGHT && (terrain - out.z) > COVERED_MARGIN;
    }

    out.info.z = out.z;
    out.info.altCount = altCount;
    for (uint32 i = 0; i < altCount; ++i)
    {
        out.info.altX[i] = altX[i];
        out.info.altY[i] = altY[i];
        out.info.altZ[i] = altZ[i];
    }

    if (report)
    {
        char l[352];
        std::snprintf(l, sizeof(l),
                      "Anker: %s (Eintrag %u, Spawn %llu) | Belege: %s | %.1f yd vom Kartenpunkt, Z %.2f%s%s | Etagen im Radius: %u",
                      out.info.name.c_str(), out.info.entry, (unsigned long long)infos[pick.index].spawnId,
                      AT::AnchorFlagsText(best.flags).c_str(), pick.dist2D, out.z,
                      out.info.snappedXY ? ", X/Y uebernommen" : "",
                      out.info.covered ? ", verdeckt (Gelaende darueber)" : "", pick.floors);
        note(l);

        for (uint32 i = 0; i < altCount; ++i)
        {
            std::snprintf(l, sizeof(l), "  gleichwertige andere Etage: %s, Z %.2f",
                          infos[pick.alt[i]].name.c_str(), altZ[i]);
            note(l);
        }

        // Die naechsten Kandidaten, der Naehe nach.
        std::vector<size_t> order(cands.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t c)
        {
            return AT::Dist2D(cands[a].x, cands[a].y, x, y) < AT::Dist2D(cands[c].x, cands[c].y, x, y);
        });
        for (size_t k = 0; k < order.size() && k < 6; ++k)
        {
            AT::AnchorCand const& c = cands[order[k]];
            std::snprintf(l, sizeof(l), "  Kandidat: %s%s (%u) | %.1f yd, Z %.1f | %s",
                          infos[order[k]].name.c_str(), order[k] == pick.index ? " <== gewaehlt" : "",
                          c.entry, AT::Dist2D(c.x, c.y, x, y), c.z, AT::AnchorFlagsText(c.flags).c_str());
            note(l);
        }
    }

    return true;
}

bool AutoTravelMgr::ArrivedVertically(Player* player, ATSession const& s) const
{
    if (!s.anchor.found)
        return true;

    // Wer fliegt oder schwimmt, kommt nicht "auf einer Etage" an.
    if (s.flying || player->IsFlying() || player->IsInWater())
        return true;

    if (std::fabs(player->GetPositionZ() - s.destZ) <= ATConf.anchorArrivalZ)
        return true;

    // Nach zwei Teilstuecken, die am Ziel auf der falschen Etage endeten, gilt das Ziel
    // als erreicht: ein NPC auf einem Podest, zu dem das NavMesh keinen Weg kennt, soll
    // die Reise nicht endlos beschaeftigen.
    return s.anchorZRefusals >= 2;
}
