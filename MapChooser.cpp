#include <stdio.h>
#include "MapChooser.h"
#include "metamod_oslink.h"

MapChooser g_MapChooser;
PLUGIN_EXPOSE(MapChooser, g_MapChooser);

IUtilsApi* g_pUtils;
IMenusApi* g_pMenus;
IPlayersApi* g_pPlayers;

INetworkGameServer *g_pNetworkGameServer = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
IVEngineServer2* engine = nullptr;

std::map<std::string, std::string> g_vecPhrases;

struct RTV_Settings {
	int iMinPlayers;
	int iPlayersPercent;
	bool bType;
	int iVoteTime;
	std::string sVoteCommands;
} g_RTVSettings;

struct Settings {
	int iType;
	bool bCurrentMap;
	int iTimeEnd;
	int iTimeEndRounds;
	int iMapCount;
	int iTimeoutStartMap;
	int iTimeoutBeforeVote;
	std::string sNominationCommands;
} g_Settings;

bool g_bVote[64];
int g_iVotes[64];
int g_iNominationVotes[64];
int g_iLastChange;
bool g_bActive = false;
int g_iFinalizeVote = -1;
bool g_bVoteForce = false;
bool g_bFinalized = false;

std::vector<std::pair<std::string, std::string>> g_vecMaps;

ConVarRefAbstract* mp_timelimit;
ConVarRefAbstract* mp_maxrounds;

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils->GetCGameEntitySystem();
}

int GetCountVotes()
{
	int iCount = 0;
	for (int i = 0; i < 64; i++)
	{
		if(g_pPlayers->IsFakeClient(i)) continue;
		if (g_bVote[i]) iCount++;
	}
	return iCount;
}

int GetPlayersCount()
{
	int iCount = 0;
	for (int i = 0; i < 64; i++)
	{
		if(g_pPlayers->IsFakeClient(i)) continue;
		iCount++;
	}
	return iCount;
}

int GetNeedVotes()
{
	int iPlayersCount = GetPlayersCount();
	return ceil((iPlayersCount * g_RTVSettings.iPlayersPercent) / 100.0);
}

const char* GetTranslation(const char* szKey)
{
	auto it = g_vecPhrases.find(szKey);
	if (it == g_vecPhrases.end()) return szKey;
	return it->second.c_str();
}

bool MapChooser::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);

	g_SMAPI->AddListener( this, this );
	
	mp_timelimit = new ConVarRefAbstract("mp_timelimit");
	mp_maxrounds = new ConVarRefAbstract("mp_maxrounds");
	
	ConVar_Register(FCVAR_GAMEDLL);

	return true;
}

bool MapChooser::Unload(char *error, size_t maxlen)
{
	ConVar_Unregister();
	
	return true;
}

void LoadConfigSettings()
{
	KeyValues* hKvSettings = new KeyValues("Settings");
	const char *pszPath = "addons/configs/map_chooser/settings.ini";

	if (!hKvSettings->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		g_pUtils->ErrorLog("[%s] Failed to load rtv settings from %s", g_MapChooser.GetLogTag(), pszPath);
		return;
	}

	g_RTVSettings.iMinPlayers = hKvSettings->GetInt("rtv_min_players", 4);
	g_RTVSettings.iPlayersPercent = hKvSettings->GetInt("rtv_players_percent", 50);
	g_RTVSettings.bType = hKvSettings->GetBool("rtv_type", true);
	g_RTVSettings.iVoteTime = hKvSettings->GetInt("rtv_vote_time", 30);
	g_RTVSettings.sVoteCommands = hKvSettings->GetString("rtv_commands", "!rtv,rtv");

	g_Settings.bCurrentMap = hKvSettings->GetBool("vote_current_map", true);
	g_Settings.iMapCount = hKvSettings->GetInt("vote_map_count", 5);
	g_Settings.iTimeEnd = hKvSettings->GetInt("end_map_vote", 300);
	g_Settings.iTimeEndRounds = hKvSettings->GetInt("end_map_vote_round", 3);
	g_Settings.iType = hKvSettings->GetInt("end_map_vote_type", 0);
	g_Settings.iTimeoutStartMap = hKvSettings->GetInt("timeout_start_map", 120);
	g_Settings.iTimeoutBeforeVote = hKvSettings->GetInt("timeout_before_vote", 5);
	g_Settings.sNominationCommands = hKvSettings->GetString("nomination_commands", "!nominate,nominate");
}

void LoadConfigMaps()
{
	KeyValues* kvMaps = new KeyValues("Maps");
	const char *pszPath = "addons/configs/map_chooser/maplist.ini";

	if (!kvMaps->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		g_pUtils->ErrorLog("[%s] Failed to load map list from %s", g_MapChooser.GetLogTag(), pszPath);
		return;
	}

	FOR_EACH_VALUE(kvMaps, pValue)
	{
		g_vecMaps.push_back({std::string(pValue->GetName()), std::string(pValue->GetString(nullptr, nullptr))});
	}
}

void LoadTranslations()
{
	KeyValues::AutoDelete g_kvPhrases("Phrases");

	if (!g_kvPhrases->LoadFromFile(g_pFullFileSystem, "addons/translations/map_chooser.phrases.txt"))
	{
		Msg("[%s] Failed to load addons/translations/map_chooser.phrases.txt", g_PLAPI->GetLogTag());
		return;
	}

	for (KeyValues *pKey = g_kvPhrases->GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey())
		g_vecPhrases[std::string(pKey->GetName())] = std::string(pKey->GetString(g_pUtils->GetLanguage()));
}

void ChangeLevel()
{
	if(g_iFinalizeVote < 0 || g_iFinalizeVote >= g_vecMaps.size()) {
		g_pUtils->PrintToChatAll(GetTranslation("VoteFailed"));
		return;
	}
	
	char szBuffer[256], szMap[128];
	if(g_iFinalizeVote >= 0) {
		g_SMAPI->Format(szMap, sizeof(szMap), "%s", g_vecMaps[g_iFinalizeVote].second.c_str());
	} else {
		g_SMAPI->Format(szMap, sizeof(szMap), "%s", g_pUtils->GetCGlobalVars()->mapname);
	}

	g_pUtils->PrintToChatAll(GetTranslation("ChangingLevel"), szMap);
	
	if(!engine->IsMapValid(szMap)) {
		g_SMAPI->Format(szBuffer, sizeof(szBuffer), "ds_workshop_changelevel %s", szMap);
		g_pUtils->CreateTimer(g_Settings.iTimeoutBeforeVote, [szBuffer]() {
			engine->ServerCommand(szBuffer);
			return -1.0f;
		});
	} else {
		g_SMAPI->Format(szBuffer, sizeof(szBuffer), "%s", szMap);
		g_pUtils->CreateTimer(g_Settings.iTimeoutBeforeVote, [szBuffer]() {
			engine->ChangeLevel(szBuffer, nullptr);
			return -1.0f;
		});
	}
}

void FinalizeVote()
{
	int iVotes[64] = {0};
	for (int i = 0; i < 64; i++)
	{
		if(g_pPlayers->IsFakeClient(i)) continue;
		if(g_iVotes[i] >= 0 && g_iVotes[i] < g_vecMaps.size()) {
			iVotes[g_iVotes[i]]++;
		}
	}
	int iMaxVotes = 0;
	int iMaxIndex = -1;
	for (size_t i = 0; i < g_vecMaps.size(); i++)
	{
		if(iVotes[i] > iMaxVotes) {
			iMaxVotes = iVotes[i];
			iMaxIndex = i;
		}
	}
	if(iMaxIndex >= 0 && iMaxVotes > 0) {
		g_pUtils->PrintToChatAll(GetTranslation("VoteEnded"), 
			g_vecMaps[iMaxIndex].first.c_str(), iMaxVotes);
	} else {
		g_pUtils->PrintToChatAll(GetTranslation("VoteFailed"));
	}
	g_iFinalizeVote = iMaxIndex;
	g_bFinalized = true;
	if(g_bVoteForce || g_Settings.iType == 0) ChangeLevel();
}

void StartVote(bool bForce = false)
{
	g_bVoteForce = bForce;
	for (int i = 0; i < 64; i++)
	{
		g_bVote[i] = false;
		g_iVotes[i] = -1;
	}
	g_bActive = false;
	if(!g_RTVSettings.bType && bForce) {
		g_pUtils->PrintToChatAll(GetTranslation("VoteStarted"), g_RTVSettings.iVoteTime);
		g_pUtils->CreateTimer(g_RTVSettings.iVoteTime, [](){
			FinalizeVote();
			return -1.0f;
		});
	}
	if(!bForce) {
		g_pUtils->CreateTimer(g_RTVSettings.iVoteTime, [](){
			FinalizeVote();
			return -1.0f;
		});
	}

	Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, GetTranslation("VoteMenuTitle"));
	if(g_Settings.bCurrentMap) g_pMenus->AddItemMenu(hMenu, "-1", GetTranslation("VoteMenuCurrentMap"));
	int iCount = 0;
	char szBuffer[64];
	g_SMAPI->Format(szBuffer, sizeof(szBuffer), "%s", g_pUtils->GetCGlobalVars()->mapname);

	std::map<int, int> mapNominationVotes;
	for (int i = 0; i < 64; i++)
	{
		if(g_pPlayers->IsFakeClient(i)) continue;
		if(g_iNominationVotes[i] >= 0 && g_iNominationVotes[i] < g_vecMaps.size()) {
			mapNominationVotes[g_iNominationVotes[i]]++;
		}
	}
	//Сортируем номинации по количеству голосов
	std::vector<std::pair<int, int>> vecNominationVotes(mapNominationVotes.begin(), mapNominationVotes.end());
	std::sort(vecNominationVotes.begin(), vecNominationVotes.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
		return a.second > b.second;
	});

	for (const auto& nomination : vecNominationVotes)
	{
		if(g_Settings.iMapCount != -1 && iCount >= g_Settings.iMapCount) break;
		if(g_vecMaps[nomination.first].second == std::string(szBuffer)) continue;
		g_pMenus->AddItemMenu(hMenu, std::to_string(nomination.first).c_str(), g_vecMaps[nomination.first].first.c_str());
		iCount++;
	}

	for (size_t i = 0; i < g_vecMaps.size(); i++)
	{
		if(g_Settings.iMapCount != -1 && iCount >= g_Settings.iMapCount) break;
		if(g_vecMaps[i].second == std::string(szBuffer)) continue;
		g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), g_vecMaps[i].first.c_str());
		iCount++;
	}

	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetBackMenu(hMenu, false);
	g_pMenus->SetCallback(hMenu, [](const char* szBack, const char* szFront, int iItem, int iSlot) {
		if(iItem < 7) {
			int iMap = std::atoi(szBack);
			g_iVotes[iSlot] = iMap;
			if(iMap != -1) {
				g_pUtils->PrintToChatAll(GetTranslation("PlayerVoteMap"), 
					g_pPlayers->GetPlayerName(iSlot), 
					g_vecMaps[iMap].first.c_str());
			} else {
				g_pUtils->PrintToChatAll(GetTranslation("PlayerVoteMapCurrent"), 
					g_pPlayers->GetPlayerName(iSlot));
			}
			g_pMenus->ClosePlayerMenu(iSlot);
		}
	});
	for (int i = 0; i < 64; i++)
	{
		if(g_pPlayers->IsFakeClient(i)) continue;
		g_pMenus->DisplayPlayerMenu(hMenu, i, true, true);
	}
}

void OnStartupServer()
{
	g_pEntitySystem = GameEntitySystem();
	g_iLastChange = std::time(nullptr) + g_Settings.iTimeoutStartMap;
	g_bActive = true;
	g_iFinalizeVote = -1;
	g_bFinalized = false;
	g_bVoteForce = false;

	for (int i = 0; i < 64; i++)
	{
		g_bVote[i] = false;
		g_iVotes[i] = -1;
		g_iNominationVotes[i] = -1;
	}
}

void OnCSWinPanelMatch(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	if(!pEvent) return;
	if(!g_bActive) {
		if(!g_bFinalized) FinalizeVote();
		ChangeLevel();
	}
}

void OnRoundEnd(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	if (!pEvent) return;

	if (!g_bActive) {
		if(g_bVoteForce && g_RTVSettings.bType && !g_bFinalized) FinalizeVote();
		else if(!g_bVoteForce && g_Settings.iType == 1 && g_bFinalized) {
			ChangeLevel();
		}
	}
}

std::vector<std::string> split(std::string s, std::string delimiter) {
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr (pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back (token);
    }

    res.push_back (s.substr (pos_start));
    return res;
}

void NominationMenu(int iSlot)
{
	char szBuffer[64];
	g_SMAPI->Format(szBuffer, sizeof(szBuffer), "%s", g_pUtils->GetCGlobalVars()->mapname);

	Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, GetTranslation("NominationMenuTitle"));
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetBackMenu(hMenu, false);
	g_pMenus->SetCallback(hMenu, [](const char* szBack, const char* szFront, int iItem, int iSlot) {
		if(iItem < 7) {
			int iMap = std::atoi(szBack);
			if(iMap >= 0 && iMap < g_vecMaps.size()) {
				g_iNominationVotes[iSlot] = iMap;
				g_pUtils->PrintToChatAll(GetTranslation("PlayerNominateMap"), 
					g_pPlayers->GetPlayerName(iSlot), 
					g_vecMaps[iMap].first.c_str());
			}
			g_pMenus->ClosePlayerMenu(iSlot);
		}
	});
	for (size_t i = 0; i < g_vecMaps.size(); i++)
	{
		if(g_vecMaps[i].first == std::string(szBuffer)) continue;
		g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), g_vecMaps[i].first.c_str());
	}
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void MapChooser::AllPluginsLoaded()
{
	char error[64];
	int ret;

	g_pUtils = (IUtilsApi *)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		V_strncpy(error, "Missing Utils system plugin", 64);
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}

	g_pPlayers = (IPlayersApi *)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		V_strncpy(error, "Missing Players system plugin", 64);
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	
	g_pMenus = (IMenusApi *)g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		V_strncpy(error, "Missing Menus system plugin", 64);
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}

	LoadTranslations();
	LoadConfigMaps();
	LoadConfigSettings();
	g_pUtils->StartupServer(g_PLID, OnStartupServer);
	if(g_Settings.iType == 2) g_pUtils->HookEvent(g_PLID, "cs_win_panel_match", OnCSWinPanelMatch);
	
	g_pUtils->HookEvent(g_PLID, "round_end", OnRoundEnd);
	g_pUtils->HookEvent(g_PLID, "round_start", [](const char* szName, IGameEvent* pEvent, bool bDontBroadcast) {
		if (!pEvent) return;
		if (mp_maxrounds->GetInt() <= 0 || g_Settings.iTimeEndRounds <= 0) return;
		int iRound = g_pUtils->GetCCSGameRules()->m_totalRoundsPlayed();
		int iMaxRounds = mp_maxrounds->GetInt();
		if (iRound > 0 && iRound <= iMaxRounds && (iMaxRounds - iRound) <= g_Settings.iTimeEndRounds && g_bActive) StartVote();
	});

	std::vector<std::string> vecCommands = split(g_RTVSettings.sVoteCommands.empty() ? "!rtv,rtv" : g_RTVSettings.sVoteCommands, ",");
	std::vector<std::string> vecNominationCommands = split(g_Settings.sNominationCommands.empty() ? "!nominate,nominate" : g_Settings.sNominationCommands, ",");
	g_pUtils->RegCommand(g_PLID, {"mm_nominate"}, vecNominationCommands, [](int iSlot, const char* szContent){
		if(!g_bActive) {
			g_pUtils->PrintToChat(iSlot, GetTranslation("NominationVoteIsActive"));
			return false;
		}
		if(g_iNominationVotes[iSlot] != -1) {
			g_pUtils->PrintToChat(iSlot, GetTranslation("AlreadyNominated"), 
				g_vecMaps[g_iNominationVotes[iSlot]].first.c_str());
			return false;
		}
		NominationMenu(iSlot);
		return false;
	});
	g_pUtils->RegCommand(g_PLID, {"mm_rtv"}, vecCommands, [](int iSlot, const char* szContent){
		if(!g_bActive) {
			if(g_iFinalizeVote == -1) g_pUtils->PrintToChat(iSlot, GetTranslation("VoteIsActive"));
			else g_pUtils->PrintToChat(iSlot, GetTranslation("VoteIsActiveFinalized"), g_vecMaps[g_iFinalizeVote].first.c_str());
			return false;
		}
		if(std::time(nullptr) < g_iLastChange) {
			g_pUtils->PrintToChat(iSlot, GetTranslation("VoteIsActiveTime"), 
				g_iLastChange - std::time(nullptr));
			return false;
		}
		if(GetPlayersCount() < g_RTVSettings.iMinPlayers) {
			g_pUtils->PrintToChat(iSlot, GetTranslation("MinPlayers"));
			return false;
		}
		if(g_bVote[iSlot]) {
			g_pUtils->PrintToChat(iSlot, GetTranslation("AlreadyVoted"));
			return false;
		}
		g_bVote[iSlot] = true;
		int iVotes = GetCountVotes();
		int iNeedVotes = GetNeedVotes();
		g_pUtils->PrintToChatAll(GetTranslation("PlayerVote"), 
			g_pPlayers->GetPlayerName(iSlot), 
			iVotes, iNeedVotes > 0 ? iNeedVotes : 1);
		if(iVotes >= iNeedVotes) {
			StartVote(true);
		}
		return false;
	});

	if(g_Settings.iTimeEnd > 0) {
		g_pUtils->CreateTimer(0.0f, [](){
			if(mp_timelimit->GetInt() <= 0) return 0.0f;
			int gameStart = (int)g_pUtils->GetCCSGameRules()->m_flGameStartTime();
			int timelimit = (int)(mp_timelimit->GetInt() * 60);
			int currentTime = (int)g_pUtils->GetCGlobalVars()->curtime;
			int timeleft = timelimit - (currentTime - gameStart);
			if(timeleft <= g_Settings.iTimeEnd && timeleft > 0 && g_bActive) StartVote();
			return 0.0f;
		});
	}
}

///////////////////////////////////////
const char* MapChooser::GetLicense()
{
	return "GPL";
}

const char* MapChooser::GetVersion()
{
	return "1.1";
}

const char* MapChooser::GetDate()
{
	return __DATE__;
}

const char *MapChooser::GetLogTag()
{
	return "MapChooser";
}

const char* MapChooser::GetAuthor()
{
	return "Pisex";
}

const char* MapChooser::GetDescription()
{
	return "";
}

const char* MapChooser::GetName()
{
	return "Map Chooser";
}

const char* MapChooser::GetURL()
{
	return "";
}
