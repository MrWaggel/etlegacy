/*
 * Wolfenstein: Enemy Territory GPL Source Code
 * Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.
 * Copyright (C) 2012-2023 Stephen Larroque <lrq3000@gmail.com>
 *
 * ET: Legacy
 * Copyright (C) 2012-2023 ET:Legacy team <mail@etlegacy.com>
 *
 * This file is part of ET: Legacy - http://www.etlegacy.com
 *
 * ET: Legacy is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ET: Legacy is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ET: Legacy. If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, Wolfenstein: Enemy Territory GPL Source Code is also
 * subject to certain additional terms. You should have received a copy
 * of these additional terms immediately following the terms and conditions
 * of the GNU General Public License which accompanied the source code.
 * If not, please request a copy in writing from id Software at the address below.
 *
 * id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
 */
/**
 * @file sv_demo.c
 */

#include "server.h"

// static function decl
static void SV_DemoWriteClientConfigString(int clientNum, const char *cs_string);

#define Q_IsColorStringGameCommand(p)      ((p) && *(p) == Q_COLOR_ESCAPE && *((p) + 1)) // ^[anychar]
//#define CEIL(VARIABLE) ((VARIABLE - (int)VARIABLE) == 0 ? (int)VARIABLE : (int)VARIABLE + 1) // UNUSED but can be useful

/***********************************************
 * VARIABLES
 ***********************************************/

// Headers/markers for demo messages (events)
typedef enum
{
	demo_endDemo, // end of demo (close the demo)
	demo_EOF, // end of file/flux (end of event, separator, notify the demo parser to iterate to the next event of the _same_ frame)
	demo_endFrame, // player and gentity state marker (recorded each end of frame, hence the name) - at the same time marks the end of the demo frame

	demo_configString, // config string setting event
	demo_clientConfigString, // client config string setting event
	demo_clientCommand, // client command event
	demo_serverCommand, // server command event
	demo_gameCommand, // game command event
	demo_clientUserinfo, // client userinfo event (client_t management)
	demo_entityState, // gentity_t->entityState_t management
	demo_entityShared, // gentity_t->entityShared_t management
	demo_playerState, // players game state event (playerState_t management)

	//demo_clientUsercmd, // players commands/movements packets (usercmd_t management)
} demo_ops_e;

/*** STATIC VARIABLES ***/
// We set them as static so that they are global only for this file, this limit a bit the side-effect

// Big fat buffer to store all our stuff
static byte buf[0x400000];

// Save maxclients and democlients and restore them after the demo
static int savedMaxClients = -1;
static int savedFPS        = -1;
static int savedGametype   = -1;

static int savedDoWarmup  = -1;
static int savedAllowVote = -1;

static int savedTimelimit    = -1;
static int savedFraglimit    = -1;
static int savedCapturelimit = -1;

static char savedFsGameVal[MAX_QPATH] = "";
static char *savedFsGame              = savedFsGameVal;

static char savedPlaybackDemonameVal[MAX_QPATH] = "";
static char *savedPlaybackDemoname              = savedPlaybackDemonameVal;

static qboolean keepSaved = qfalse; // var that memorizes if we keep the new maxclients and democlients values (in the case that we restart the map/server for these cvars to be affected since they are latched, we need to stop the playback meanwhile we restart, and using this var we can know if the stop is a restart procedure or a real demo end) or if we can restore them (at the end of the demo)

// Game stats
static char sc0[MAX_STRING_CHARS];
static char sc1[MAX_STRING_CHARS];
static char gstats[MAX_CLIENTS][MAX_STRING_CHARS];

static int playerStatsNum;

/**
 * @brief Restores all CVARs
 */
static void SV_DemoCvarsRestore()
{
	// Restore initial cvars of the server that were modified by the demo playback
	// Note: must do it before the map_restart! so that latched values such as sv_maxclients takes effect
	if (!keepSaved)
	{
		Cvar_SetValue("sv_democlients", 0);

		if (savedMaxClients >= 0)
		{
			Cvar_SetLatched("sv_maxclients", va("%i", savedMaxClients));
			savedMaxClients = -1;
		}

		if (savedFPS > 0)
		{
			Cvar_SetValue("sv_fps", savedFPS);
			savedFPS = -1;
		}

		if (savedDoWarmup >= 0)
		{
			Cvar_SetValue("g_doWarmup", savedDoWarmup);
			savedDoWarmup = -1;
		}

		if (savedAllowVote >= 0)
		{
			Cvar_SetValue("g_allowVote", savedAllowVote);
			savedAllowVote = -1;
		}

		if (savedTimelimit >= 0)
		{
			Cvar_SetValue("timelimit", savedTimelimit);
			savedTimelimit = -1;
		}

		if (savedFraglimit >= 0)
		{
			Cvar_SetValue("fraglimit", savedFraglimit);
			savedFraglimit = -1;
		}

		if (savedCapturelimit >= 0)
		{
			Cvar_SetValue("capturelimit", savedCapturelimit);
			savedCapturelimit = -1;
		}

		if (strlen(savedFsGame))
		{
			// After setting all the other vars, we switch back the mod if necessary (it must be done only AFTER all the other cvars are set, else cvars set after won't take effect!)
			Cbuf_AddText(va("game_restart %s\n", savedFsGame));
			Q_strncpyz(savedFsGame, "", MAX_QPATH);
		}

		if (savedGametype >= 0)
		{
			Cvar_SetLatched("g_gametype", va("%i", savedGametype));
			Cbuf_AddText(va("g_gametype %i\n", savedGametype)); // force gametype switching (NOTE: must be AFTER game_restart to work!)
			savedGametype = -1;
		}

		Com_Printf("Demo changed CVARs restored.\n");
	}
}



/***********************************************
* AUXILIARY FUNCTIONS: CHECKING, FILTERING AND CLEANING
* Functions used to either trim unnecessary or privacy concerned data, or to just check if the data should be written in the demo, relayed to a more specialized function or just dropped.
***********************************************/

/**
 * @brief Same as Q_CleanStr but also remove any ^s or special empty color created by the engine in a gamecommand.
 * @param[in] string
 * @return
 */
static char *SV_CleanStrCmd(char *string)
{
	char *d = string;
	char *s = string;
	int  c;

	while ((c = *s) != 0)
	{
		if (Q_IsColorStringGameCommand(s))
		{
			s++;
		}
		else if (c >= 0x20 && c <= 0x7E)
		{
			*d++ = c;
		}

		s++;
	}

	*d = '\0';

	return string;
}

/**
 * @brief Check that the clientCommand is OK (if not we either continue with a more specialized function, or drop it altogether if there's a privacy concern)
 * @param[in] client
 * @param[in] cmd
 * @return
 */
static qboolean SV_CheckClientCommand(client_t *client, const char *cmd)
{
	// If that's a userinfo command, we directly handle that with a specialized function (and we check that it contains at least 10 characters so that when we copy the string we don't end up copying a random address in memory)
	if (strlen(cmd) > 9 && !Q_strncmp(cmd, "userinfo", 8))
	{
		char userinfo[MAX_INFO_STRING];

		Com_Memset(userinfo, 0, MAX_INFO_STRING);

		Q_strncpyz(userinfo, cmd + 9, MAX_INFO_STRING); // trimming out the "userinfo " substring (because we only need the userinfo string)
		SV_DemoWriteClientUserinfo(client, (const char *)userinfo); // passing relay to the specialized function for this job

		return qfalse; // we return false if the check wasn't right (meaning that this function does not need to process anything)
	}
	else if (!Q_strncmp(cmd, "say_team", 8))
	{
		// Privacy check: if the command is say_team, we just drop it
		return qfalse;
	}

	return qtrue; // else, the check is OK and we continue to process with the original function
}

/**
 * @brief Check that the serverCommand is OK to save (or if we drop it because already handled somewhere else or it's unwanted to store)
 * @param[in] cmd
 * @return
 */
static qboolean SV_CheckServerCommand(const char *cmd)
{
	if (!Q_strncmp(cmd, "print", 5) || !Q_strncmp(cmd, "cp", 2) || !Q_strncmp(cmd, "disconnect", 10))
	{
		// If that's a print/cp command, it's already handled by gameCommand (here it's a redundancy).
		// FIXME? possibly some print/cp are not handled by gameCommand? From my tests it was ok,
		// but if someday a few prints are missing, try to delete this check...
		// For security, we also drop disconnect (even if it should not be recorded anyway in the first place),
		// because server commands will be sent to every connected clients, and so it will disconnect everyone.
		return qfalse; // we return false if the check wasn't right
	}

	return qtrue; // else, the check is OK and we continue to process with the original function
}

/**
* @brief Filter game commands
*
* @details Filter demo game commands that should go to every connected client (not bots and democlients)
*		 also filter qagame game commands that are blocked during playback (because of missing clientSession_t it is sending garbage data)
*
* @param[in] cmd
* @return
*/
static qboolean SV_DemoGameCommandFilter(const char *cmd)
{
	int  i;
	char *filter[] = { "sc ", "score", "sc0", "sc1", "gstats", "impkd", "impt", "imsr", "impr", "imwa", "imws" };

	for (i = 0; i < 11; i++)
	{
		if (!strncmp(filter[i], cmd, strlen(filter[i])))
		{
			return qtrue;
		}
	}

	return qfalse;
}

/**
 * @brief Check and store the last command and compare it with the current one, to avoid duplicates.
 * If onlyStore is true, it will only store the new cmd, without checking.
 *
 * @param[in] cmd
 * @param[in] onlyStore
 * @return
 */
qboolean SV_CheckLastCmd(const char *cmd, qboolean onlyStore)
{
	static char prevcmddata[MAX_STRING_CHARS];
	static char *prevcmd = prevcmddata;
	char        cleanedprevcmd[MAX_STRING_CHARS];
	char        cleanedcmd[MAX_STRING_CHARS];

	Com_Memset(cleanedprevcmd, 0, MAX_STRING_CHARS);
	Com_Memset(cleanedcmd, 0, MAX_STRING_CHARS);

	Q_strncpyz(cleanedprevcmd, SV_CleanStrCmd(va("%s", prevcmd)), MAX_STRING_CHARS);
	Q_strncpyz(cleanedcmd, SV_CleanStrCmd(va("%s", cmd)), MAX_STRING_CHARS);

	// if we only want to store, we skip any checking
	if (!onlyStore && strlen(prevcmd) > 0 && !Q_stricmp(cleanedprevcmd, cleanedcmd))
	{
		// check that the previous cmd was different from the current cmd.
		return qfalse; // drop this command, it's a repetition of the previous one
	}
	else if (onlyStore && SV_DemoGameCommandFilter(cmd)) // block qagame game commands during playback
	{
		return qfalse;
	}
	else
	{
		// memorize the current cmd for the next check (clean the cmd before, because sometimes the same string is issued by the engine with some empty colors?)
		Q_strncpyz(prevcmd, cmd, MAX_STRING_CHARS);
		return qtrue;
	}
}

/**
* @brief Store newest game commands during playback for reuse
*        also store gstats during recording (based on guid)
*
* @param[in] cmd
* @return
*/
static qboolean SV_DemoStoreGameCommand(const char *cmd)
{
	if (!Q_strncmp(cmd, "gstats", 6))
	{
		int i, index = Q_atoi(Cmd_ArgsFrom(1));

		if (sv.demoState == DS_PLAYBACK)
		{
			Com_Memset(gstats[index], 0, MAX_STRING_CHARS);
			Q_strncpyz(gstats[index], va("%s", cmd), MAX_STRING_CHARS);
		}
		else
		{
			client_t            *client = &svs.clients[index];
			svdemoPlayerStats_t *svdps  = &sv.demoPlayerStats[index];

			if (client->gentity && client->gentity->r.svFlags & SVF_BOT)
			{
				return qtrue;
			}

			if (!strlen(svdps->guid))
			{
				Q_strcat(svdps->guid, 9, client->guid);
				playerStatsNum++;
			}

			if (strlen(svdps->guid) && !strncmp(svdps->guid, client->guid, 8))
			{
				Q_strncpyz(svdps->name, va("%s", client->name), MAX_NAME_LENGTH);
				Q_strncpyz(svdps->stats, va("%s", cmd), MAX_STRING_CHARS);
			}
			else
			{
				// if after reconnecting we have different clientNum
				for (i = 0; i < 100; i++)
				{
					*svdps = sv.demoPlayerStats[i];

					if (!strlen(svdps->guid))
					{
						Q_strcat(svdps->guid, 9, client->guid);
						playerStatsNum++;
					}

					if (strlen(svdps->guid) && !strncmp(svdps->guid, client->guid, 8))
					{
						Q_strncpyz(svdps->name, va("%s", client->name), MAX_NAME_LENGTH);
						Q_strncpyz(svdps->stats, va("%s", cmd), MAX_STRING_CHARS);
						break;
					}
				}
			}
		}

		return qtrue;
	}
	else if (!Q_strncmp(cmd, "sc0", 3))
	{
		if (sv.demoState == DS_PLAYBACK)
		{
			Com_Memset(sc0, 0, MAX_STRING_CHARS);
			Q_strncpyz(sc0, va("%s", cmd), MAX_STRING_CHARS);
		}

		return qtrue;
	}
	else if (!Q_strncmp(cmd, "sc1", 3))
	{
		if (sv.demoState == DS_PLAYBACK)
		{
			Com_Memset(sc1, 0, MAX_STRING_CHARS);
			Q_strncpyz(sc1, va("%s", cmd), MAX_STRING_CHARS);
		}
		return qtrue;
	}

	return qfalse;
}

/**
 * @brief Check that the gameCommand is OK and that we can save it (or if we drop it for privacy concerns and/or because it's handled somewhere else already)
 * @param[in] cmd
 * @return
 */
static qboolean SV_CheckGameCommand(const char *cmd)
{
	static int scores = 0;

	if (!SV_CheckLastCmd(cmd, qfalse))
	{
		// check that the previous cmd was different from the current cmd. The engine may send the same command to every client by their cid (one command per client) instead of just sending one command to all using NULL or -1.
		return qfalse; // drop this command, it's a repetition of the previous one
	}
	else
	{
		// store game commands during recording to save it to file
		if (SV_DemoStoreGameCommand(cmd))
		{
			return qtrue;
		}
		// we filter out the chat and tchat commands which are recorded and handled directly by clientCommand (which is easier to manage because it makes a difference between say, say_team and tell, which we don't have here in gamecommands: we either have chat(for say and tell) or tchat (for say_team) and the other difference is that chat/tchat messages directly contain the name of the player, while clientCommand only contains the clientid, so that it itself fetch the name from client->name
		// do not save tinfo and entnfo, qagame during playback will handle it
		else if (!Q_strncmp(cmd, "chat", 4) || !Q_strncmp(cmd, "tchat", 5) || !Q_strncmp(cmd, "tinfo", 5) || !Q_strncmp(cmd, "entnfo", 6))
		{
			return qfalse; // we return false if the check wasn't right
		}
		else if (!Q_strncmp(cmd, "cs", 2))
		{
			// if it's a configstring command, we handle that with the specialized function
			Cmd_TokenizeString(cmd);
			SV_DemoWriteConfigString(atoi(Cmd_Argv(1)), Cmd_Argv(2)); // relay to the specialized write configstring function
			return qfalse; // drop it with the processing of the game command
		}
		// do not save more than 1 /scores game command at the end of a round (VERY important) - FIXME: VERY ugly
		// scores = 1 - receiving first scores game command (Axis)
		// scores = 2 - receiving first scores game command (Allies)
		// scores = 3 - start ignoring
		else if (!Q_stricmp(cmd, "sc \"\n\"") && Q_atoi((Info_ValueForKey(sv.configstrings[CS_WOLFINFO], "gamestate"))) == GS_INTERMISSION)
		{
			scores++;

			if (scores > 2)
			{
				return qfalse;
			}
		}
		else if (scores && Q_atoi((Info_ValueForKey(sv.configstrings[CS_WOLFINFO], "gamestate"))) != GS_INTERMISSION)
		{
			scores = 0;
		}
		else if (scores > 2 && !Q_strncmp(cmd, "sc ", 3))
		{
			return qfalse;
		}

		return qtrue; // else, the check is OK and we continue to process with the original function
	}
}

/**
 * @brief Check that the configstring is OK (or if we relay to ClientConfigString)
 * @param cs_index
 * @param cs_string
 * @return
 */
static qboolean SV_CheckConfigString(int cs_index, const char *cs_string)
{
	// if this is a player, we save the configstring as a clientconfigstring
	if (cs_index >= CS_PLAYERS && cs_index < CS_PLAYERS + sv_maxclients->integer)
	{
		SV_DemoWriteClientConfigString(cs_index - CS_PLAYERS, cs_string);
		// we return false if the check wasn't right (meaning that we stop the normal configstring processing since the clientconfigstring function will handle that)
		return qfalse;
	}
	else if (cs_index < CS_MOTD)
	{
		// if the configstring index is below 4 (the motd), we don't save it (these are systems configstrings and will make an infinite loop for real clients at connection when the demo is played back, their management must be left to the system even at replaying)
		// just drop this configstring
		return qfalse;
	}

	// else, the check is OK and we continue to process to save it as a normal configstring (for capture scores CS_SCORES1/2, for CS_FLAGSTATUS, etc..)
	return qtrue;
}

/**
 * @brief Filters privacy keys from a client userinfo to later write in a demo file
 * @param[in,out] userinfo
 */
void SV_DemoFilterClientUserinfo(char *userinfo)
{
	Info_RemoveKey(userinfo, "cl_guid");
	Info_RemoveKey(userinfo, "n_guid"); // nitmod guid
	Info_RemoveKey(userinfo, "ip");
	//Info_SetValueForKey(userinfo, "cl_voip", "0");
}

/**
 * @brief Attempts to clean invalid characters from a filename that may prevent the demo to be stored on the filesystem
 * @param[in] string
 * @return
 */
static char *SV_CleanFilename(char *string)
{
	char *d = string;
	char *s = string;
	int  c;

	while ((c = *s) != 0)
	{
		if (Q_IsColorString(s))
		{
			s++; // skip if it's a color
		}
		else if (c == 0x2D || c == 0x2E || c == 0x5F ||  // - . _
		         (c >= 0x30 && c <= 0x39) || // numbers
		         (c >= 0x41 && c <= 0x5A) || // uppercase letters
		         (c >= 0x61 && c <= 0x7A) // lowercase letters
		         )
		{
			*d++ = c; // keep if this character is not forbidden (contained inside the above whitelist)
		}

		s++; // go to next character
	}

	*d = '\0';

	return string;
}

/**
 * @brief Generate a full datetime (local and utc) from now
 * @return
 */
static char *SV_GenerateDateTime(void)
{
	// Current local time
	qtime_t   now;
	time_t    utcnow = time(NULL);
	struct tm tnow   = *gmtime(&utcnow);
	char      buff[1024];

	Com_RealTime(&now);

	strftime(buff, sizeof buff, "timezone %Z (UTC timezone: %Y-%m-%d %H:%M:%S W%W)", &tnow);

	// Return local time and utc time
	return va("%04d-%02d-%02d %02d:%02d:%02d %s",
	          1900 + now.tm_year,
	          1 + now.tm_mon,
	          now.tm_mday,
	          now.tm_hour,
	          now.tm_min,
	          now.tm_sec,
	          buff);
}

/***********************************************
* DEMO WRITING FUNCTIONS
* Functions used to construct and write demo events
***********************************************/

/**
 * @brief Write a message/event to the demo file
 * @param[in,out] msg
 */
static void SV_DemoWriteMessage(msg_t *msg)
{
	int len;

	// Write the entire message to the file, prefixed by the length
	MSG_WriteByte(msg, demo_EOF); // append EOF (end-of-file or rather end-of-flux) to the message so that it will tell the demo parser when the demo will be read that the message ends here, and that it can proceed to the next message
	len = LittleLong(msg->cursize);
	(void) FS_Write(&len, 4, sv.demoFile);
	(void) FS_Write(msg->data, msg->cursize, sv.demoFile);
	MSG_Clear(msg);
}

/**
 * @brief Write a client command to the demo file
 * @param[in] client
 * @param[in] cmd
 */
static void SV_DemoWriteClientCommand(client_t *client, const char *cmd)
{
	msg_t msg;

	if (!SV_CheckClientCommand(client, cmd))
	{
		// check that this function really needs to store the command (or if another more specialized function should do it instead, eg: userinfo)
		return;
	}

	MSG_Init(&msg, buf, sizeof(buf));
	MSG_WriteByte(&msg, demo_clientCommand);
	MSG_WriteByte(&msg, client - svs.clients);
	MSG_WriteString(&msg, cmd);
	SV_DemoWriteMessage(&msg);
}

/**
* @brief Send newest game commands during playback
*
* @param[in] client
* @param[in] arg0
* @param[in] arg1
*/
static qboolean SV_DemoSendStoredCommands(client_t *client, const char *arg0, const char *arg1)
{
	if (!Q_strncmp(arg0, "score", 5))
	{
		if (sc0[0] && strlen(sc0))
		{
			SV_GameSendServerCommand(client - svs.clients, sc0, qtrue);
		}

		if (sc1[0] && strlen(sc1))
		{
			SV_GameSendServerCommand(client - svs.clients, sc1, qtrue);
		}

		return qtrue;
	}

	if (!Q_strncmp(arg0, "sgstats", 7))
	{
		int index = Q_atoi(arg1);
		if (index >= 0 && index < MAX_CLIENTS && strlen(gstats[index]))
		{
			SV_GameSendServerCommand(client - svs.clients, gstats[index], qtrue);
		}

		return qtrue;
	}

	return qfalse;
}

/**
 * @brief SV_DemoClientCommandCapture
 * @param[in] client
 * @param[in] msg
 * @return
 */
qboolean SV_DemoClientCommandCapture(client_t *client, const char *msg)
{
	if (sv.demoState == DS_RECORDING)     // if demo is recording, we store this command and clientid
	{
		SV_DemoWriteClientCommand(client, msg);
	}
	else if (sv.demoState == DS_PLAYBACK &&
	         ((client - svs.clients) >= sv_democlients->integer) && ((client - svs.clients) < sv_maxclients->integer)) // preventing only real clients commands (not democlients commands replayed)
	{
		// if there is a demo playback, we prevent any real client from doing a team change, if so, we issue a chat messsage (except if the player join team spectator again)
		if ((!Q_stricmp(Cmd_Argv(0), "team") && Q_stricmp(Cmd_Argv(1), "s") && Q_stricmp(Cmd_Argv(1), "spectator")))
		{
			// issue a chat message only to the player trying to join a team
			SV_SendServerCommand(client, "chat \"^3Can't join a team when a demo is replaying!\"");
			// issue a chat message only to the player trying to join a team
			SV_SendServerCommand(client, "cp \"^3Can't join a team when a demo is replaying!\"");
			return qfalse;
		}

		if (SV_DemoSendStoredCommands(client, Cmd_Argv(0), Cmd_Argv(1)))
		{
			return qfalse;
		}
	}

	return qtrue;
}

/**
 * @brief Write a server command to the demo file
 * @param[in] cmd
 *
 * @note We record only server commands that are meant to be sent to everyone in the first place,
 * that's why we don't even bother to store the clientnum. For commands that were only intended to specifically one player,
 * we only record them if they were game commands (see below the specialized function),
 * else we do not because it may be unsafe (such as "disconnect" command)
 */
void SV_DemoWriteServerCommand(const char *cmd)
{
	msg_t msg;

	if (!SV_CheckServerCommand(cmd))
	{
		// check that this function really needs to store the command
		return;
	}

	MSG_Init(&msg, buf, sizeof(buf));
	MSG_WriteByte(&msg, demo_serverCommand);
	MSG_WriteString(&msg, cmd);
	SV_DemoWriteMessage(&msg);
}

/**
 * @brief Write a game command to the demo file
 * @param[in] clientNum
 * @param[in] cmd
 *
 * @note Game commands are sent to server commands,
 * but we record them separately because game commands are safe to be replayed to everyone
 * (even if at recording time it was only intended to one player),
 * while server commands may be unsafe if it was dedicated only to one player (such as "disconnect")
 */
void SV_DemoWriteGameCommand(int clientNum, const char *cmd)
{
	msg_t msg;

	if (!SV_CheckGameCommand(cmd))
	{
		// check that this function really needs to store the command
		return;
	}

	MSG_Init(&msg, buf, sizeof(buf));
	MSG_WriteByte(&msg, demo_gameCommand);
	MSG_WriteByte(&msg, clientNum);
	MSG_WriteString(&msg, cmd);
	SV_DemoWriteMessage(&msg);
}

/**
 * @brief Write a configstring to the demo file
 *
 * @details
 * cs_index = index of the configstring (see bg_public.h)
 * cs_string = content of the configstring to set
 *
 * @param[in] cs_index
 * @param[in] cs_string
 */
void SV_DemoWriteConfigString(int cs_index, const char *cs_string)
{
	msg_t msg;
	char  cindex[MAX_CONFIGSTRINGS];

	if (!SV_CheckConfigString(cs_index, cs_string))
	{
		// check that this function really needs to store the configstring (or if it's a client configstring we relay to the specialized function)
		return;
	} // else we save it below as a normal configstring (for capture scores CS_SCORES1/2, for CS_FLAGSTATUS, etc..)

	MSG_Init(&msg, buf, sizeof(buf));
	MSG_WriteByte(&msg, demo_configString);
	Com_sprintf(cindex, sizeof(cindex), "%i", cs_index); // convert index to a string since we don't have any other way to store values that are greater than a byte (and max_configstrings is 1024 currently)
	                                                     // FIXME: try to replace by a WriteLong instead of WriteString? Or WriteData (with length! and it uses WriteByte)
	MSG_WriteString(&msg, (const char *)cindex);
	MSG_WriteString(&msg, cs_string);
	SV_DemoWriteMessage(&msg);
}

/**
 * @brief Write a client configstring to the demo file
 *
 * @param[in] clientNum
 * @param[in] cs_string
 *
 * @note This is a bit redundant with SV_DemoWriteUserinfo, because clientCommand userinfo sets the new userinfo for a democlient,
 * and clients configstrings are always derived from userinfo string.
 * But this function is left for security purpose (so we make sure the configstring is set right).
 */
static void SV_DemoWriteClientConfigString(int clientNum, const char *cs_string)
{
	msg_t msg;

	MSG_Init(&msg, buf, sizeof(buf));
	MSG_WriteByte(&msg, demo_clientConfigString);
	MSG_WriteByte(&msg, clientNum);
	MSG_WriteString(&msg, cs_string);
	SV_DemoWriteMessage(&msg);
}

/**
 * @brief Write a client userinfo to the demo file (client_t fields can almost all be filled from the userinfo string)
 * @param[in] client
 * @param[in] userinfo
 *
 * @note In practice, clients userinfo should be loaded before their configstrings (because configstrings derive from userinfo in a real game)
 */
void SV_DemoWriteClientUserinfo(client_t *client, const char *userinfo)
{
	msg_t       msg;
	static char fuserinfodata[MAX_STRING_CHARS];
	static char *fuserinfo = fuserinfodata;

	Q_strncpyz(fuserinfo, userinfo, sizeof(fuserinfodata)); // copy the client's userinfo to another storage var, so that we don't modify the client's userinfo (else the filtering will clean its infos such as cl_guid and ip, and next map change the client will be disconnected because of a wrong guid/ip!)

	if (strlen(userinfo) > 0)
	{
		// do the filtering only if the string is not empty
		SV_DemoFilterClientUserinfo(fuserinfo);   // filters out privacy keys such as ip, cl_guid, (cl_voip)
	}

	MSG_Init(&msg, buf, sizeof(buf));
	MSG_WriteByte(&msg, demo_clientUserinfo); // write the event marker
	MSG_WriteByte(&msg, client - svs.clients); // write the client number (client_t - svs.clients = client num int)
	MSG_WriteString(&msg, fuserinfo); // write the (filtered) userinfo string
	SV_DemoWriteMessage(&msg); // commit this demo event in the demo file
}

/*
 * @brief Write a client usercmd_t for the current packet (called from sv_client.c SV_UserMove) which contains the movements commands for the player
 *
 * @param[in] cl
 * @param[in] delta
 * @param[in] cmdCount
 * @param[in] cmds
 * @param[in] key
 *
 * @note Note: this is unnecessary to make players move, this is handled by entities management.
 * This is only used to add more data to the dama for data analysis AND avoid inactivity timer activation
 *
 * @note Enabling this feature will use a LOT more storage space, so enable it only if you will really use it.
 * Generally, you're probably better off leaving it disabled.
void SV_DemoWriteClientUsercmd( client_t *cl, qboolean delta, int cmdCount, usercmd_t *cmds, int key )
{
    msg_t msg;
    usercmd_t nullcmd;
    usercmd_t *cmd, *oldcmd;
    int i;

    //Com_Printf("DebugGBOWriteusercmd: %i\n", cl - svs.clients);

    MSG_Init(&msg, buf, sizeof(buf));
    MSG_WriteByte(&msg, demo_clientUsercmd);
    MSG_WriteByte(&msg, cl - svs.clients);
    if (delta)
    MSG_WriteByte(&msg, 1);
    else
    MSG_WriteByte(&msg, 0);
    MSG_WriteByte(&msg, cmdCount);

    Com_Memset( &nullcmd, 0, sizeof(nullcmd) );
    oldcmd = &nullcmd;
    for ( i = 0 ; i < cmdCount ; i++ )
    {
        cmd = &cmds[i];
        MSG_WriteDeltaUsercmdKey( &msg, key, oldcmd, cmd );
        oldcmd = cmd;
    }

    SV_DemoWriteMessage(&msg);
}
*/

/**
 * @brief Write all active clients playerState (playerState_t)
 *
 * @note This is called at every game's endFrame.
 *
 * @note Contrary to the other DemoWrite functions, this one writes all entities at once in one message, instead of one entity/command per message.
 */
static void SV_DemoWriteAllPlayerState(void)
{
	msg_t         msg;
	playerState_t *player;
	int           i;

	// Initiliaze msg
	MSG_Init(&msg, buf, sizeof(buf));

	// Write clients playerState (playerState_t)
	for (i = 0; i < sv_maxclients->integer; i++)
	{
		if (svs.clients[i].state < CS_ACTIVE)
		{
			continue;
		}

		player = SV_GameClientNum(i);
		MSG_WriteByte(&msg, demo_playerState);
		MSG_WriteByte(&msg, i);
		MSG_WriteDeltaPlayerstate(&msg, &sv.demoPlayerStates[i], player);
		sv.demoPlayerStates[i] = *player;
	}

	// Commit all these datas to the demo file
	SV_DemoWriteMessage(&msg);
}

/**
 * @brief Write all entities state (gentity_t->entityState_t)
 *
 * @note This is called at every game's endFrame.
 *
 * @note Contrary to the other DemoWrite functions, this one writes all entities at once in one message, instead of one entity/command per message.
 * This could be easily changed, but I'm not sure it would be beneficial for the CPU time and demo storage.
 */
static void SV_DemoWriteAllEntityState(void)
{
	msg_t          msg;
	sharedEntity_t *entity;
	int            i;

	// Initiliaze msg
	MSG_Init(&msg, buf, sizeof(buf));

	// Write entities (gentity_t->entityState_t or concretely sv.gentities[num].s, in gamecode level. instead of sv.)
	MSG_WriteByte(&msg, demo_entityState);
	for (i = 0; i < sv.num_entities; i++)
	{
		if (i >= sv_maxclients->integer && i < MAX_CLIENTS)
		{
			continue;
		}

		entity           = SV_GentityNum(i);
		entity->s.number = i;
		MSG_WriteDeltaEntity(&msg, &sv.demoEntities[i].s, &entity->s, qfalse);
		sv.demoEntities[i].s = entity->s;
	}

	MSG_WriteBits(&msg, ENTITYNUM_NONE, GENTITYNUM_BITS); // End marker/Condition to break: since we don't know prior how many entities we store, when reading  the demo we will use an empty entity to break from our while loop

	// Commit all these datas to the demo file
	SV_DemoWriteMessage(&msg);
}

/**
 * @brief Write all entities (gentity_t->entityShared_t)
 *
 * @note This is called at every game's endFrame.
 *
 * @note Contrary to the other DemoWrite functions, this one writes all entities at once in one message, instead of one entity/command per message.
 */
static void SV_DemoWriteAllEntityShared(void)
{
	msg_t          msg;
	sharedEntity_t *entity;
	int            i;

	// Initiliaze msg
	MSG_Init(&msg, buf, sizeof(buf));

	// Write entities (gentity_t->entityShared_t or concretely sv.gentities[num].r, in gamecode level. instead of sv.)
	MSG_WriteByte(&msg, demo_entityShared);

	for (i = 0; i < sv.num_entities; i++)
	{
		if (i >= sv_maxclients->integer && i < MAX_CLIENTS)
		{
			continue;
		}

		entity = SV_GentityNum(i);
		MSG_WriteDeltaSharedEntity(&msg, &sv.demoEntities[i].r, &entity->r, qfalse, i);
		sv.demoEntities[i].r = entity->r;
	}

	MSG_WriteBits(&msg, ENTITYNUM_NONE, GENTITYNUM_BITS); // End marker/Condition to break: since we don't know prior how many entities we store, when reading  the demo we will use an empty entity to break from our while loop

	// Commit all these datas to the demo file
	SV_DemoWriteMessage(&msg);
}

/**
* @brief SV_DemoRequestStats
*
* FIXME: look into storing and replaying ws, wstats, stshots.
*/
static void SV_DemoRequestStats(void)
{
	client_t   *client;
	static int lastRequestTime = 0;
	int        num             = sv_maxclients->integer - 1;

	if (lastRequestTime + 5000 > svs.time)
	{
		return;
	}

	lastRequestTime = svs.time;

	client = &svs.clients[num];

	if (client->state != CS_FREE && client->demoClient)
	{
		client_t  *cl;
		usercmd_t cmd;
		int       i;

		Com_Memset(&cmd, 0, sizeof(usercmd_t));
		cmd.serverTime = svs.time;

		for (i = 0, cl = svs.clients; i < num; i++, cl++)
		{
			if (cl->state == CS_ACTIVE)
			{
				SV_ExecuteClientCommand(client, va("sgstats %d", i), qtrue, qfalse);
			}
		}

		SV_ExecuteClientCommand(client, "score", qtrue, qfalse);

		// needed to actually request scoreboard stats (score)
		SV_ClientThink(client, &cmd);
	}
}

/**
* @brief SV_DemoConnectDummy
*
* @details Connect dummy client on last available server slot that will be used as requester of stats.
*
*/
static void SV_DemoConnectDummy()
{
	client_t       *client;
	sharedEntity_t *entity;
	const char     userinfo[]     = "\\cg_etVersion\\ETLTV\\etVersion\\ETLTV\\name\\^7ETLTV\\rate\\45000\\snaps\\40";
	const char     configstring[] = "n\\^7ETLTV\\3\\1\\c\\1\\lc\\1\\r\\0\\p\\6\\m\\0000000\\s\\0000000\\dn\\ - 1\\w\\3\\lw\\3\\sw\\2\\lsw\\2\\mu\\0\\ref\\0\\sc\\1\\u\\246";
	int            num            = sv_maxclients->integer - 1;

	client = &svs.clients[num];

	if (client->state != CS_FREE)
	{
		return;
	}

	client->demoClient = qtrue;
	entity             = SV_GentityNum(num);

	NET_StringToAdr("localhost", &client->netchan.remoteAddress, NA_LOOPBACK);
	entity->r.svFlags |= SVF_BOT; // set player as a "bot" so the empty IP and GUID will not cause a kick

	Cmd_TokenizeString(va("userinfo \"%s\"", userinfo));
	SV_UpdateUserinfo_f(client);

	// set the flag back or the dummy will actually play
	entity->r.svFlags &= ~SVF_BOT;

	SV_SetConfigstring(CS_PLAYERS + num, configstring);
	Q_strncpyz(client->name, Info_ValueForKey(configstring, "n"), MAX_NAME_LENGTH);

	SV_ClientEnterWorld(client, NULL);

	SV_ExecuteClientCommand(client, "team spectator", qtrue, qfalse);
}

/**
 * @brief Record all the entities (gentities fields) and players (player_t fields) at the end of every frame (this is the only write function to be called in every frame for sure)
 *
 * @details Will be called once per server's frame
 * Called in the main server's loop SV_Frame() in sv_main.c
 * Note that this function could be called DemoWriteEndFrame,
 * because it writes once at the end of every frame (the other events are written whenever they happen using hooks)
 */
void SV_DemoWriteFrame(void)
{
	msg_t msg;

	// STEP1: write all entities states at the end of the frame

	// Write entities (gentity_t->entityState_t or concretely sv.gentities[num].s, in gamecode level. instead of sv.)
	SV_DemoWriteAllEntityState();

	// Write entities (gentity_t->entityShared_t or concretely sv.gentities[num].r, in gamecode level. instead of sv.)
	SV_DemoWriteAllEntityShared();

	// Write clients playerState (playerState_t)
	SV_DemoWriteAllPlayerState();

	// Request stats
	SV_DemoRequestStats();

	//-----------------------------------------------------

	// STEP2: write the endFrame marker and server time

	MSG_Init(&msg, buf, sizeof(buf));

	// Write end of frame marker: this will commit every demo entity change (and it's done at the very end of every server frame to overwrite any change the gamecode/engine may have done)
	MSG_WriteByte(&msg, demo_endFrame);

	// Write server time (will overwrite the server time every end of frame)
	MSG_WriteLong(&msg, svs.time);

	// Commit data to the demo file
	SV_DemoWriteMessage(&msg);
}

/***********************************************
* DEMO MANAGEMENT FUNCTIONS
* Functions to start/stop the recording/playback of a demo file
***********************************************/

/**
* @brief SV_DemoAutoDemoRecordCheck
*
* @details sv_autoDemo 2 - Start recording only when there are active players
*          stop recording when there are no active players
*
*/
static qboolean SV_DemoAutoDemoRecordCheck(void)
{
	client_t   *cl;
	int        i;
	static int lastCheckTime = 0;
	qboolean   activePlayers = qfalse;

	if (lastCheckTime + 10000 > svs.time)
	{
		return qfalse;
	}

	lastCheckTime = svs.time;

	switch (sv_autoDemo->integer)
	{
	case 1:

		if (sv.demoState == DS_NONE)
		{
			return qtrue;
		}
		else
		{
			return qfalse;
		}

	case 2:

		for (i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++)
		{
			if (cl->state == CS_ACTIVE && cl->gentity && !(cl->gentity->r.svFlags & SVF_BOT))
			{
				activePlayers = qtrue;
				break;
			}
		}

		if (activePlayers && sv.demoState == DS_NONE)
		{
			return qtrue;
		}
		else if (!activePlayers && sv.demoState == DS_RECORDING)
		{
			SV_DemoStopAll();
		}

	default:
		return qfalse;
	}
}

/**
 * @brief Generates a meaningful demo filename and automatically starts the demo recording.
 *
 * @details This function is used in conjunction with the variable sv_autoDemo 1
 *
 * @note Be careful, if the hostname contains bad characters, the demo may not be able to be saved at all!
 * There's a small filtering in place but a bad filename may pass through!
 *
 * @note this function is called at MapRestart and SpawnServer (called in Map func),
 * but in no way it's called right at the time it's set.
 */
void SV_DemoAutoDemoRecord(void)
{
	qtime_t now;
	char    demoname[MAX_QPATH];

	Com_Memset(demoname, 0, MAX_QPATH);

	if (!SV_DemoAutoDemoRecordCheck())
	{
		return;
	}

	Com_RealTime(&now);
	Q_strncpyz(demoname, va("%s_%04d-%02d-%02d-%02d-%02d-%02d_%s",
	                        SV_CleanFilename(va("%s", sv_hostname->string)),
	                        1900 + now.tm_year,
	                        1 + now.tm_mon,
	                        now.tm_mday,
	                        now.tm_hour,
	                        now.tm_min,
	                        now.tm_sec,
	                        SV_CleanFilename(Cvar_VariableString("mapname"))),
	           MAX_QPATH);

	// print a message
	Com_Printf("DEMO: automatic recording server-side demo to: %s/svdemos/%s.%s%d\n", strlen(Cvar_VariableString("fs_game")) ? Cvar_VariableString("fs_game") : BASEGAME, demoname, SVDEMOEXT, PROTOCOL_VERSION);
	SV_SendServerCommand(NULL, "chat \"^3DEMO: automatic recording server-side demo to: %s.%s%d.\"", demoname, SVDEMOEXT, PROTOCOL_VERSION);
	// launch the demo recording
	Cbuf_AddText(va("demo_record %s\n", demoname));
}

/**
 * @brief Close the demo file and restart the map (can be used both when recording or when playing or at shutdown of the game)
 */
static void SV_DemoStopPlayback(void)
{
	client_t *client;
	int      olddemostate = sv.demoState;

	// Clear client configstrings
	if (olddemostate == DS_PLAYBACK)
	{
		int i;

		// unload democlients only if we were replaying a demo (if not it will produce an error!)
		for (i = 0; i < sv_democlients->integer; i++)
		{
			client = &svs.clients[i];
			if (client->demoClient)
			{
				SV_DropClient(client, "disconnected");   // same as SV_Disconnect_f(client);
				client->demoClient = qfalse;
			}
		}
	}

	// Close demo file after playback
	FS_FCloseFile(sv.demoFile);
	sv.demoState = DS_NONE;
	Cvar_SetValue("sv_demoState", DS_NONE);
	Com_Printf("DEMO: End of demo. Stopped playing demo %s.\n", sv.demoName);

	SV_DemoCvarsRestore();

	// LAST RELOAD (to reinit all vars)
	// demo hasn't actually started yet
	if (olddemostate == DS_NONE && !keepSaved)
	{
		// If keepSaved is true, then this restart procedure is totally normal (and that's why we keep values saved), else it is not normal and probably an error happened
#ifdef DEDICATED
		Com_Printf("DEMOERROR: An error happened while playing/recording the demo, please check the log for more info\n"); // if server, don't crash it if an error happens, just print a message
#else
		Cvar_SetValue("sv_killserver", 1); // do we have to do this for our listen sever? - we do Com_Error after ...
		Com_Error(ERR_DROP, "An error happened while replaying the demo, please check the log for more info.");
#endif
	}
	else if (olddemostate == DS_PLAYBACK)
	{
		// set a special state to say that we are waiting to stop (SV_DemoChangeMaxClients() will set to DS_NONE after moving the real clients to their correct slots)
		sv.demoState = DS_WAITINGSTOP;
		Cvar_SetValue("sv_demoState", DS_WAITINGSTOP);
#ifdef DEDICATED
		Cbuf_AddText(va("map %s\n", Cvar_VariableString("mapname")));   // better to do a map command rather than map_restart if we do a mod switching with game_restart, map_restart will point to no map (because the config is completely unloaded)
#else
		// Update sv_maxclients latched value (since we will kill the server because it's not a dedicated server, we won't restart the map, so latched values won't be affected unless we force the refresh)
		Cvar_Get("sv_maxclients", "20", 0);   // FIXME: Restore sv_maxclients value (force latched values to commit)
		                                      // set to 20 clients (ETL default value)
		sv_maxclients->modified = qfalse; // Set modified to false // FIXME: this is more than ugly
		// Kill the local server
		Cvar_SetValue("sv_killserver", 1); // instead of sending a Cbuf_AddText("killserver") command, we here just set a special cvar which will kill the server at the next SV_Frame() iteration (smoother than force killing)
#endif
	}
}

/**
 * @brief Call this to abort the demo play by error.
 * @param [in]
 */
static void _attribute((noreturn)) SV_DemoPlaybackError(const char *message)
{
	// FIXME: reset static vars?!

	SV_DemoStopPlayback();

	Com_Error(ERR_DROP, "SV_DemoPlaybackError: %s\n", message);
}

/**
 * @brief Just issue again the demo_play command along with the demo filename (used when the system will automatically restart the server for special cvars values to be set up, eg: sv_maxclients, fs_game, g_gametype, etc..)
 */
void SV_DemoRestartPlayback(void)
{
	if (strlen(savedPlaybackDemoname))
	{
		// Set the demoState from DS_WAITINGPLAYBACK to DS_NONE (this avoids the engine to repeatedly restart the playback until it gets done, here we just launch it once) - this fix messages bug and accelerate the loading of the demo when switching mods
		sv.demoState = DS_NONE;
		Cvar_SetValue("sv_demoState", DS_NONE);
		// Restart the playback (reissue the demo_play command and the demo filename)
		Cbuf_AddText(va("%s\n", savedPlaybackDemoname));
	}
	return;
}

/**
 * @brief Start the playback of a demo
 *
 * @details sv.demo* have already been set and the demo file opened, start reading gamestate info.
 *
 * This function will also check that everything is alright (such as gametype, map, sv_fps, sv_democlients, sv_maxclients, etc.),
 * if not it will try to fix it automatically
 *
 * @note For developers: this is basically a mirror of SV_DemoStartRecord() but the other way around (there it writes, here it reads)
 * - but only for the part about the headers, for the rest (eg: userinfo, configstrings, playerstates) it's directly managed by ReadFrame.
 */
static void SV_DemoStartPlayback(void)
{
	msg_t msg;
	int   r, time = 400, i, clients = 0, fps = 0, gametype = 0, timelimit = 0, fraglimit = 0, capturelimit = 0, warmuptime = 0, gamestate = GS_INITIALIZE;
	int   g_currentRound = 0, g_doWarmup = 0, g_warmup = 60, match_minplayers = Q_atoi(MATCH_MINPLAYERS), match_readypercent = 100, g_intermissiontime = 60;
	int   g_userTimeLimit = 0, g_useralliedrespawntime = 0, g_useraxisrespawntime = 0;
	float g_moverScale = 1.0f;
	char  map[MAX_QPATH];
	char  fs[MAX_QPATH]; // FIXME:  MAX_QPATH - only 64 chars ?!!!
	char  hostname[MAX_NAME_LENGTH];
	char  datetime[1024]; // there's no limit in the whole engine specifically designed for dates and time...
	char  *metadata; // used to store the current metadata index
	char  g_customConfig[MAX_CVAR_VALUE_STRING];

	Com_Memset(map, 0, MAX_QPATH);
	Com_Memset(fs, 0, MAX_QPATH);
	Com_Memset(hostname, 0, MAX_NAME_LENGTH);
	Com_Memset(datetime, 0, 1024);

	// Initialize the demo message buffer
	MSG_Init(&msg, buf, sizeof(buf));

	// Get the demo header
	r = FS_Read(&msg.cursize, 4, sv.demoFile);
	if (r != 4)
	{
		SV_DemoPlaybackError("DEMOERROR: SV_DemoReadFrame: demo is corrupted (not initialized correctly!)");
	}
	msg.cursize = LittleLong(msg.cursize);
	if (msg.cursize == -1)
	{
		SV_DemoPlaybackError("DEMOERROR: SV_DemoReadFrame: demo is corrupted (demo file is empty?)");
	}

	if (msg.cursize > msg.maxsize)
	{
		SV_DemoPlaybackError("DEMOERROR: SV_DemoReadFrame: demo message too long");
	}

	r = FS_Read(msg.data, msg.cursize, sv.demoFile);
	if (r != msg.cursize)
	{
		SV_DemoPlaybackError("DEMOERROR: Demo file was truncated.\n");
	}

	// Reading meta-data (infos about the demo)
	// Note: we read with an if statement, so that if in the future we add more meta datas, older demos which haven't these meta datas will still be replayable
	metadata = "";
	while (Q_stricmp(metadata, "endMeta"))
	{
		metadata = MSG_ReadString(&msg);   // We read the meta data marker

		if (!Q_stricmp(metadata, "endMeta"))     // if the string is the special endMeta string, we already break
		{
			break;
		}
		else if (!Q_stricmp(metadata, "clients"))
		{ // democlients
			// Check slots, time and map
			clients = MSG_ReadByte(&msg); // number of democlients (sv_maxclients at the time of the recording)
			if (sv_maxclients->integer < clients || // if we have less real slots than democlients slots or
			    savedMaxClients < 0 || // if there's no savedMaxClients (so this means we didin't change sv_maxclients yet, and we always need to do so since we need to add sv_democlients)
			    sv_maxclients->integer <= savedMaxClients)
			{ // or if maxclients is below or equal to the previous value of maxclients (normally it can only be equal, but if we switch the mod with game_restart, it can get the default value of 8, which can be below, so we need to check that as well)
				Com_Printf("DEMO: Not enough demo slots, automatically increasing sv_democlients to %d and sv_maxclients to %d.\n", clients, (sv_maxclients->integer + clients > MAX_CLIENTS) ? MAX_CLIENTS : sv_maxclients->integer + clients);

				// save the old values of sv_maxclients, sv_democlients and bot_minplayers to later restore them
				if (savedMaxClients < 0) // save only if it's the first value, the subsequent ones may be default values of the engine
				{
					savedMaxClients = sv_maxclients->integer;
				}

				// automatically adjusting sv_democlients, sv_maxclients and bot_minplayers
				Cvar_SetValue("sv_democlients", clients);
				Cvar_SetLatched("sv_maxclients", va("%i", (sv_maxclients->integer + clients > MAX_CLIENTS) ? MAX_CLIENTS : sv_maxclients->integer + clients));
				// BUGGY makes a dedicated server crash
				//Cvar_Get( "sv_maxclients", "8", 0 );
				//sv_maxclients->modified = qfalse;
			}
		}
		else if (!Q_stricmp(metadata, "time"))
		{ // server time
			// reading server time (from the demo)
			time = MSG_ReadLong(&msg);
			if (time < 400)
			{
				SV_DemoPlaybackError(va("DEMOERROR: Demo time too small: %d.\n", time));
			}
		}
		else if (!Q_stricmp(metadata, "sv_fps"))
		{
			// reading sv_fps (from the demo)
			fps = MSG_ReadLong(&msg);
			if (sv_fps->integer != fps && fps > 0)
			{
				savedFPS = sv_fps->integer;
				Cvar_SetValue("sv_fps", fps);
			}
		}
		else if (!Q_stricmp(metadata, "g_gametype"))
		{
			// reading g_gametype (from the demo)
			gametype = MSG_ReadLong(&msg);
			// memorize the current gametype
			if (!keepSaved)
			{
				savedGametype = sv_gametype->integer; // save the gametype before switching
			}
		}
		else if (!Q_stricmp(metadata, "fs_game"))
		{
			// reading fs_game (mod name)
			Q_strncpyz(fs, MSG_ReadString(&msg), MAX_QPATH);
			if (strlen(fs))
			{
				Com_Printf("DEMO: Warning: this demo was recorded for the following mod: %s\n", fs);
			}
		}
		else if (!Q_stricmp(metadata, "map"))
		{
			// reading map (from the demo)
			Q_strncpyz(map, MSG_ReadString(&msg), MAX_QPATH);
			if (!FS_FOpenFileRead(va("maps/%s.bsp", map), NULL, qfalse))
			{
				SV_DemoPlaybackError(va("Map does not exist: %s.\n", map));
			}
		}
		else if (!Q_stricmp(metadata, "timelimit"))
		{
			// reading initial timelimit
			timelimit = MSG_ReadLong(&msg);
			// memorize the current timelimit
			if (!keepSaved)
			{
				savedTimelimit = Cvar_VariableIntegerValue("timelimit");
			}

			// set the demo setting
			// Note: setting correct timelimit is essential for the demo gamestate to be replayed correctly
			Cvar_SetValue("timelimit", timelimit);
		}
		else if (!Q_stricmp(metadata, "fraglimit"))
		{
			// reading initial fraglimit
			fraglimit = MSG_ReadLong(&msg);
			// memorize the current fraglimit
			if (!keepSaved)
			{
				savedFraglimit = Cvar_VariableIntegerValue("fraglimit");
			}

			// set the demo setting
			Cvar_SetValue("fraglimit", fraglimit); // Note: unnecessary for the demo to be replayed, but allows to show the limit in the HUD. FIXME: if the limit is changed during the game, the new value won't be reflected in the demo (but the demo will continue to play to its integrality)
		}
		else if (!Q_stricmp(metadata, "capturelimit"))
		{
			// reading initial capturelimit
			capturelimit = MSG_ReadLong(&msg);
			// memorize the current capturelimit
			if (!keepSaved)
			{
				savedCapturelimit = Cvar_VariableIntegerValue("capturelimit");
			}

			// set the demo setting
			Cvar_SetValue("capturelimit", capturelimit); // Note: unnecessary for the demo to be replayed, but allows to show the limit in the HUD. FIXME: if the limit is changed during the game, the new value won't be reflected in the demo (but the demo will continue to play to its integrality)

			// Additional infos (not necessary to replay a demo)
		}
		else if (!Q_stricmp(metadata, "hostname"))
		{
			// reading sv_hostname (additional info)
			Q_strncpyz(hostname, MSG_ReadString(&msg), MAX_NAME_LENGTH);

		}
		else if (!Q_stricmp(metadata, "datetime"))
		{
			// reading datetime
			Q_strncpyz(datetime, MSG_ReadString(&msg), 1024);
		}
		else if (!Q_stricmp(metadata, "warmuptime"))
		{
			// reading warmuptime (CS_WARMUP)
			warmuptime = MSG_ReadLong(&msg);
		}
		else if (!Q_stricmp(metadata, "gamestate"))
		{
			// reading initial gamestate
			gamestate = MSG_ReadLong(&msg);

			// FIXME: this is a workaround, when recording starts during GS_WARMUP_COUNTDOWN we can't properly set level.warmupTime and the game instantly changes gamestate to GS_PLAYING
			// thus causing the timelimits of the map to be incorrect, as a workaround, if we start recording during GS_WARMUP_COUNTDOWN we will skip it and start replaying demo from when the game started
			// so level.time = warmuptime instead
			if (warmuptime && gamestate == GS_WARMUP_COUNTDOWN)
			{
				// set the demo setting
				Cvar_SetValue("gamestate", GS_PLAYING);
				time = warmuptime;
			}
			else
			{
				// set the demo setting
				Cvar_SetValue("gamestate", gamestate);
			}
		}
		else if (!Q_stricmp(metadata, "g_doWarmup"))
		{
			// reading g_doWarmup
			g_doWarmup = MSG_ReadLong(&msg);

			if (!keepSaved)
			{
				savedDoWarmup = Cvar_VariableIntegerValue("g_doWarmup");
			}

			// set the demo setting
			Cvar_SetValue("g_doWarmup", g_doWarmup);
		}
		else if (!Q_stricmp(metadata, "g_warmup"))
		{
			// reading g_warmup
			g_warmup = MSG_ReadLong(&msg);

			// set the demo setting
			Cvar_SetValue("g_warmup", g_warmup);
		}
		else if (!Q_stricmp(metadata, "g_currentRound"))
		{
			// reading g_currentRound
			g_currentRound = MSG_ReadLong(&msg);

			// set the demo setting
			Cvar_SetValue("g_currentRound", g_currentRound);
		}
		else if (!Q_stricmp(metadata, "match_minplayers"))
		{
			// reading match_minplayers
			match_minplayers = MSG_ReadLong(&msg);

			// set the demo setting
			Cvar_SetValue("match_minplayers", match_minplayers);
		}
		else if (!Q_stricmp(metadata, "match_readypercent"))
		{
			// reading match_readypercent
			match_readypercent = MSG_ReadLong(&msg);

			// set the demo setting
			Cvar_SetValue("match_readypercent", match_readypercent);
		}
		else if (!Q_stricmp(metadata, "g_intermissiontime"))
		{
			// reading g_intermissiontime
			g_intermissiontime = MSG_ReadLong(&msg);

			// set the demo setting
			Cvar_SetValue("g_intermissiontime", g_intermissiontime);
		}
		else if (!Q_stricmp(metadata, "g_userTimeLimit"))
		{
			// reading g_userTimeLimit
			g_userTimeLimit = MSG_ReadLong(&msg);

			// set the demo setting
			Cvar_SetValue("g_userTimeLimit", g_userTimeLimit);

			if (g_userTimeLimit)
			{
				Cvar_SetValue("timelimit", g_userTimeLimit);
			}
		}
		else if (!Q_stricmp(metadata, "g_useralliedrespawntime"))
		{
			// reading g_useralliedrespawntime
			g_useralliedrespawntime = MSG_ReadLong(&msg);

			// set the demo setting
			Cvar_SetValue("g_useralliedrespawntime", g_useralliedrespawntime);
		}
		else if (!Q_stricmp(metadata, "g_useraxisrespawntime"))
		{
			// reading g_useraxisrespawntime
			g_useraxisrespawntime = MSG_ReadLong(&msg);

			// set the demo setting
			Cvar_SetValue("g_useraxisrespawntime", g_useraxisrespawntime);
		}
		else if (!Q_stricmp(metadata, "g_moverScale"))
		{
			// reading g_moverScale
			g_moverScale = MSG_ReadFloat(&msg);

			// set the demo setting
			Cvar_SetValue("g_moverScale", g_moverScale);
		}
		else if (!Q_stricmp(metadata, "g_customConfig"))
		{
			// reading g_customConfig (config name)
			// Note: configs are a big issue, they are loaded on devmap and can overwrite settings that are needed for the demo to replay correctly, like g_doWarmup or timelimit
			// where incase of g_doWarmup it's not possible to correct it after map loads because the gamestate could be already changed
			// the config is reloaded at times like end of GS_WARMUP_COUNTDOWN thus overwriting demo settings (fe. timelimit), we will need to save all crucial cvars instead of loading the configs
			// leaving this for debugging in case of future issues that could be caused by custom configs
			Q_strncpyz(g_customConfig, MSG_ReadString(&msg), MAX_CVAR_VALUE_STRING);
			if (strlen(g_customConfig))
			{
				Com_Printf("DEMO: Warning: this demo was recorded for the following config: %s\n", g_customConfig);
			}

			// set the demo setting
			Cvar_Set("g_customConfig", "");
		}
	}

	// force g_antiwarp to 0 as it can break player viewangles sometimes with empty ClientThinks
	Cvar_SetValue("g_antiwarp", 0);
	Cvar_SetValue("g_skipCorrection", 0);

	// Memorize g_allowVote
	if (!keepSaved)
	{
		savedAllowVote = Cvar_VariableIntegerValue("g_allowVote");
	}
	// Remove g_allowVote (prevent players to call a vote while a map is replaying)
	Cvar_SetValue("g_allowVote", 0);
	Cvar_SetValue("vote_allow_map", 0);

	// Printing infos about the demo
	if (!sv_demoTolerant->integer)   // print the meta datas only if we're not in faults tolerance mode (because if there are missing meta datas, the printing will throw an exception! So we'd better avoid it)
	{
		Com_Printf("DEMO: Details of %s recorded %s on server \"%s\": sv_fps: %i initial_servertime: %i clients: %i fs_game: %s g_gametype: %i map: %s timelimit: %i fraglimit: %i capturelimit: %i \n", sv.demoName, datetime, hostname, fps, time, clients, fs, gametype, map, timelimit, fraglimit, capturelimit);
	}

	// Checking if all initial conditions from the demo are met (map, sv_fps, gametype, servertime, etc...)
	// FIXME? why sv_cheats is needed? Just to be able to use cheats commands to pass through walls?
	if (!com_sv_running->integer || Q_stricmp(sv_mapname->string, map) ||
	    Q_stricmp(Cvar_VariableString("fs_game"), fs) ||
	    !Cvar_VariableIntegerValue("sv_cheats") ||
	    //(time < svs.time && !keepSaved) || // We do restart to reinit the server time
	    sv_maxclients->modified ||
	    (sv_gametype->integer != gametype && !(gametype == GT_SINGLE_PLAYER && sv_gametype->integer == GT_COOP))  // check for gametype change (need a restart to take effect since it's a latched var) AND check that the gametype difference is not between SinglePlayer and DM/FFA, which are in fact the same gametype (and the server will automatically change SinglePlayer to FFA, so we need to detect that and ignore this automatic change)
	    )
	{
		/// Change to the right map/maxclients/mod and restart the demo playback at the next SV_Frame() iteration

		//Cvar_SetValue("sv_democlients", 0); // necessary to stop the playback, else it will produce an error since the demo has not yet started!
		keepSaved = qtrue; // Declare that we want to keep the value saved (and we don't want to restore them now, because the demo hasn't started yet!)
		SV_DemoStopPlayback(); // Stop the demo playback (reset back any change)
		sv.demoState = DS_WAITINGPLAYBACK; // Set the status WAITINGPLAYBACK meaning that as soon as the server will be restarted, the next SV_Frame() iteration must reactivate the demo playback
		Cvar_SetValue("sv_demoState", DS_WAITINGPLAYBACK); // set the cvar too because when restarting the server, all sv.* vars will be destroyed
		Q_strncpyz(savedPlaybackDemoname, Cmd_Cmd(), MAX_QPATH); // we need to copy the value because since we may spawn a new server (if the demo is played client-side OR if we change fs_game), we will lose all sv. vars

		Cvar_SetValue("sv_autoDemo", 0); // disable sv_autoDemo else it will start a recording before we can replay a demo (since we restart the map)
		// **** Automatic mod (fs_game) switching management ****
		if ((Q_stricmp(Cvar_VariableString("fs_game"), fs) && strlen(fs)) ||
		    (!strlen(fs) && Q_stricmp(Cvar_VariableString("fs_game"), fs) && Q_stricmp(fs, BASEGAME)))
		{ // change the game mod only if necessary - if it's different from the current gamemod and the new is not empty, OR the new is empty but it's not BASEGAME and it's different (we're careful because it will restart the game engine and so probably every client will get disconnected)

			// Memorize the current mod (only if we are indeed switching mod, otherwise we will save basegame instead of empty strings and force a mod switching when stopping the demo when we haven't changed mod in the first place!)
			if (strlen(Cvar_VariableString("fs_game")))
			{
				// if fs_game is not "", we save it
				Q_strncpyz(savedFsGame, (const char *)Cvar_VariableString("fs_game"), MAX_QPATH);
			}
			else
			{
				// else, it's equal to "", and this means that we were playing in the basegame mod, but we need to have a non-empty string, else we can't use game_restart!
				Q_strncpyz(savedFsGame, BASEGAME, MAX_QPATH);
			}

			// Switch the mod
			Com_Printf("DEMO: Trying to switch automatically to the mod %s to replay the demo\n", strlen(fs) ? fs : BASEGAME);
			Cvar_SetValue("sv_democlients", 0); // set sv_democlients to 0 (because game_restart will reset sv_maxclients, so we have a risk to have a greater sv_democlients than sv_maxclients, and we don't want that)
			Cbuf_AddText(va("game_restart %s\n", fs)); // switch mod! FIXME: command doesn't exist!
		}

		// Set the game time (level.time, level.startTime) to our demo time to correctly replay the timelimits of the map/rounds
		// This is estimate - 7 GAME_INIT_FRAMES from starting the server and loading the map and one additional frame in which we hope to get back here and start reading the demo frames
		// But it's almost never the case, svs.time can be higher by 50-300ms, which can have a negative effect on the playback,
		// The time will be adjusted in SV_DemoReadFrame to fix issues caused by mismatched time
		if (time > svs.time)
		{
			svs.time = time - ((GAME_INIT_FRAMES + 1) * FRAMETIME) + (1000 / sv_fps->integer);
		}

		Cbuf_AddText(va("g_gametype %i\ndevmap %s\n", gametype, map)); // Change gametype and map (using devmap to enable cheats)

		return;
	}

	// Initialize our stuff
	Com_Memset(sv.demoEntities, 0, sizeof(sv.demoEntities));
	Com_Memset(sv.demoPlayerStates, 0, sizeof(sv.demoPlayerStates));
	Cvar_SetValue("sv_democlients", clients); // Note: we need SV_Startup() to NOT use SV_ChangeMaxClients for this to work without crashing when changing fs_game

	// FIXME: omnibot - this bot stuff isn't tested well (but better than before)
	// disable bots and ensure they don't connect again
	if (Cvar_Get("omnibot_enable", "0", 0)->integer > 0) // FIXME: and/or check for bot player count, omnibot_enable is latched !!!
	{

		Cbuf_ExecuteText(EXEC_NOW, "bot maxbots 0; bot kickall\n");
		Cvar_SetValue("omnibot_enable", 0); // FIXME: restore value and bot players after demo play

		Com_Printf("Bye bye omnibots - you are not allowed to view this demo.\n");
	}

	// Force all real clients already connected before the demo begins to be set to spectator team
	for (i = sv_democlients->integer; i < sv_maxclients->integer; i++)
	{
		if (svs.clients[i].state >= CS_CONNECTED)
		{
			// Only set as spectator a player that is at least connected (or primed or active)
			SV_ExecuteClientCommand(&svs.clients[i], "team spectator", qtrue, qfalse); // should be more interoperable than a forceteam
			Cbuf_ExecuteText(EXEC_NOW, va("forceteam %i spectator", i)); // sometimes team spectator does not work when a demo is replayed client-side with some mods (eg: ExcessivePlus), in this case we also issue a forceteam (even if it's less interoperable)
		}
	}

	// Start reading the first frame
	Com_Printf("Playing server-side demo %s.\n", sv.demoName); // log that the demo is started here
	SV_SendServerCommand(NULL, "chat \"^3DEMO: Server-side demo replay started!\"");   // send a message to player
	SV_SendServerCommand(NULL, "cp \"^3DEMO: Server-side demo replay started!\"");   // send a centerprint message to player
	sv.demoState = DS_PLAYBACK; // set state to playback
	Cvar_SetValue("sv_demoState", DS_PLAYBACK);
	keepSaved = qfalse; // Don't save values anymore: the next time we stop playback, we will restore previous values (because now we are really launching the playback, so anything that might happen now is either a big bug or the end of demo, in any case we want to restore the values)
	SV_DemoReadFrame(); // reading the first frame, which should contain some initialization events (eg: initial confistrings/userinfo when demo recording started, initial entities states and placement, etc..)

	return;
}

/**
 * @brief Start the recording of a demo by saving some headers data (such as the number of clients, mapname, gametype, etc..)
 * @details sv.demo* have already been set and the demo file opened, start writing gamestate info
 */
static void SV_DemoStartRecord(void)
{
	msg_t msg;
	int   i;

	// Set democlients to 0 since it's only used for replaying demo
	Cvar_SetValue("sv_democlients", 0);

	MSG_Init(&msg, buf, sizeof(buf));

	// Write number of clients (sv_maxclients < MAX_CLIENTS or else we can't playback)
	MSG_WriteString(&msg, "clients"); // for each demo meta data (infos about the demo), we prepend the name of the var (this allows for fault tolerance and retrocompatibility) - FIXME? We could also use MSG_LookaheadByte() to read a byte, instead of a string, this would save a tiny bit of storage space
	MSG_WriteByte(&msg, sv_maxclients->integer);
	// Write current server in-game time
	MSG_WriteString(&msg, "time");
	MSG_WriteLong(&msg, svs.time);
	// Write sv_fps
	MSG_WriteString(&msg, "sv_fps");
	MSG_WriteLong(&msg, sv_fps->integer);
	// Write g_gametype
	MSG_WriteString(&msg, "g_gametype");
	MSG_WriteLong(&msg, sv_gametype->integer);
	// Write fs_game (mod name)
	MSG_WriteString(&msg, "fs_game");
	MSG_WriteString(&msg, Cvar_VariableString("fs_game"));
	// Write map name
	MSG_WriteString(&msg, "map");
	MSG_WriteString(&msg, sv_mapname->string);
	// Write timelimit
	MSG_WriteString(&msg, "timelimit");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("timelimit"));
	// Write sv_hostname (only for info)
	MSG_WriteString(&msg, "hostname");
	MSG_WriteString(&msg, sv_hostname->string);
	// Write current datetime (only for info)
	MSG_WriteString(&msg, "datetime");
	MSG_WriteString(&msg, SV_GenerateDateTime());
	// Write warmuptime (CS_WARMUP)
	MSG_WriteString(&msg, "warmuptime");
	MSG_WriteLong(&msg, Q_atoi(sv.configstrings[CS_WARMUP]));
	// Write current gamestate
	MSG_WriteString(&msg, "gamestate");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("gamestate"));
	// Write g_doWarmup
	MSG_WriteString(&msg, "g_doWarmup");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("g_doWarmup"));
	// Write g_warmup
	MSG_WriteString(&msg, "g_warmup");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("g_warmup"));
	// Write g_currentRound
	MSG_WriteString(&msg, "g_currentRound");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("g_currentRound"));
	// Write match_minplayers
	MSG_WriteString(&msg, "match_minplayers");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("match_minplayers"));
	// Write match_readypercent
	MSG_WriteString(&msg, "match_readypercent");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("match_readypercent"));
	// Write g_intermissiontime
	MSG_WriteString(&msg, "g_intermissiontime");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("g_intermissiontime"));
	// Write g_userTimeLimit
	MSG_WriteString(&msg, "g_userTimeLimit");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("g_userTimeLimit"));
	// Write g_useralliedrespawntime
	MSG_WriteString(&msg, "g_useralliedrespawntime");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("g_useralliedrespawntime"));
	// Write g_useraxisrespawntime
	MSG_WriteString(&msg, "g_useraxisrespawntime");
	MSG_WriteLong(&msg, Cvar_VariableIntegerValue("g_useraxisrespawntime"));
	// Write g_moverScale
	MSG_WriteString(&msg, "g_moverScale");
	MSG_WriteLong(&msg, Cvar_VariableValue("g_moverScale"));
	// Write current config
	MSG_WriteString(&msg, "g_customConfig");
	MSG_WriteString(&msg, Cvar_VariableString("g_customConfig"));

	// Write end of meta datas (since we will read a string each loop, we need to set a special string to specify the reader that we end the loop, we cannot use a marker because it's a byte)
	MSG_WriteString(&msg, "endMeta");

	// Write all the above into the demo file
	SV_DemoWriteMessage(&msg);

	// Write initial clients userinfo
	// Note: should be before clients configstrings since clients configstrings are derived from userinfo
	for (i = 0; i < sv_maxclients->integer; i++)
	{
		client_t *client = &svs.clients[i];

		if (client->state >= CS_CONNECTED)
		{
			if (client->userinfo[0] != '\0')
			{
				// if player is connected and the userinfo exists, we store it
				SV_DemoWriteClientUserinfo(client, (const char *)client->userinfo);
			}
		}
	}

	// Write all configstrings (such as current capture score CS_SCORE1/2, etc...), including clients configstrings
	// Note: system configstrings will be filtered and excluded (there's a check function for that), and clients configstrings  will be automatically redirected to the specialized function (see the check function)
	for (i = 0; i < MAX_CONFIGSTRINGS; i++)
	{
		if (sv.configstrings[i])     // if the configstring pointer exists in memory (because we will check all the possible indexes, but we don't know if they really exist in memory and are used or not, so here we check for that)
		{
			SV_DemoWriteConfigString(i, sv.configstrings[i]);
		}
	}

	// Write entities and players
	Com_Memset(sv.demoEntities, 0, sizeof(sv.demoEntities));
	Com_Memset(sv.demoPlayerStates, 0, sizeof(sv.demoPlayerStates));
	Com_Memset(sv.demoPlayerStats, 0, sizeof(sv.demoPlayerStats));
	playerStatsNum = 0;
	// End of frame
	SV_DemoWriteFrame();
	// Announce we are writing the demo
	Com_Printf("DEMO: Recording server-side demo %s.\n", sv.demoName);
	SV_SendServerCommand(NULL, "chat \"^3DEMO: Recording server-side demo %s.\"", sv.demoName);
	// Change recording state
	sv.demoState = DS_RECORDING;
	Cvar_SetValue("sv_demoState", DS_RECORDING);

	// connect dummy client that will be used to collect stats
	SV_DemoConnectDummy();
}

/**
 * @brief Stop the recording of a demo
 * @details Write end of demo (demo_endDemo marker) and close the demo file
 */
static void SV_DemoStopRecord(void)
{
	client_t     *client = &svs.clients[sv_maxclients->integer - 1];
	msg_t        msg;
	fileHandle_t demoStatsFile;
	char         statsFileName[MAX_QPATH], buffer[2048];
	int          i;

	// End the demo
	MSG_Init(&msg, buf, sizeof(buf));
	MSG_WriteByte(&msg, demo_endDemo);
	SV_DemoWriteMessage(&msg); // this also writes demo_EOF

	// Close the file (else it won't be openable until the server is closed)
	FS_FCloseFile(sv.demoFile);
	// Change recording state
	sv.demoState = DS_NONE;
	Cvar_SetValue("sv_demoState", DS_NONE);
	// Announce
	Com_Printf("DEMO: Stopped recording server-side demo %s.\n", sv.demoName);
	SV_SendServerCommand(NULL, "chat \"^3DEMO: Stopped recording server-side demo %s.\"", sv.demoName);

	// disconnect dummy client if there is one
	if (client->state != CS_FREE && client->demoClient)
	{
		SV_DropClient(client, "disconnected");
		client->demoClient = qfalse;
	}

	// save player stats to file
	COM_StripExtension(sv.demoName, statsFileName, MAX_QPATH);
	demoStatsFile = FS_FOpenFileWrite(va("%s.txt", statsFileName));

	if (!demoStatsFile)
	{
		Com_Printf("DEMO: ERROR: Couldn't open %s.txt for writing.\n", statsFileName);
		return;
	}

	for (i = 0; i < playerStatsNum; i++)
	{
		Q_strncpyz(buffer, va("%s", va("%s\\%s\\%s", sv.demoPlayerStats[i].guid, sv.demoPlayerStats[i].name, sv.demoPlayerStats[i].stats)), 2048);
		FS_Write(buffer, strlen(buffer), demoStatsFile);
	}

	FS_FCloseFile(demoStatsFile);
}

/***********************************************
 * DEMO READING FUNCTIONS
 *
 * Functions to read demo events
 ***********************************************/

/**
 * @brief Replay a client command
 *
 * @details Client command management (generally automatic, such as tinfo for HUD team overlay status, team selection, etc.)
 * - except userinfo command that is managed by another event
 *
 * @param[in] msg
 */
static void SV_DemoReadClientCommand(msg_t *msg)
{
	char *cmd;
	int  num;

	num = MSG_ReadByte(msg);
	cmd = MSG_ReadString(msg);

	SV_ExecuteClientCommand(&svs.clients[num], cmd, qtrue, qfalse); // 3rd arg = clientOK, and it's necessarily true since we saved the command in the demo (else it wouldn't be saved)
}

/**
 * @brief Replay a server command
 *
 * @details Server command management - except print/cp (dropped at recording because already handled by gameCommand).
 *
 * @param[in] msg
 */
static void SV_DemoReadServerCommand(msg_t *msg)
{
	char *cmd;

	cmd = MSG_ReadString(msg);
	SV_SendServerCommand(NULL, "%s", cmd);
}

/**
 * @brief Replay a game command
 *
 * @details Game command management, such as prints/centerprint (cp) scores command, except chat/tchat (handled by clientCommand)
 *
 * Basically the same as demo_serverCommand (because sv_GameSendServerCommand uses SV_SendServerCommand,
 * but game commands are safe to be replayed to everyone (but shouldn't), while server commands may be unsafe such as disconnect)
 *
 * @param[in] msg
 */
static void SV_DemoReadGameCommand(msg_t *msg)
{
	playerState_t *player;
	client_t      *client;
	char          *cmd;
	int           clientNum, i;

	clientNum = MSG_ReadByte(msg);
	cmd       = MSG_ReadString(msg);

	if (SV_CheckLastCmd(cmd, qfalse) && clientNum < sv_maxclients->integer)
	{
		// store commands that should be sent per request
		if (SV_DemoStoreGameCommand(cmd))
		{
			return;
		}

		// do not send game commands to democlients
		for (i = 0, client = svs.clients; i < sv_maxclients->integer; i++, client++)
		{
			player = SV_GameClientNum(i);

			// FIXME: this is just stupid
			// commands that get sent to every connected client
			if (!client->demoClient && client->state == CS_ACTIVE && SV_DemoGameCommandFilter(cmd))
			{

			}
			// send game commands to real clients only if they are following a democlient that receives the command
			else if (client->demoClient || client->state <= CS_PRIMED || player->clientNum != clientNum)
			{
				continue;
			}

			SV_GameSendServerCommand(i, cmd, qtrue);
		}
	}
}

/**
 * @brief Read a configstring from a message and load it into memory
 * @param[in] msg
 */
static void SV_DemoReadConfigString(msg_t *msg)
{
	char *configstring;
	int  num;

	//num = MSG_ReadLong(msg, MAX_CONFIGSTRINGS); FIXME: doesn't work, dunno why, but it would be better than a string to store a long int!
	num          = Q_atoi(MSG_ReadString(msg));
	configstring = MSG_ReadString(msg);

	if (num < CS_PLAYERS + sv_democlients->integer || num >= CS_PLAYERS + sv_maxclients->integer)
	{
		// we make sure to not overwrite real client configstrings (else when the demo starts, normal players will have no name, no model and no status!) - this cannot be done at recording time because we can't know how many sv_maxclients will be set at replaying time
		SV_SetConfigstring(num, configstring);

		// Set cvars based on configstrings
		if (num == CS_WOLFINFO && strlen(configstring))
		{
			float timelimit = Q_atof(Info_ValueForKey(configstring, "g_nextTimeLimit"));

			if (timelimit)
			{
				Cvar_SetValue("g_nextTimeLimit", timelimit);
			}
		}
	}
}

#define TEAM_COMMAND_LENGTH 13

/**
 * @brief Read a demo client configstring from a message, load it into memory and broadcast changes to gamecode and clients
 * @details This function also manages demo clientbegin at connections and teamchange
 * (which are normally totally handled by the gamecode, so we can't directly access nor store these events in the demo,
 * we must use clever ways to reproduce them at the right time)
 *
 * @param[in] msg
 */
static void SV_DemoReadClientConfigString(msg_t *msg)
{
	client_t *client;
	char     *configstring;
	int      num;

	num          = MSG_ReadByte(msg);
	configstring = MSG_ReadString(msg);

	/**** DEMOCLIENTS CONNECTION MANAGEMENT  ****/
	// This part manages when a client should begin or be dropped based on the configstrings. This is a workaround because begin and disconnect events are managed in the gamecode,
	// so we here use a clever way to know when these events happen (this is based on a careful reading of how work the mechanisms that manage players in a real game, so this should be OK in any case).
	// Note: this part could also be used in userinfo instead of client configstrings (but since DropClient automatically sets userinfo to null, which is not the case the other way around, this way was preferred)
	if (Q_stricmp(sv.configstrings[CS_PLAYERS + num], configstring) && configstring && strlen(configstring))
	{
		// client begin or just changed team: previous configstring and new one are different, and the new one is not null
		int svdoldteam;
		int svdnewteam;

		client = &svs.clients[num];

		svdoldteam = strlen(Info_ValueForKey(sv.configstrings[CS_PLAYERS + num], "t")) ? Q_atoi(Info_ValueForKey(sv.configstrings[CS_PLAYERS + num], "t")) : -1; // affect the new team if detected, else if an empty string is returned, just set -1 (will allow us to detect that there's really no team change instead of having 0 which is TEAM_FREE)
		svdnewteam = strlen(Info_ValueForKey(configstring, "t")) ? Q_atoi(Info_ValueForKey(configstring, "t")) : -1;

		// Set the client configstring (using a standard Q3 function)
		SV_SetConfigstring(CS_PLAYERS + num, configstring);

		// set the name (useful for internal functions such as status_f). Anyway userinfo will automatically set both name (server-side) and netname (gamecode-side).
		Q_strncpyz(client->name, Info_ValueForKey(configstring, "n"), MAX_NAME_LENGTH);

		// Set the remoteAddress of this client to localhost (this will set "ip\localhost" in the userinfo, which will in turn force the gamecode to set this client as a localClient, which avoids inactivity timers and some other stuffs to be triggered)
		if (strlen(configstring)) // we check that the client isn't being dropped
		{
			NET_StringToAdr("localhost", &client->netchan.remoteAddress, NA_LOOPBACK);
		}

		SV_ClientEnterWorld(client, NULL);

		// DEMOCLIENT INITIAL TEAM MANAGEMENT
		// Note: needed only to set the initial team of the democlients, subsequent team changes are directly handled by their clientCommands
		if (configstring && strlen(configstring) &&
		    (svdoldteam == -1 || (svdoldteam != svdnewteam && svdnewteam != -1 && client->gentity->s.teamNum == TEAM_FREE))   // if there was no team for this player before or if the new team is different
		    )
		{
			// If the client changed team, we manually issue a team change (workaround by using a clientCommand team)

			char svdnewteamstr[TEAM_COMMAND_LENGTH] = { 0 };

			// FIXME: selecting medic because of possible class and weapon restrictions.
			// Use client->sess.playerType from configstring (key = c) for class, client->sess.playerWeapon (key = w) for weapon, and client->sess.playerWeapon2 (key = sw) for secondary weapon
			// But it's not really needed as players are correct class and weapons anyway and medic is the safest bet if there will be weapon/class restrictions because of config issues
			switch (svdnewteam)
			{
			case TEAM_ALLIES:
				strcpy(svdnewteamstr, "allies 1 8 7");
				break;
			case TEAM_AXIS:
				strcpy(svdnewteamstr, "axis 1 3 2");
				break;
			case TEAM_SPECTATOR:
				strcpy(svdnewteamstr, "spectator");
				break;
			case TEAM_FREE:
			default:
				strcpy(svdnewteamstr, "free");
				break;
			}

			SV_ExecuteClientCommand(client, va("team %s", svdnewteamstr), qtrue, qfalse);
		}
	}
	else if (Q_stricmp(sv.configstrings[CS_PLAYERS + num], configstring) && strlen(sv.configstrings[CS_PLAYERS + num]) && (!configstring || !strlen(configstring)))
	{
		// client disconnect: different configstrings and the new one is empty, so the client is not there anymore, we drop him (also we check that the old config was not empty, else we would be disconnecting a player who is already dropped)
		client = &svs.clients[num];
		SV_DropClient(client, "disconnected");   // same as SV_Disconnect_f(client);
		SV_SetConfigstring(CS_PLAYERS + num, configstring); // empty the configstring
	}
	else
	{ // In any other case (should there be?), we simply set the client configstring (which should not produce any error)
		SV_SetConfigstring(CS_PLAYERS + num, configstring);
	}
}

/**
 * @brief Read a demo client userinfo string from a message, load it into memory, fills client_t fields by parsing the userinfo and broacast the change to the gamecode and clients
 * @param msg
 *
 * @note This function should be called before SV_DemoReadClientConfigString on demo playback start and every first client connection to the server
		 because userinfo is set right on connect to the server and configstring derives from it, client slots are reused so no need to worry about going over MAX_CLIENTS.
 */
static void SV_DemoReadClientUserinfo(msg_t *msg)
{
	sharedEntity_t *entity;
	client_t       *client;
	char           *userinfo;
	int            num;

	// Get client
	num    = MSG_ReadByte(msg);
	client = &svs.clients[num];
	// Get userinfo
	userinfo = MSG_ReadString(msg);

	client->demoClient = qtrue;

	entity = SV_GentityNum(num);

	// Set the remoteAddress of this client to localhost (this will set "ip\localhost" in the userinfo, which will in turn force the gamecode to set this client as a localClient, which avoids inactivity timers and some other stuffs to be triggered)
	if (strlen(userinfo)) // we check that the client isn't being dropped (in which case we shouldn't set an address)
	{
		NET_StringToAdr("localhost", &client->netchan.remoteAddress, NA_LOOPBACK);
		entity->r.svFlags |= SVF_BOT; //Set player as a "bot" so the empty IP and GUID will not cause a kick
	}

	// Update the userinfo for both the server and gamecode
	Cmd_TokenizeString(va("userinfo \"%s\"", userinfo));   // we need to prepend the userinfo command (or in fact any word) to tokenize the userinfo string to the second index because SV_UpdateUserinfo_f expects to fetch it with Argv(1)
	SV_UpdateUserinfo_f(client); // will update the server userinfo, automatically fill client_t fields and then transmit to the gamecode and call ClientUserinfoChanged() which will also update the gamecode's client_t from the new userinfo (eg: name [server-side] and netname [gamecode-side] will be both updated)

	// Differentiate between players and bots to properly replay gamestates
	if (strlen(userinfo) && strlen(Info_ValueForKey(userinfo, "cg_etVersion")))
	{
		entity->r.svFlags &= ~SVF_BOT;
	}
}

/**
 * @brief Read the usercmd_t for a democlient and restituate the movements
 *
 * @details This is NOT needed to make democlients move, this is handled by entities management,
 * but it should avoid inactivity timer to activate and can be used for demo analysis
 *
 * @param[in] msg
 *
 * @todo FIXME: should set ucmd->serverTime = client->ps.commandTime + 2 to avoid the dropping of the usercmds_t packets, see g_active.c
 */
/*
static void SV_DemoReadClientUsercmd( msg_t *msg )
{
    int num, deltaint;
    qboolean delta;

    Cmd_SaveCmdContext(); // Save the context (tokenized strings) so that the engine can continue its usual processing normally just after this function
    num = MSG_ReadByte(msg);
    deltaint = MSG_ReadByte(msg);
    if (deltaint)
        delta = qtrue;
    else
        delta = qfalse;
    //Com_Printf("DebugGBOReadusercmd: %i\n", num);
    SV_UserMove(&svs.clients[num], msg, delta);
    Cmd_RestoreCmdContext(); // Restore the context (tokenized strings)
}
*/

/**
 * @brief Read all democlients playerstate (playerState_t) from a message and store them in a demoPlayerStates array (it will be loaded in memory later when SV_DemoReadRefresh() is called)
 * @param[in] msg
 */
static void SV_DemoReadAllPlayerState(msg_t *msg)
{
	playerState_t *player;
	int           num;

	num = MSG_ReadByte(msg);

	if (num < 0 || num >= MAX_CLIENTS)
	{
		SV_DemoPlaybackError("SV_DemoReadAllPlayerState: invalid demo message!");
	}

	player = SV_GameClientNum(num);
	MSG_ReadDeltaPlayerstate(msg, &sv.demoPlayerStates[num], player);
	sv.demoPlayerStates[num] = *player;
}

/**
 * @brief Read all entities state (gentity_t or sharedEntity_t->entityState_t) from a message and store them in a demoEntities[num].s array (it will be loaded in memory later when SV_DemoReadRefresh() is called)
 * @param[in] msg
 */
static void SV_DemoReadAllEntityState(msg_t *msg)
{
	sharedEntity_t *entity;
	int            num;

	// For each state in this message (because we store a concatenation of all entities states in a single message, we could do otherwise, but that's the design)
	while (1)
	{
		// Get entity num
		num = MSG_ReadBits(msg, GENTITYNUM_BITS);

		// Reached the end of the message, stop here
		if (num == ENTITYNUM_NONE)
		{
			break;
		}

		// Create a blank entity
		entity = SV_GentityNum(num);
		// Interpolate the new entity state from previous state in sv.demoEntities
		MSG_ReadDeltaEntity(msg, &sv.demoEntities[num].s, &entity->s, num);

		// Fix mover movements, (avoid the "bad moverstate" error)
		if (entity->s.eType == ET_MOVER)
		{
			// 1st way to fix: completely disable movers (but not their displacement effects, so they will be invisible, but players are still moved by movers)
			//entity->s.eType = ET_GENERAL;

			// 2nd way to fix (better): only binarymovers are producing the bug, because the game will be confused about the moverState since we cannot have access nor set it...
			// the solution: avoid changing the moverState, only change the position. To do this, avoid calls to ent->reached, so in other words never allow s.pot.trType to be TR_LINEAR_STOP (other types of movers are not affected since they don't have a stop/reached state, they are just looping over and over)
			if (entity->s.pos.trType == TR_LINEAR_STOP)  // mover reached end of movement? change it...
			{
				entity->s.pos.trType = TR_LINEAR;  // ... to always set movers in a moving linear state.
			}

			// 'Do_Activate_f' from 'g_cmds.c' is not executed when replaying demo thus causing crashes with doors because 'Use_BinaryMover' is not used, but 'Reached_BinaryMover' is.
			// 'MOVER_POS1ROTATE' -> 'Use_BinaryMover' -> 'MOVER_1TO2ROTATE' -> 'Reached_BinaryMover' -> 'MOVER_POS2ROTATE' -> 'ReturnToPos1Rotate' -> 'MOVER_2TO1ROTATE' -> 'Reached_BinaryMover' -> 'MOVER_POS1ROTATE'
			// moverState is not changed properly in game code, at least for movers `func_door_rotating`
			if (entity->s.apos.trType == TR_LINEAR_STOP)
			{
				entity->s.apos.trType = TR_LINEAR;
			}
		}

		if (entity->s.eType == ET_ITEM)
		{
			SV_GentityUpdateItemField(entity);
		}

		// Save new entity state (in sv.demoEntities, which in other words display the new state)
		sv.demoEntities[num].s = entity->s;
	}
}

/**
 * @brief Read all shared entities (gentity_t or sharedEntity_t->entityShared_t
 * @param[in] msg
 *
 * @note sharedEntity_t = gentity_t != entityShared_t which is a subfield of gentity_t) from a message and store them in a demoEntities[num].r array (it will be loaded in memory later when SV_DemoReadRefresh() is called)
 */
static void SV_DemoReadAllEntityShared(msg_t *msg)
{
	sharedEntity_t *entity;
	int            num;

	while (1) // loop until we read the whole message (which is storing several states)
	{
		// Get entity num
		num = MSG_ReadBits(msg, GENTITYNUM_BITS);

		// Reached end of message, stop here
		if (num == ENTITYNUM_NONE)
		{
			break;
		}

		// Load an empty entity
		entity = SV_GentityNum(num);
		// Interpolate the new entity state from previous state in sv.demoEntities
		MSG_ReadDeltaSharedEntity(msg, &sv.demoEntities[num].r, &entity->r, num);

		// Link/unlink the entity
		if (entity->r.linked && (!sv.demoEntities[num].r.linked ||
		                         entity->r.linkcount != sv.demoEntities[num].r.linkcount))
		{
			SV_LinkEntity(entity);
		}
		else if (!entity->r.linked && sv.demoEntities[num].r.linked)
		{
			SV_UnlinkEntity(entity);
		}

		// Save the new state in sv.demoEntities (ie, display current entity state)
		sv.demoEntities[num].r = entity->r;
		if (num > sv.num_entities)
		{
			sv.num_entities = num;
		}
	}
}

/**
 * @brief Load into memory all stored demo players states and entities (which effectively overwrites the one that were previously written by the game since SV_ReadFrame is called at the very end of every game's frame iteration).
 */
static void SV_DemoReadRefreshEntities(void)
{
	int            i;
	sharedEntity_t *entity;

	for (i = 0; i < sv_democlients->integer; i++)
	{
		// This is a workaround to not use user commands to save storage space, ClientThink is needed for map script triggers to be run
		// fe. EndRound script which starts G_LogExit
		if (Q_atoi((Info_ValueForKey(sv.configstrings[CS_WOLFINFO], "gamestate"))) != GS_INTERMISSION && !(SV_GameClientNum(i)->eFlags & EF_DEAD))
		{
			usercmd_t cmd;

			Com_Memset(&cmd, 0, sizeof(cmd));
			cmd.serverTime = svs.time;

			SV_ClientThink(&svs.clients[i], &cmd);
		}

		*SV_GameClientNum(i) = sv.demoPlayerStates[i]; // Overwrite player states
	}

	// Overwrite anything the game may have changed
	for (i = 0; i < sv.num_entities; i++)
	{
		if (i >= sv_democlients->integer && i < MAX_CLIENTS) // FIXME? shouldn't MAX_CLIENTS be sv_maxclients->integer?
		{
			continue;
		}

		*SV_GentityNum(i) = sv.demoEntities[i]; // Overwrite entities

		entity = SV_GentityNum(i);

		if (entity->s.eType == ET_FLAMETHROWER_CHUNK)
		{
			SV_GentityUpdateParentField(entity, SV_GentityNum(entity->r.ownerNum));
		}
	}
}

/**
 * @brief Update all demoplayers health (will be reflected on the HUD when spectated - for team overlay see tinfo clientcommand)
 *
 * @note This function should always be called after SV_DemoReadRefreshEntities() (or you can also change SV_GameClientNum(i) to sv.demoPlayerStates[i],
 * but make sure the entity already has updated its health state...)
 */
void SV_DemoReadRefreshPlayersHealth(void)
{
	int i;

	// Update all players' health in HUD
	for (i = 0; i < sv_democlients->integer; i++)
	{
		SV_GentityUpdateHealthField(SV_GentityNum(i), SV_GameClientNum(i));   // Update the health with the value of playerState_t->stats[STAT_HEALTH]
	}
}

/**
 * @brief Play a frame from the demo file
 *
 * @details This function will read one frame per call, and will process every events contained (switch to the next event when meeting the demo_EOF marker)
 * until it meets the end of the frame (demo_endDemo marker).
 *
 * Called in the main server's loop SV_Frame() in sv_main.c (it's called after any other event processing,
 * so that it overwrites anything the game may have loaded into memory, but before the entities are broadcasted to the clients)
 */
qboolean SV_DemoReadFrame(void)
{
	msg_t msg;
	int   cmd, r, time;

	static int memsvtime;
	static int currentframe = -1;

read_next_demo_frame: // used to read another whole demo frame

	// Demo freezed? Just stop reading the demo frames
	if (Cvar_VariableIntegerValue("sv_freezeDemo"))
	{
		svs.time = memsvtime; // reset server time to the same time as the previous frame, to avoid the time going backward when resuming the demo (which will disconnect every players)
		return qfalse;
	}

	// Update timescale
	currentframe++; // update the current frame number

	if (com_timescale->value < 1.0f && com_timescale->value > 0.0f)
	{
		// Check timescale: if slowed timescale (below 1.0), then we check that we pass one frame on 1.0/com_timescale (eg: timescale = 0.5, 1.0/0.5=2, so we pass one frame on two)
		if (currentframe % (int)(1.0f / com_timescale->value) != 0)
		{ // if it's not yet the right time to read the frame, we just pass and wait for the next server frame to read this demo frame
			return qfalse;
		}
	}

	// Initialize / reinitialize the msg buffer
	MSG_Init(&msg, buf, sizeof(buf));

	while (1)
	{
read_next_demo_event: // used to read next demo event

		MSG_BeginReading(&msg);

		// Get a message
		r = FS_Read(&msg.cursize, 4, sv.demoFile);

		if (r != 4)
		{
			SV_DemoPlaybackError("DEMOERROR: SV_DemoReadFrame: Demo file is corrupted");
		}
		msg.cursize = LittleLong(msg.cursize); // get the size of the next demo message

		if (msg.cursize > msg.maxsize) // if the size is too big, we throw an error
		{
			SV_DemoPlaybackError("DEMOERROR: SV_DemoReadFrame: demo message too long\n");
		}

		r = FS_Read(msg.data, msg.cursize, sv.demoFile); // fetch the demo message (using the length we got) from the demo file sv.demoFile, and store it into msg.data (will be accessed automatically by MSG_thing() functions), and store in r the length of the data returned (used to check that it's correct)
		if (r != msg.cursize) // if the returned length of the read demo message is not the same as the length we expected (the one that was stored just prior to the demo message), we return an error because we miss the demo message, and the only reason is that the file is truncated, so there's nothing to read after
		{
			SV_DemoPlaybackError("DEMOERROR: Demo file was truncated.");
		}

		// Parse the message
		while (1)
		{
			cmd = MSG_ReadByte(&msg); // Read the demo message marker

			switch (cmd) // switch to the right processing depending on the type of the marker
			{
			default:
				// Error tolerance mode: if we encounter an unknown demo message, we just skip to the next (this may allow for retrocompatibility)
				if (sv_demoTolerant->integer)
				{
					MSG_Clear(&msg);
					goto read_next_demo_event;
				}
				else
				{     // else we just drop the demo and throw a big fat error
					SV_DemoPlaybackError(va("SV_DemoReadFrame: Illegible demo message [%i %i:%i]", cmd, msg.readcount, msg.cursize));
				}
			case demo_EOF:     // end of a demo event (the loop will continue to real the next event)
				MSG_Clear(&msg);
				goto read_next_demo_event;
			case demo_configString:     // general configstrings setting (such as capture scores CS_SCORES1/2, etc.) - except clients configstrings
				SV_DemoReadConfigString(&msg);
				break;
			case demo_clientConfigString:     // client configstrings setting and clients status management
				SV_DemoReadClientConfigString(&msg);
				break;
			case demo_clientUserinfo:     // client userinfo setting and client_t fields management
				SV_DemoReadClientUserinfo(&msg);
				break;
			case demo_clientCommand:     // client command management (generally automatic, such as tinfo for HUD team overlay status, team selection, etc.) - except userinfo command that is managed by another event
				SV_DemoReadClientCommand(&msg);
				break;
			case demo_serverCommand:     // server command management - except print/cp (already handled by gameCommand),
				SV_DemoReadServerCommand(&msg);
				break;
			case demo_gameCommand:     // game command management - such as prints/centerprint (cp) scores command - except chat/tchat (handled by clientCommand) - basically the same as demo_serverCommand (because sv_GameSendServerCommand uses SV_SendServerCommand, but game commands are safe to be replayed to everyone, while server commands may be unsafe such as disconnect)
				SV_DemoReadGameCommand(&msg);
				break;
			case demo_playerState:     // manage playerState_t (some more players game status management, see demo_endFrame)
				SV_DemoReadAllPlayerState(&msg);
				break;
			case demo_entityState:     // manage gentity->entityState_t (some more gentities game status management, see demo_endFrame)
				SV_DemoReadAllEntityState(&msg);
				break;
			case demo_entityShared:     // gentity_t->entityShared_t management (see g_local.h for more infos)
				SV_DemoReadAllEntityShared(&msg);
				break;
			/*
			case demo_clientUsercmd:
			    SV_DemoReadClientUsercmd(&msg);
			    break;
			*/
			case -1: // no more chars in msg FIXME: inspect!
				Com_DPrintf("SV_DemoReadFrame: no chars [%i %i:%i]\n", cmd, msg.readcount, msg.cursize);
				return qfalse;
			case demo_endFrame:     // end of the frame - players and entities game status update: we commit every demo entity to the server, update the server time, then release the demo frame reading here to the next server (and demo) frame
				Com_DPrintf(" END OF FRAME \n");

				// Update entities
				SV_DemoReadRefreshEntities();     // load into memory the demo entities (overwriting any change the game may have done)
				// Update all players' health in HUD
				SV_DemoReadRefreshPlayersHealth();
				// Set the server time
				time = MSG_ReadLong(&msg);

				// Iterate a few demo frames to catch to until we are above the server time,
				// Note: this is needed after map_restart (SV_MapRestart_f) and to correctly replay gamestates (when the recording started at GS_WARMUP_COUNTDOWN and GS_PLAYING)
				if (time < svs.time)
				{
					// Note: having a server time below the demo time is CRITICAL, else we may send to the clients a server time that is below the previous,
					// or in the case of legacy mod the time would be set to levelTime = levelTime + level.previousTime (G_RunFrame).
					int timetoreach = svs.time;

					svs.time = time;
					while (svs.time <= timetoreach)
					{
						SV_DemoReadFrame(); // run a few frames to settle things out
					}
				}
				else
				{
					svs.time  = time;   // refresh server in-game time (overwriting any change the game may have done)
					memsvtime = svs.time;     // keep memory of the last server time, in case we want to freeze the demo
				}

				// Check for timescale: if timescale is faster (above 1.0), we read more frames at once (eg: timescale=2, we read 2 frames for one call of this function)
				if (com_timescale->value > 1.0f)
				{
					// Check that we've read all the frames we needed
					if (currentframe % (int)(com_timescale->value) != 0)
					{
						goto read_next_demo_frame;     // if not true, we read another frame
					}
				}

				return qfalse;     // else we end the current demo frame
			case demo_endDemo:     // end of the demo file - just stop playback and restore saved cvars
				Com_Printf("End of demo reached.\n");
				SV_DemoStopPlayback();
				return qtrue;
			}
		}
	}
	return qfalse;
}

/**
 * @brief SV_DemoStopAll
 */
void SV_DemoStopAll(void)
{
	// stop any demos
	if (sv.demoState == DS_RECORDING)
	{
		SV_DemoStopRecord();
	}
	else if (sv.demoState == DS_PLAYBACK)
	{
		SV_DemoStopPlayback();
	}
}

/**
 * @brief SV_Demo_Record_f
 */
static void SV_Demo_Record_f(void)
{
	// make sure server is running
	if (!com_sv_running->integer)
	{
		Com_Printf("Server is not running.\n");
		return;
	}

	if (Cmd_Argc() > 2)
	{
		Com_Printf("Usage: demo_record <demoname>\n");
		return;
	}

	if (sv.demoState != DS_NONE)
	{
		Com_Printf("A demo is already being recorded/played. Use demo_stop and retry.\n");
		return;
	}

	if (sv_maxclients->integer > MAX_CLIENTS)
	{
		Com_Printf("DEMO: ERROR: Too many client slots, reduce sv_maxclients and retry.\n");
		return;
	}

	if (Cmd_Argc() == 2)
	{
		Com_sprintf(sv.demoName, sizeof(sv.demoName), "svdemos/%s.%s%d", Cmd_Argv(1), SVDEMOEXT, PROTOCOL_VERSION);
	}
	else
	{
		int number = 0;
		// scan for a free demo name
		while (1)
		{
			Com_sprintf(sv.demoName, sizeof(sv.demoName), "svdemos/%d.%s%d", number++, SVDEMOEXT, PROTOCOL_VERSION);
			if (!FS_FileExists(sv.demoName))
			{
				break;  // file doesn't exist
			}
		}
	}

	sv.demoFile = FS_FOpenFileWrite(sv.demoName);
	if (!sv.demoFile)
	{
		Com_Printf("DEMO: ERROR: Couldn't open %s for writing.\n", sv.demoName);
		return;
	}

	SV_DemoStartRecord();
}

/**
 * @brief SV_Demo_Play_f
 */
static void SV_Demo_Play_f(void)
{
	char *arg;

	if (Cmd_Argc() != 2)
	{
		Com_Printf("Usage: demo_play <demoname>\n");
		return;
	}

	if (sv.demoState != DS_NONE && sv.demoState != DS_WAITINGPLAYBACK)
	{
		Com_Printf("A demo is already being recorded/played. Use demo_stop and retry.\n");
		return;
	}

	// check for an extension .svdm_?? (?? is protocol)
	arg = Cmd_Argv(1);
	if (!strcmp(arg + strlen(arg) - 6, va(".%s%d", SVDEMOEXT, PROTOCOL_VERSION)))
	{
		Com_sprintf(sv.demoName, sizeof(sv.demoName), "svdemos/%s", arg);
	}
	else
	{
		Com_sprintf(sv.demoName, sizeof(sv.demoName), "svdemos/%s.%s%d", arg, SVDEMOEXT, PROTOCOL_VERSION);
	}

	//FS_FileExists(sv.demoName);
	FS_FOpenFileRead(sv.demoName, &sv.demoFile, qtrue);
	if (!sv.demoFile)
	{
		Com_Printf("ERROR: Couldn't open %s for reading.\n", sv.demoName);
		return;
	}

	SV_DemoStartPlayback();
}

/**
 * @brief SV_Demo_Stop_f
 */
static void SV_Demo_Stop_f(void)
{
	if (sv.demoState == DS_NONE)
	{
		Com_Printf("No demo is currently being recorded or played.\n");
		return;
	}

	SV_DemoStopAll();
}

/**
 * @brief SV_CompleteDemoName
 * @param args - unused
 * @param[in] argNum
 */
static void SV_CompleteDemoName(char *args, int argNum)
{
	if (argNum == 2)
	{
		char demoExt[16];

		Com_sprintf(demoExt, sizeof(demoExt), ".%s%d", SVDEMOEXT, PROTOCOL_VERSION);
		Field_CompleteFilename("svdemos", demoExt, qtrue, qtrue);
	}
}

/**
* @brief SV_Demo_Fastforward_f
*
* @details Fast-forward demo recording but stop when gamestate changes so it's easy to skip some parts like warmup.
*
*/
static void SV_Demo_Fastforward_f(void)
{
	int timetoreach, gamestate;

	if (Cmd_Argc() != 2)
	{
		Com_Printf("Usage: demo_ff <seconds>\n");
		return;
	}

	if (sv.demoState != DS_PLAYBACK)
	{
		Com_Printf("No demo is currently being played.\n");
		return;
	}

	timetoreach = Q_atoi(Cmd_Argv(1));

	if (timetoreach <= 0)
	{
		Com_Printf("Bad argument.\n");
		return;
	}

	timetoreach = svs.time + (timetoreach * 1000);
	gamestate   = Q_atoi(Info_ValueForKey(sv.configstrings[CS_WOLFINFO], "gamestate"));

	while (svs.time < timetoreach)
	{
		if (SV_DemoReadFrame() || gamestate != Q_atoi(Info_ValueForKey(sv.configstrings[CS_WOLFINFO], "gamestate")))
		{
			break;
		}
	}
}

/**
 * @brief SV_DemoInit
 */
void SV_DemoInit(void)
{
	Cmd_AddCommand("demo_record", SV_Demo_Record_f, "Starts demo recording.");
	Cmd_AddCommand("demo_play", SV_Demo_Play_f, "Plays a demo record.", SV_CompleteDemoName);
	Cmd_AddCommand("demo_stop", SV_Demo_Stop_f, "Stops a demo record.");
	Cmd_AddCommand("demo_ff", SV_Demo_Fastforward_f, "Fast-forwards a demo record.");
}
