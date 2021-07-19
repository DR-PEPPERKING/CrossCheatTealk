#include "../BaseIncludes.h"
#include "SDK.h"


#include "../ThirdParty/Protobuf/Headers/CrossCheatTalkNetMessages.pb.h"
//typedef int32_t CrossCheatMsgType;




#define MAX_MESSAGE_SIZE 5200
#define RATE_LIMIT_RESET_INTERVAL 60.0
#define MAX_PACKETS_PER_INTERVAL 64
#define MAX_PACKETS_TO_PROCESS 256
#define MAX_PACKETS_PER_INTERVAL_BEFORE_DISCONNECT 512
#define MAX_TIMEOUT 120.0
#define CROSSCHEATNETWORK_PORT 58
#define TIME_PER_GLOBAL_SEARCH 30.0

#pragma pack(push, 1)
struct MsgHeader_t {
	char CHEAT[6];
	uint32_t nSize;
	uint32_t SteamID;
	CrossCheatMsgType nType;
	bool bIsConnectionInit{ false };
	bool bVoice{ false }; // VoiceChannel SomeDay? SteamWorks has all the needed stuff!
};
#pragma pack(pop)


enum ConnectionState
{
	CONNSTATE_INVALID = -1,
	CONNSTATE_REQUESTED,
	CONNSTATE_RESPONSE,
};

class CrossCheatClient;
bool IsPlayerInGame(CSteamID csID);

// Example Handler
bool ChatMessage_Handler(CrossCheatClient* pClient, size_t nDataSize, const char* pMsg);

typedef bool(__cdecl* MessageHandlerFunc)(CrossCheatClient* Client, size_t nDataSize, const char* pMsg);
class SteamNetSocketsChannelMessageHandler {
public:
	virtual void ProcessMessage(int nSize, CrossCheatMsgType nType, const char* pMsg) = 0;
	void AddHandlerFuncton(CrossCheatMsgType nType, MessageHandlerFunc pFunc) {
		m_mapHandlerFunctions[nType] = pFunc;
	}
protected:
	std::map< CrossCheatMsgType, MessageHandlerFunc > m_mapHandlerFunctions;
};

class CrossCheatClient : public SteamNetSocketsChannelMessageHandler
{
public:
	CrossCheatClient() { InitializeHandlers(); }
	CrossCheatClient(CSteamID id) { m_ClientID = id; InitializeHandlers(); }
	CrossCheatClient(SteamNetworkingIdentity idRemote) { m_ClientID = idRemote.GetSteamID(); m_IdRemote = idRemote; InitializeHandlers(); }


	void InitializeHandlers()
	{
		m_mapHandlerFunctions[_ChatMessage] = &ChatMessage_Handler;
	}

	MessageHandlerFunc FindMessageHandler(CrossCheatMsgType nType)
	{
		if (m_mapHandlerFunctions.find(nType) != m_mapHandlerFunctions.end())
		{
			return m_mapHandlerFunctions[nType];
		}
		return 0;
	}




	virtual void ProcessMessage(int nSize, CrossCheatMsgType nType, const char* pMsg) 
	{ 
		MessageHandlerFunc Handler = FindMessageHandler(nType);
		if (!Handler)
			return;

		Handler(this, nSize, pMsg);
	};

	void Disconnect()
	{

	}

	bool __forceinline AreRateLimiting()
	{
		if (!(m_nNewPacketsSinceLastReset > MAX_PACKETS_PER_INTERVAL))
			return false;
		
		m_nNewPacketsSinceLastReset++;
		return true;	
	}

	bool __forceinline ShouldDisconnectSpammer()
	{
		return m_nNewPacketsSinceLastReset > MAX_PACKETS_PER_INTERVAL_BEFORE_DISCONNECT;
	}

	void __forceinline OnNewPacket()
	{
		m_dbTimeSinceLastPacket = Plat_FloatTime();
		m_nNewPacketsSinceLastReset++;
		if (m_dbLastResetTime + RATE_LIMIT_RESET_INTERVAL < Plat_FloatTime())
		{
			m_nNewPacketsSinceLastReset = 0;
		}

	}

	CSteamID GetClientSteamID()
	{
		return m_ClientID;
	}

	SteamNetworkingIdentity GetNetworkingIdentity()
	{
		return m_IdRemote;
	}

	double GetLastRecieveTime()
	{
		return m_dbTimeSinceLastPacket;
	}


	bool OurRequest()
	{
		return !m_bWeCreatedNetChannel;
	}

	void SetTheirRequest()
	{
		m_bWeCreatedNetChannel = false;
	}

	bool AllowedToProcessMessage(CrossCheatMsgType nType)
	{
		for (CrossCheatMsgType& Type : m_Priviledges)
		{
			if (nType == Type)
				return true;
		}

		return false;
	}

	void RemoveMessagePriviledge(CrossCheatMsgType nType)
	{
		int i = 0;
		for (CrossCheatMsgType& Type : m_Priviledges)
		{
			i++;
			if (nType == Type)
				break;

			if (i == m_Priviledges.size())
				return;
		}

		m_Priviledges.erase(m_Priviledges.begin() + i);
	}

	void AddMessagePriviledge(CrossCheatMsgType nType)
	{
		for (CrossCheatMsgType& Type : m_Priviledges)
		{
			if (nType == Type)
				return;
		}

		m_Priviledges.push_back(nType);
	}

private:
	CSteamID m_GameServer;
	CSteamID m_ClientID;
	SteamNetworkingIdentity m_IdRemote;
	std::vector<CrossCheatMsgType> m_Priviledges;
	int m_nNewPacketsSinceLastReset = 0;
	double m_dbTimeSinceLastPacket = 0;
	double m_dbLastResetTime = 0.0;
	bool m_bWeCreatedNetChannel = true;

};

class CrossCheatTalkNetwork
{
public:
	void OnNewFrame();

	void CullDeadClients() 
	{ 
		for (std::pair<const CSteamID, CrossCheatClient*>& Client : m_Clients)
		{
			CrossCheatClient* pClient = Client.second;
			if (!pClient)
				continue;

			if ((Plat_FloatTime() - pClient->GetLastRecieveTime()) > MAX_TIMEOUT)
			{
				DisconnectClient(pClient);
			}
		}
	};

	void Search();

	void OpenPort(int nPortToOpen)
	{
		for (int Port : m_vOpenPorts)
		{
			if (Port == nPortToOpen)
				return;
		}
		m_vOpenPorts.push_back(nPortToOpen);

	}

	bool __forceinline IsSteamIDWhiteListed(CSteamID cidClient)
	{
		if (!m_bIsUsingWhiteList)
			return true; 

		for (CSteamID& id : m_WhiteListedClients)
		{
			if (cidClient == id)
				return true;
		}
		return false;
	}

	bool __forceinline IsSteamIDBanned(CSteamID cidClient)
	{
		for (CSteamID& id : m_BannedClients)
		{
			if (cidClient == id)
				return true;
		}
		return false;
	}

	bool __forceinline IsSteamIDConnected(CSteamID cidClient)
	{
		if (m_Clients.find(cidClient) != m_Clients.end())
			return true;

		return false;
	}

	void __forceinline BanSteamID(CSteamID cidClient)
	{
		if (!IsSteamIDBanned(cidClient))
			m_BannedClients.push_back(cidClient);
	}

	void DisconnectClient(CSteamID cidClient)
	{
		CrossCheatClient* pClient = FindClientForID(cidClient);
		DisconnectClient(pClient);
	}

	void DisconnectClient(CrossCheatClient* pClient)
	{
		Globals::g_pSteamNetworking->CloseP2PSessionWithUser(pClient->GetClientSteamID());
		Globals::g_pSteamNetworkingMessages->CloseSessionWithUser(pClient->GetNetworkingIdentity());
		pClient->Disconnect();
		m_Clients.erase(pClient->GetClientSteamID());
		delete pClient;
	}

	CrossCheatClient* FindClientForID(CSteamID cidClient)
	{
		if (m_Clients.find(cidClient) != m_Clients.end())
		{
			return m_Clients[cidClient];
		}

		return nullptr;
	}
	CrossCheatClient* CreateClientForID(CSteamID id)
	{
		CrossCheatClient* pClient = FindClientForID(id);
		if (!pClient)
		{
			pClient = new CrossCheatClient(id);
			m_Clients[id] = pClient;
		}

		return pClient;
	}
	CrossCheatClient* CreateClientForNetworkingIdentity(SteamNetworkingIdentity idRemote)
	{
		CrossCheatClient* pClient = FindClientForID(idRemote.GetSteamID());
		if (!pClient)
		{
			pClient = new CrossCheatClient(idRemote);
			m_Clients[idRemote.GetSteamID()] = pClient;
		}

		return pClient;
	}

	CrossCheatClient* ConnectClient(SteamNetworkingIdentity idRemote)
	{
		CrossCheatClient* pClient = CreateClientForNetworkingIdentity(idRemote);
		Globals::g_pSteamNetworkingMessages->AcceptSessionWithUser(idRemote);
		Globals::g_pSteamFriends->RequestUserInformation(pClient->GetClientSteamID(), true);
		return pClient;
	}

	void SendConnectionRequestToClient(CSteamID csID)
	{
		// TODO : Make this check to see if we have a pending request
		SteamNetworkingIdentity idRemote;
		idRemote.SetSteamID64(csID.ConvertToUint64());
		char* pBuffer[sizeof(MsgHeader_t)];
		MsgHeader_t* pHeader = reinterpret_cast<MsgHeader_t*>(pBuffer);
		pHeader->nSize = 0;
		pHeader->nType = CrossCheatMsgType::_ConnectionAccepted;
		pHeader->SteamID = csID.GetAccountID();
		pHeader->bVoice = false;
		pHeader->bIsConnectionInit = true;
		strcpy(pHeader->CHEAT, "CHEAT");
		Globals::g_pSteamNetworkingMessages->SendMessageToUser(idRemote, pBuffer, sizeof(MsgHeader_t), k_nSteamNetworkingSend_ReliableNoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession, 58);
	}


	void SendMessageToUser(CrossCheatMsgType nType, ::google::protobuf::Message* pMsg, uint32_t SteamID, int nVirtualPort = 58) {
		size_t nMessageSize = pMsg->ByteSize();
		size_t nTotalSize = nMessageSize + sizeof(MsgHeader_t);
		char* pBuffer = (char*)malloc(nTotalSize);
		pMsg->SerializePartialToArray((void*)(pBuffer + sizeof(MsgHeader_t)), nMessageSize);
		MsgHeader_t* pHeader = reinterpret_cast<MsgHeader_t*>(pBuffer);
		pHeader->nSize = nMessageSize;
		pHeader->nType = nType;
		pHeader->SteamID = Globals::g_pSteamUser->GetSteamID().GetAccountID();
		pHeader->bVoice = false;
		pHeader->bIsConnectionInit = false;
		strcpy(pHeader->CHEAT, "CHEAT");
		SteamNetworkingIdentity Iden;
		CSteamID SteamIDFull(SteamID, k_EUniversePublic, k_EAccountTypeIndividual);
		Iden.SetSteamID64(SteamIDFull.ConvertToUint64());
		Globals::g_pSteamNetworkingMessages->SendMessageToUser(Iden, pBuffer, nTotalSize, k_nSteamNetworkingSend_ReliableNoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession, nVirtualPort);
		free(pBuffer);	
	}

	void BroadCastMessage(CrossCheatMsgType nType, ::google::protobuf::Message* pMsg, int nVirtualPort = 58)
	{
		for (std::pair<const CSteamID, CrossCheatClient*>& Client : m_Clients)
		{
			SendMessageToUser(nType, pMsg, Client.second);
		}
	}


	void BroadCastMessageToTeammates(CrossCheatMsgType nType, ::google::protobuf::Message* pMsg, int nVirtualPort = 58)
	{


		if (!g_pLocalPlayer.Get())
			return;

		int nOurTeam = g_pLocalPlayer->m_iTeamNum();

		for (int i = 1; i < g_pInterfaces->m_pEngine->GetMaxClients(); i++)
		{
			Entity* pEnt = g_pInterfaces->m_pEntityList->GetClientEntity(i);

			if (!pEnt || !pEnt->IsPlayer() || pEnt->m_iTeamNum() != nOurTeam)
				continue;

			player_info_t player_info;
			if (!g_pInterfaces->m_pEngine->GetPlayerInfo(i, &player_info) || player_info.fakeplayer)
				continue;


			CrossCheatClient* pClient = FindClientForID(CSteamID(player_info.friendsId, k_EUniversePublic, k_EAccountTypeIndividual));

			if (!pClient)
				continue;

			SendMessageToUser(nType, pMsg, pClient, nVirtualPort);
		}
	}


	void SendMessageToUser(CrossCheatMsgType nType, ::google::protobuf::Message* pMsg, CrossCheatClient* pClient, int nVirtualPort = 58) {
		SendMessageToUser(nType, pMsg, pClient, nVirtualPort);
	}


	bool IsMessagePriviledged(CrossCheatMsgType nType)
	{
		for (CrossCheatMsgType& Type : m_PriviledgedMessages)
		{
			if (nType == Type)
				return true;
		}

		return false;
	}

	void MakeMessagePriviledged(CrossCheatMsgType nType)
	{
		for (CrossCheatMsgType& Type : m_PriviledgedMessages)
		{
			if (nType == Type)
				return;
		}

		m_PriviledgedMessages.push_back(nType);
	}
private:
	std::map<CSteamID, CrossCheatClient*> m_Clients;
	std::vector<CSteamID> m_BannedClients;
	std::vector<CSteamID> m_WhiteListedClients;
	std::vector<CrossCheatMsgType> m_PriviledgedMessages;
	std::vector<int> m_vOpenPorts;

	bool m_bIsUsingWhiteList = false;
};

inline CrossCheatTalkNetwork* g_pCCNetwork;


unsigned int WINAPI BlockThread(void*); // Blocks IP Grabbing
void CrossCheat_Initialize(); // Initalizes 


void SessionFailedHandler(SteamNetworkingMessagesSessionFailed_t* pSessionFailed);
void SessionRequestHandler(SteamNetworkingMessagesSessionRequest_t* pRequest);