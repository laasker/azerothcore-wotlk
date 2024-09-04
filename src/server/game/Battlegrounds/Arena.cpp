/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Arena.h"
#include "ArenaTeamMgr.h"
#include "GroupMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"
//#include "WorldStatePackets.h"

void ArenaScore::AppendToPacket(WorldPacket& data)
{
    data << PlayerGuid;

    data << uint32(KillingBlows);
    data << uint8(PvPTeamId);
    data << uint32(DamageDone);
    data << uint32(HealingDone);

    BuildObjectivesBlock(data);
}

void ArenaScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(0); // Objectives Count
}

void ArenaTeamScore::BuildRatingInfoBlock(WorldPacket& data)
{
    uint32 ratingLost = std::abs(std::min(RatingChange, 0));
    uint32 ratingWon = std::max(RatingChange, 0);

    // should be old rating, new rating, and client will calculate rating change itself
    data << uint32(ratingLost);
    data << uint32(ratingWon);
    data << uint32(MatchmakerRating);
}

void ArenaTeamScore::BuildTeamInfoBlock(WorldPacket& data)
{
    data << TeamName;
}

Arena::Arena()
{
    StartDelayTimes[BG_STARTING_EVENT_FIRST]  = BG_START_DELAY_1M;
    StartDelayTimes[BG_STARTING_EVENT_SECOND] = BG_START_DELAY_30S;
    StartDelayTimes[BG_STARTING_EVENT_THIRD]  = BG_START_DELAY_15S;
    StartDelayTimes[BG_STARTING_EVENT_FOURTH] = BG_START_DELAY_NONE;

    StartMessageIds[BG_STARTING_EVENT_FIRST]  = ARENA_TEXT_START_ONE_MINUTE;
    StartMessageIds[BG_STARTING_EVENT_SECOND] = ARENA_TEXT_START_THIRTY_SECONDS;
    StartMessageIds[BG_STARTING_EVENT_THIRD]  = ARENA_TEXT_START_FIFTEEN_SECONDS;
    StartMessageIds[BG_STARTING_EVENT_FOURTH] = ARENA_TEXT_START_BATTLE_HAS_BEGUN;
}

void Arena::AddPlayer(Player* player)
{
    Battleground::AddPlayer(player);
    PlayerScores.emplace(player->GetGUID().GetCounter(), new ArenaScore(player->GetGUID(), player->GetBgTeamId()));

    if (player->GetBgTeamId() == TEAM_ALLIANCE) // gold
    {
        if (player->GetTeamId() == TEAM_HORDE)
            player->CastSpell(player, SPELL_HORDE_GOLD_FLAG, true);
        else
            player->CastSpell(player, SPELL_ALLIANCE_GOLD_FLAG, true);
    }
    else // green
    {
        if (player->GetTeamId() == TEAM_HORDE)
            player->CastSpell(player, SPELL_HORDE_GREEN_FLAG, true);
        else
            player->CastSpell(player, SPELL_ALLIANCE_GREEN_FLAG, true);
    }

    UpdateArenaWorldState();

    Group* group = player->GetGroup();
    if (group)
    {
        // Hackfix for crossfaction arenas, recreate group when joining
        // Without this, players in a crossfaction arena group would not be able to cast beneficial spells on their teammates

        std::vector<Player*> members;
        bool isCrossfaction = false;
        for (Group::member_citerator mitr = group->GetMemberSlots().begin(); mitr != group->GetMemberSlots().end(); ++mitr)
        {
            Player* member = ObjectAccessor::FindPlayer(mitr->guid);
            if (!member || member->GetGUID() == player->GetGUID())
            {
                continue;
            }
            members.push_back(member);
            if (member->GetTeamId(true) != player->GetTeamId(true))
            {
                isCrossfaction = true;
            }
        }

        if (isCrossfaction)
        {
            for (Player* member : members)
            {
                member->RemoveFromGroup();
            }
            group->Disband();

            group = new Group();
            SetBgRaid(player->GetBgTeamId(), group);
            group->Create(player);
            sGroupMgr->AddGroup(group);
            for (Player* member : members)
            {
                group->AddMember(member);
            }
        }
    }
}

void Arena::RemovePlayer(Player* /*player*/)
{
    if (GetStatus() == STATUS_WAIT_LEAVE)
        return;

    UpdateArenaWorldState();
    CheckWinConditions();
}

void Arena::FillInitialWorldStates(WorldPacket& data)
{
    data << uint32(ARENA_WORLD_STATE_ALIVE_PLAYERS_GREEN) << uint32(GetAlivePlayersCountByTeam(TEAM_HORDE));
    data << uint32(ARENA_WORLD_STATE_ALIVE_PLAYERS_GOLD) << uint32(GetAlivePlayersCountByTeam(TEAM_ALLIANCE));
}

void Arena::UpdateArenaWorldState()
{
    UpdateWorldState(ARENA_WORLD_STATE_ALIVE_PLAYERS_GREEN, GetAlivePlayersCountByTeam(TEAM_HORDE));
    UpdateWorldState(ARENA_WORLD_STATE_ALIVE_PLAYERS_GOLD, GetAlivePlayersCountByTeam(TEAM_ALLIANCE));
}

void Arena::HandleKillPlayer(Player* player, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    Battleground::HandleKillPlayer(player, killer);

    UpdateArenaWorldState();
    CheckWinConditions();
}

void Arena::RemovePlayerAtLeave(Player* player)
{
    if (isRated() && GetStatus() == STATUS_IN_PROGRESS)
    {
        if (auto const& member = Acore::Containers::MapGetValuePtr(m_Players, player->GetGUID()))
        {
            // if the player was a match participant, calculate rating
            auto teamId = member->GetBgTeamId();

            ArenaTeam* winnerArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(GetOtherTeamId(teamId)));
            ArenaTeam* loserArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(teamId));

            if (winnerArenaTeam && loserArenaTeam && winnerArenaTeam != loserArenaTeam)
                loserArenaTeam->MemberLost(player, GetArenaMatchmakerRating(GetOtherTeamId(teamId)));
        }
    }

    // remove player
    Battleground::RemovePlayerAtLeave(player);
}

void Arena::CheckWinConditions()
{
    if (!sScriptMgr->OnBeforeArenaCheckWinConditions(this))
        return;

    if (!GetAlivePlayersCountByTeam(TEAM_ALLIANCE) && GetPlayersCountByTeam(TEAM_HORDE))
        EndBattleground(TEAM_HORDE);
    else if (GetPlayersCountByTeam(TEAM_ALLIANCE) && !GetAlivePlayersCountByTeam(TEAM_HORDE))
        EndBattleground(TEAM_ALLIANCE);
}

void Arena::EndBattleground(TeamId winnerTeamId)
{
    //if (GetArenaType() == 4)
    //{
    //    EndSoloBattleground(winnerTeamId);
    //    return;
    //}

    if (isRated())
    {
        uint32 startDelay = GetStartDelayTime();
        bool bValidArena = GetStatus() == STATUS_IN_PROGRESS && GetStartTime() >= startDelay + 15000; // pussywizard: only if arena lasted at least 15 secs

        uint32 loserTeamRating = 0;
        uint32 loserMatchmakerRating = 0;
        int32  loserChange = 0;
        int32  loserMatchmakerChange = 0;
        uint32 winnerTeamRating = 0;
        uint32 winnerMatchmakerRating = 0;
        int32  winnerChange = 0;
        int32  winnerMatchmakerChange = 0;

        ArenaTeam* winnerArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(winnerTeamId == TEAM_NEUTRAL ? TEAM_HORDE : winnerTeamId));
        ArenaTeam* loserArenaTeam  = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(winnerTeamId == TEAM_NEUTRAL ? TEAM_ALLIANCE : GetOtherTeamId(winnerTeamId)));

        auto SaveArenaLogs = [&]()
        {
            // pussywizard: arena logs in database
            uint32 fightId = sArenaTeamMgr->GetNextArenaLogId();
            uint32 currOnline = sWorld->GetActiveSessionCount();

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            CharacterDatabasePreparedStatement* stmt2 = CharacterDatabase.GetPreparedStatement(CHAR_INS_ARENA_LOG_FIGHT);
            stmt2->SetData(0, fightId);
            stmt2->SetData(1, GetArenaType());
            stmt2->SetData(2, ((GetStartTime() <= startDelay ? 0 : GetStartTime() - startDelay) / 1000));
            stmt2->SetData(3, winnerArenaTeam->GetId());
            stmt2->SetData(4, loserArenaTeam->GetId());
            stmt2->SetData(5, (uint16)winnerTeamRating);
            stmt2->SetData(6, (uint16)winnerMatchmakerRating);
            stmt2->SetData(7, (int16)winnerChange);
            stmt2->SetData(8, (uint16)loserTeamRating);
            stmt2->SetData(9, (uint16)loserMatchmakerRating);
            stmt2->SetData(10, (int16)loserChange);
            stmt2->SetData(11, currOnline);
            trans->Append(stmt2);

            uint8 memberId = 0;
            for (auto const& [playerGuid, arenaLogEntryData] : ArenaLogEntries)
            {
                auto const& score = PlayerScores.find(playerGuid.GetCounter());
                stmt2 = CharacterDatabase.GetPreparedStatement(CHAR_INS_ARENA_LOG_MEMBERSTATS);
                stmt2->SetData(0, fightId);
                stmt2->SetData(1, ++memberId);
                stmt2->SetData(2, arenaLogEntryData.Name);
                stmt2->SetData(3, arenaLogEntryData.Guid);
                stmt2->SetData(4, arenaLogEntryData.ArenaTeamId);
                stmt2->SetData(5, arenaLogEntryData.Acc);
                stmt2->SetData(6, arenaLogEntryData.IP);
                if (score != PlayerScores.end())
                {
                    stmt2->SetData(7, score->second->GetDamageDone());
                    stmt2->SetData(8, score->second->GetHealingDone());
                    stmt2->SetData(9, score->second->GetKillingBlows());
                }
                else
                {
                    stmt2->SetData(7, 0);
                    stmt2->SetData(8, 0);
                    stmt2->SetData(9, 0);
                }
                trans->Append(stmt2);
            }

            CharacterDatabase.CommitTransaction(trans);
        };

        if (winnerArenaTeam && loserArenaTeam && winnerArenaTeam != loserArenaTeam)
        {
            loserTeamRating = loserArenaTeam->GetRating();
            loserMatchmakerRating = GetArenaMatchmakerRating(GetOtherTeamId(winnerTeamId));
            winnerTeamRating = winnerArenaTeam->GetRating();
            winnerMatchmakerRating = GetArenaMatchmakerRating(winnerTeamId);

            if (winnerTeamId != TEAM_NEUTRAL)
            {
                winnerMatchmakerChange = bValidArena ? winnerArenaTeam->WonAgainst(winnerMatchmakerRating, loserMatchmakerRating, winnerChange, GetBgMap()) : 0;
                loserMatchmakerChange = loserArenaTeam->LostAgainst(loserMatchmakerRating, winnerMatchmakerRating, loserChange, GetBgMap());

                sScriptMgr->OnAfterArenaRatingCalculation(this, winnerMatchmakerChange, loserMatchmakerChange, winnerChange, loserChange);

                LOG_DEBUG("bg.arena", "match Type: {} --- Winner: old rating: {}, rating gain: {}, old MMR: {}, MMR gain: {} --- Loser: old rating: {}, rating loss: {}, old MMR: {}, MMR loss: {} ---",
                    GetArenaType(), winnerTeamRating, winnerChange, winnerMatchmakerRating, winnerMatchmakerChange,
                    loserTeamRating, loserChange, loserMatchmakerRating, loserMatchmakerChange);

                SetArenaMatchmakerRating(winnerTeamId, winnerMatchmakerRating + winnerMatchmakerChange);
                SetArenaMatchmakerRating(GetOtherTeamId(winnerTeamId), loserMatchmakerRating + loserMatchmakerChange);

                // bg team that the client expects is different to TeamId
                // alliance 1, horde 0
                uint8 winnerTeam = winnerTeamId == TEAM_ALLIANCE ? PVP_TEAM_ALLIANCE : PVP_TEAM_HORDE;
                uint8 loserTeam = winnerTeamId == TEAM_ALLIANCE ? PVP_TEAM_HORDE : PVP_TEAM_ALLIANCE;

                _arenaTeamScores[winnerTeam].Assign(winnerChange, winnerMatchmakerRating, winnerArenaTeam->GetName());
                _arenaTeamScores[loserTeam].Assign(loserChange, loserMatchmakerRating, loserArenaTeam->GetName());

                LOG_DEBUG("bg.arena", "Arena match Type: {} for Team1Id: {} - Team2Id: {} ended. WinnerTeamId: {}. Winner rating: +{}, Loser rating: {}",
                    GetArenaType(), GetArenaTeamIdForTeam(TEAM_ALLIANCE), GetArenaTeamIdForTeam(TEAM_HORDE), winnerArenaTeam->GetId(), winnerChange, loserChange);
            }
            else // Deduct 16 points from each teams arena-rating if there are no winners after 45+2 minutes
            {
                // pussywizard: in case of a draw, the following is always true:
                // winnerArenaTeam => TEAM_HORDE, loserArenaTeam => TEAM_ALLIANCE

                winnerTeamRating = winnerArenaTeam->GetRating();
                winnerMatchmakerRating = GetArenaMatchmakerRating(TEAM_HORDE);
                loserTeamRating = loserArenaTeam->GetRating();
                loserMatchmakerRating = GetArenaMatchmakerRating(TEAM_ALLIANCE);
                winnerMatchmakerChange = 0;
                loserMatchmakerChange = 0;
                winnerChange = ARENA_TIMELIMIT_POINTS_LOSS;
                loserChange = ARENA_TIMELIMIT_POINTS_LOSS;

                _arenaTeamScores[PVP_TEAM_ALLIANCE].Assign(ARENA_TIMELIMIT_POINTS_LOSS, winnerMatchmakerRating, winnerArenaTeam->GetName());
                _arenaTeamScores[PVP_TEAM_HORDE].Assign(ARENA_TIMELIMIT_POINTS_LOSS, loserMatchmakerRating, loserArenaTeam->GetName());

                winnerArenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS, GetBgMap());
                loserArenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS, GetBgMap());
            }
        }

        SaveArenaLogs();

        uint8 aliveWinners = GetAlivePlayersCountByTeam(winnerTeamId);

        for (auto const& [playerGuid, player] : GetPlayers())
        {
            auto const& bgTeamId = player->GetBgTeamId();

            // per player calculation
            if (winnerArenaTeam && loserArenaTeam && winnerArenaTeam != loserArenaTeam)
            {
                if (bgTeamId == winnerTeamId)
                {
                    if (bValidArena)
                    {
                        // update achievement BEFORE personal rating update
                        uint32 rating = player->GetArenaPersonalRating(winnerArenaTeam->GetSlot());
                        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_RATED_ARENA, rating ? rating : 1);
                        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_ARENA, GetMapId());

                        // Last standing - Rated 5v5 arena & be solely alive player
                        if (GetArenaType() == ARENA_TYPE_5v5 && aliveWinners == 1 && player->IsAlive())
                        {
                            player->CastSpell(player, SPELL_LAST_MAN_STANDING, true);
                        }

                        winnerArenaTeam->MemberWon(player, loserMatchmakerRating, winnerMatchmakerChange);
                    }
                }
                else
                {
                    loserArenaTeam->MemberLost(player, winnerMatchmakerRating, loserMatchmakerChange);

                    // Arena lost => reset the win_rated_arena having the "no_lose" condition
                    player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_NO_LOSE, 0);
                }

                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_PLAY_ARENA, GetMapId());
            }
        }

        // update previous opponents for arena queue
        winnerArenaTeam->SetPreviousOpponents(loserArenaTeam->GetId());
        loserArenaTeam->SetPreviousOpponents(winnerArenaTeam->GetId());

        // save the stat changes
        if (bValidArena)
        {
            winnerArenaTeam->SaveToDB();
            winnerArenaTeam->NotifyStatsChanged();
        }

        loserArenaTeam->SaveToDB();
        loserArenaTeam->NotifyStatsChanged();
    }

    // end battleground
    Battleground::EndBattleground(winnerTeamId);
}

//
//
//
//

void Arena::EndSoloBattleground(TeamId winnerTeamId)
{
    if (isRated())
    {
        LOG_ERROR("solo3v3", "EndSoloBattleground called");

        uint32 startDelay = GetStartDelayTime();
        bool bValidArena = GetStatus() == STATUS_IN_PROGRESS && GetStartTime() >= startDelay + 15000; // pussywizard: only if arena lasted at least 15 secs

        uint32 loserTeamRating = 0;
        uint32 loserMatchmakerRating = 0;
        int32  loserChange = 0;
        int32  loserMatchmakerChange = 0;
        uint32 winnerTeamRating = 0;
        uint32 winnerMatchmakerRating = 0;
        int32  winnerChange = 0;
        int32  winnerMatchmakerChange = 0;

        // SOLOQUEUE - Store ratings
        uint32 sq_loser_rating = 0;
        uint32 sq_winner_rating = 0;

        ArenaTeam* winnerArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(winnerTeamId == TEAM_NEUTRAL ? TEAM_HORDE : winnerTeamId));
        ArenaTeam* loserArenaTeam = sArenaTeamMgr->GetArenaTeamById(GetArenaTeamIdForTeam(winnerTeamId == TEAM_NEUTRAL ? TEAM_ALLIANCE : GetOtherTeamId(winnerTeamId)));

        /*
        ArenaTeam* winnerArenaTeam = nullptr;
        ArenaTeam* loserArenaTeam = nullptr;
        for (auto const& [playerGuid, player] : GetPlayers())
        {
            auto const& bgTeamId = player->GetBgTeamId();

            if (bgTeamId == winnerTeamId)
            {
                ArenaTeam* winnerArenaTeam = sArenaTeamMgr->GetArenaTeamByCaptain(playerGuid, 4);
                LOG_ERROR("solo3v3", "EndSoloBattleground player is in the winning team with ArenaTeamID = {}", winnerArenaTeam->GetId());
            }
            else
            {
                ArenaTeam* loserArenaTeam = sArenaTeamMgr->GetArenaTeamByCaptain(playerGuid, 4);
                LOG_ERROR("solo3v3", "EndSoloBattleground player is in the losing team with ArenaTeamID = {}", loserArenaTeam->GetId());
            }
        }*/

        // SOLOQUEUE
        if (isArena() && isRated() && GetArenaType() == 4)
        {
            if (winnerTeamId == TEAM_ALLIANCE)
            {
                sq_winner_rating = (GetSoloQueueRatingForTeam(TEAM_ALLIANCE) / 3);
                sq_loser_rating = (GetSoloQueueRatingForTeam(TEAM_HORDE) / 3);
                LOG_ERROR("solo3v3", "--- Winner rating: %u. Loser rating: %u ---", sq_winner_rating, sq_loser_rating);
            }
            else if (winnerTeamId == TEAM_HORDE)
            {
                sq_winner_rating = (GetSoloQueueRatingForTeam(TEAM_HORDE) / 3);
                sq_loser_rating = (GetSoloQueueRatingForTeam(TEAM_ALLIANCE) / 3);
                LOG_ERROR("solo3v3", "--- Winner rating: %u. Loser rating: %u ---", sq_winner_rating, sq_loser_rating);
            }
        }



        /*
        for (auto const& [playerGuid, player] : GetPlayers())
        {
            auto const& bgTeamId = player->GetBgTeamId();

            if (bgTeamId == winnerTeamId)
            {
                if (ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(Player::GetArenaTeamIdFromDB(itr.first, ARENA_TEAM_5v5)))
                    winnerTeamRating += ceil(team->GetRating() / 3);
            }
            else
            {
                if (ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(Player::GetArenaTeamIdFromDB(itr.first, ARENA_TEAM_5v5)))
                    loserTeamRating += ceil(team->GetRating() / 3);
            }
        }
        
        for (auto& itr : PlayerScores)
        {
            uint32 playerTeamId = 0;

            if (itr.second->bg == winnerTeamId)
            {
                playerTeamId = sPlayerMgr->GetArenaTeamId(itr.first, ARENA_TEAM_5v5); // Obter o ArenaTeamId do jogador do time vencedor
            }
            else
            {
                playerTeamId = sPlayerMgr->GetArenaTeamId(itr.first, ARENA_TEAM_5v5); // Obter o ArenaTeamId do jogador do time perdedor
            }

            if (GetBgTeamId == winnerTeamId)
            {
                if (ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(Player::GetArenaTeamIdFromDB(itr.first, ARENA_TEAM_5v5)))
                    winnerTeamRating += ceil(team->GetRating() / 3);
            }
            else
            {
                if (ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(Player::GetArenaTeamIdFromDB(itr.first, ARENA_TEAM_5v5)))
                    loserTeamRating += ceil(team->GetRating() / 3);
            }

            if (playerTeamId < 0xFFF00000)
            {
                ArenaTeam* playerArenaTeam = sArenaTeamMgr->GetArenaTeamById(playerTeamId);
                if (playerArenaTeam)
                {
                    if (itr.second->BgTeam == winnerTeamId)
                    {
                        // Lidar com o jogador do time vencedor
                        winnerTeamRating += ceil(playerArenaTeam->GetRating() / 3);
                    }
                    else
                    {
                        // Lidar com o jogador do time perdedor
                        loserTeamRating += ceil(playerArenaTeam->GetRating() / 3);
                    }
                }
            }
        }
        */


        auto SaveArenaLogs = [&]()
        {
                // pussywizard: arena logs in database
                uint32 fightId = sArenaTeamMgr->GetNextArenaLogId();
                uint32 currOnline = sWorld->GetActiveSessionCount();

                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                CharacterDatabasePreparedStatement* stmt2 = CharacterDatabase.GetPreparedStatement(CHAR_INS_ARENA_LOG_FIGHT); // log_arena_fights (fight id, time, type, duration, winner, loser, winner tr etc)
                stmt2->SetData(0, fightId);
                stmt2->SetData(1, GetArenaType());
                stmt2->SetData(2, ((GetStartTime() <= startDelay ? 0 : GetStartTime() - startDelay) / 1000));
                stmt2->SetData(3, winnerArenaTeam->GetId());
                stmt2->SetData(4, loserArenaTeam->GetId());
                stmt2->SetData(5, (uint16)winnerTeamRating);
                stmt2->SetData(6, (uint16)winnerMatchmakerRating);
                stmt2->SetData(7, (int16)winnerChange);
                stmt2->SetData(8, (uint16)loserTeamRating);
                stmt2->SetData(9, (uint16)loserMatchmakerRating);
                stmt2->SetData(10, (int16)loserChange);
                stmt2->SetData(11, currOnline);
                trans->Append(stmt2);

                uint8 memberId = 0;
                for (auto const& [playerGuid, arenaLogEntryData] : ArenaLogEntries)
                {
                    auto const& score = PlayerScores.find(playerGuid.GetCounter());
                    stmt2 = CharacterDatabase.GetPreparedStatement(CHAR_INS_ARENA_LOG_MEMBERSTATS); // log_arena_memberstats (fight id, member id, name, guid, team, account, damage, heal)
                    stmt2->SetData(0, fightId);
                    stmt2->SetData(1, ++memberId);
                    stmt2->SetData(2, arenaLogEntryData.Name);
                    stmt2->SetData(3, arenaLogEntryData.Guid);
                    stmt2->SetData(4, arenaLogEntryData.ArenaTeamId);
                    stmt2->SetData(5, arenaLogEntryData.Acc);
                    stmt2->SetData(6, arenaLogEntryData.IP);
                    if (score != PlayerScores.end())
                    {
                        stmt2->SetData(7, score->second->GetDamageDone());
                        stmt2->SetData(8, score->second->GetHealingDone());
                        stmt2->SetData(9, score->second->GetKillingBlows());
                    }
                    else
                    {
                        stmt2->SetData(7, 0);
                        stmt2->SetData(8, 0);
                        stmt2->SetData(9, 0);
                    }
                    trans->Append(stmt2);
                }

                CharacterDatabase.CommitTransaction(trans);
        };

        if (isArena() && isRated() && GetArenaType() != 4) // solo 3v3 = 4
        {
            if (winnerArenaTeam && loserArenaTeam && winnerArenaTeam != loserArenaTeam)
            {
                loserTeamRating = loserArenaTeam->GetRating();
                loserMatchmakerRating = GetArenaMatchmakerRating(GetOtherTeamId(winnerTeamId));
                winnerTeamRating = winnerArenaTeam->GetRating();
                winnerMatchmakerRating = GetArenaMatchmakerRating(winnerTeamId);

                if (winnerTeamId != TEAM_NEUTRAL)
                {
                    winnerMatchmakerChange = bValidArena ? winnerArenaTeam->WonAgainst(winnerMatchmakerRating, loserMatchmakerRating, winnerChange, GetBgMap()) : 0;
                    loserMatchmakerChange = loserArenaTeam->LostAgainst(loserMatchmakerRating, winnerMatchmakerRating, loserChange, GetBgMap());

                    sScriptMgr->OnAfterArenaRatingCalculation(this, winnerMatchmakerChange, loserMatchmakerChange, winnerChange, loserChange);

                    LOG_DEBUG("bg.arena", "match Type: {} --- Winner: old rating: {}, rating gain: {}, old MMR: {}, MMR gain: {} --- Loser: old rating: {}, rating loss: {}, old MMR: {}, MMR loss: {} ---",
                        GetArenaType(), winnerTeamRating, winnerChange, winnerMatchmakerRating, winnerMatchmakerChange,
                        loserTeamRating, loserChange, loserMatchmakerRating, loserMatchmakerChange);

                    SetArenaMatchmakerRating(winnerTeamId, winnerMatchmakerRating + winnerMatchmakerChange);
                    SetArenaMatchmakerRating(GetOtherTeamId(winnerTeamId), loserMatchmakerRating + loserMatchmakerChange);

                    // bg team that the client expects is different to TeamId
                    // alliance 1, horde 0
                    uint8 winnerTeam = winnerTeamId == TEAM_ALLIANCE ? PVP_TEAM_ALLIANCE : PVP_TEAM_HORDE;
                    uint8 loserTeam = winnerTeamId == TEAM_ALLIANCE ? PVP_TEAM_HORDE : PVP_TEAM_ALLIANCE;

                    _arenaTeamScores[winnerTeam].Assign(winnerChange, winnerMatchmakerRating, winnerArenaTeam->GetName());
                    _arenaTeamScores[loserTeam].Assign(loserChange, loserMatchmakerRating, loserArenaTeam->GetName());

                    LOG_DEBUG("bg.arena", "Arena match Type: {} for Team1Id: {} - Team2Id: {} ended. WinnerTeamId: {}. Winner rating: +{}, Loser rating: {}",
                        GetArenaType(), GetArenaTeamIdForTeam(TEAM_ALLIANCE), GetArenaTeamIdForTeam(TEAM_HORDE), winnerArenaTeam->GetId(), winnerChange, loserChange);
                }
                else // Deduct 16 points from each teams arena-rating if there are no winners after 45+2 minutes
                {
                    // pussywizard: in case of a draw, the following is always true:
                    // winnerArenaTeam => TEAM_HORDE, loserArenaTeam => TEAM_ALLIANCE

                    winnerTeamRating = winnerArenaTeam->GetRating();
                    winnerMatchmakerRating = GetArenaMatchmakerRating(TEAM_HORDE);
                    loserTeamRating = loserArenaTeam->GetRating();
                    loserMatchmakerRating = GetArenaMatchmakerRating(TEAM_ALLIANCE);
                    winnerMatchmakerChange = 0;
                    loserMatchmakerChange = 0;
                    winnerChange = ARENA_TIMELIMIT_POINTS_LOSS;
                    loserChange = ARENA_TIMELIMIT_POINTS_LOSS;

                    _arenaTeamScores[PVP_TEAM_ALLIANCE].Assign(ARENA_TIMELIMIT_POINTS_LOSS, winnerMatchmakerRating, winnerArenaTeam->GetName());
                    _arenaTeamScores[PVP_TEAM_HORDE].Assign(ARENA_TIMELIMIT_POINTS_LOSS, loserMatchmakerRating, loserArenaTeam->GetName());

                    winnerArenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS, GetBgMap());
                    loserArenaTeam->FinishGame(ARENA_TIMELIMIT_POINTS_LOSS, GetBgMap());
                }
            }
        }

        // solo 3v3
        if (GetArenaType() == 4)
        {
            if (winnerTeamId == TEAM_ALLIANCE)
            {
                sq_winner_rating = (GetSoloQueueRatingForTeam(TEAM_ALLIANCE) / 3);
                sq_loser_rating = (GetSoloQueueRatingForTeam(TEAM_HORDE) / 3);
                LOG_ERROR("solo3v3", "--- Winner rating: %u. Loser rating: %u ---", sq_winner_rating, sq_loser_rating);
            }
            else if (winnerTeamId == TEAM_HORDE)
            {
                sq_winner_rating = (GetSoloQueueRatingForTeam(TEAM_HORDE) / 3);
                sq_loser_rating = (GetSoloQueueRatingForTeam(TEAM_ALLIANCE) / 3);
                LOG_ERROR("solo3v3", "--- Winner rating: %u. Loser rating: %u ---", sq_winner_rating, sq_loser_rating);
            }

            float winnerChance = 1.0f / (1.0f + exp(log(10.0f) * (float)((float)sq_winner_rating - (float)sq_loser_rating) / 650.0f));
            float loserChance = 1.0f / (1.0f + exp(log(10.0f) * (float)((float)sq_loser_rating - (float)sq_winner_rating) / 650.0f));
            // calculate the rating modification (ELO system with k=32)
            int32 modWinner = (int32)floor(16.0f * (1.0f - winnerChance));
            int32 modLoser = (int32)ceil(16.0f * (0.0f - loserChance));

            /*
            if (winnerTeamId == TEAM_ALLIANCE)
            {
                SetArenaTeamRatingChangeForTeam(TEAM_ALLIANCE, modWinner);
                SetArenaTeamRatingChangeForTeam(TEAM_HORDE, modLoser);
            }
            else if (winnerTeamId == TEAM_HORDE)
            {
                SetArenaTeamRatingChangeForTeam(TEAM_HORDE, modWinner);
                SetArenaTeamRatingChangeForTeam(TEAM_ALLIANCE, modLoser);
            }*/


            for (auto const& [playerGuid, player] : GetPlayers())
            {
                auto const& bgTeamId = player->GetBgTeamId();

                if (bgTeamId == winnerTeamId)
                {
                    ArenaTeam* winnerSoloArenaTeam = sArenaTeamMgr->GetArenaTeamByCaptain(playerGuid, 4);
                    if (winnerSoloArenaTeam)
                    {
                        //winnerSoloArenaTeam->WonAgainst(sq_loser_rating, sq_winner_rating);
                        int32 winnerChange = 0;
                        int32 winnerMatchmakerChange = winnerSoloArenaTeam->WonAgainst(sq_winner_rating, sq_loser_rating, winnerChange, GetBgMap());
                        winnerSoloArenaTeam->SaveToDB();
                        winnerSoloArenaTeam->NotifyStatsChanged();
                        LOG_ERROR("solo3v3", "EndSoloBattleground player is in the winning team with ArenaTeamID = {}", winnerSoloArenaTeam->GetId());
                    }
                }
                else
                {
                    ArenaTeam* loserSoloArenaTeam = sArenaTeamMgr->GetArenaTeamByCaptain(playerGuid, 4);
                    if (loserSoloArenaTeam)
                    {
                        loserSoloArenaTeam->LostAgainst(sq_winner_rating, sq_loser_rating, winnerChange, GetBgMap());
                        loserSoloArenaTeam->SaveToDB();
                        loserSoloArenaTeam->NotifyStatsChanged();
                        LOG_ERROR("solo3v3", "EndSoloBattleground player is in the losing team with ArenaTeamID = {}", loserSoloArenaTeam->GetId());
                    }
                }
            }
        }




        SaveArenaLogs();

        /*
        uint8 aliveWinners = GetAlivePlayersCountByTeam(winnerTeamId);
        for (auto const& [playerGuid, player] : GetPlayers())
        {
            auto const& bgTeamId = player->GetBgTeamId();

            // per player calculation
            if (winnerArenaTeam && loserArenaTeam && winnerArenaTeam != loserArenaTeam)
            {
                if (bgTeamId == winnerTeamId)
                {
                    if (bValidArena)
                    {
                        // update achievement BEFORE personal rating update
                        uint32 rating = player->GetArenaPersonalRating(winnerArenaTeam->GetSlot());
                        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_RATED_ARENA, rating ? rating : 1);
                        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_WIN_ARENA, GetMapId());

                        // Last standing - Rated 5v5 arena & be solely alive player
                        if (GetArenaType() == ARENA_TYPE_5v5 && aliveWinners == 1 && player->IsAlive())
                        {
                            player->CastSpell(player, SPELL_LAST_MAN_STANDING, true);
                        }

                        winnerArenaTeam->MemberWon(player, loserMatchmakerRating, winnerMatchmakerChange);
                    }
                }
                else
                {
                    loserArenaTeam->MemberLost(player, winnerMatchmakerRating, loserMatchmakerChange);

                    // Arena lost => reset the win_rated_arena having the "no_lose" condition
                    player->ResetAchievementCriteria(ACHIEVEMENT_CRITERIA_CONDITION_NO_LOSE, 0);
                }

                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_PLAY_ARENA, GetMapId());
            }
        */

        // update previous opponents for arena queue
        winnerArenaTeam->SetPreviousOpponents(loserArenaTeam->GetId());
        loserArenaTeam->SetPreviousOpponents(winnerArenaTeam->GetId());

        // save the stat changes
        if (bValidArena)
        {
            winnerArenaTeam->SaveToDB();
            winnerArenaTeam->NotifyStatsChanged();
        }

        loserArenaTeam->SaveToDB();
        loserArenaTeam->NotifyStatsChanged();
    }

    // end battleground
    Battleground::EndBattleground(winnerTeamId);
}
