#include "CrossCheatTalk.h"
#include "SDK.h"


#ifdef _DEBUG
#define DEBUGCON(OUT_MSG, ...) VCON_RAW(XorStr(OUT_MSG), __VA_ARGS__);
#else
#define DEBUGCON(OUT_MSG, ...) /**/
#endif


bool IsPlayerInGame(CSteamID csID)
{
	for (int i = 0; i < g_pInterfaces->m_pEngine->GetMaxClients(); i++)
	{
		Entity* pEnt = g_pInterfaces->m_pEntityList->GetClientEntity(i);

		if (!pEnt || !pEnt->IsPlayer())
			continue;

		player_info_t player_info;
		if (!g_pInterfaces->m_pEngine->GetPlayerInfo(i, &player_info))
			continue;

		if (player_info.friendsId == csID.GetAccountID())
			return true;
	}

	return false;
}


void CrossCheatTalkNetwork::OnNewFrame()
{
	SteamNetworkingMessage_t* pMsg{ nullptr };
	SteamNetworkingMessage_t* pMsgArray{ nullptr };

	static double dbLastSearchTime{ 0.0 };
	if ((dbLastSearchTime  < (Plat_FloatTime() - TIME_PER_GLOBAL_SEARCH)))
	{
		dbLastSearchTime = Plat_FloatTime();
		//Search();
	}

	int fuck = m_Clients.size();
	int j = fuck;

	for (int nOpenPort : m_vOpenPorts) {
		int nMessagesRecieved = Globals::g_pSteamNetworkingMessages->ReceiveMessagesOnChannel(nOpenPort, &pMsgArray, MAX_PACKETS_TO_PROCESS);
		for (int i = 0; i < nMessagesRecieved; i++) {
			pMsg = &(pMsgArray[i]);
			if (!pMsg)
				continue;

			CrossCheatClient* pClient = FindClientForID(pMsg->m_identityPeer.GetSteamID());

			size_t nDataSize = pMsg->GetSize();
			if ((nDataSize < sizeof(MsgHeader_t)) || nDataSize > MAX_MESSAGE_SIZE + sizeof(MsgHeader_t)) { //Not Valid

				pMsg->Release();
				continue;
			}

			MsgHeader_t* pHeader = reinterpret_cast<MsgHeader_t*>((void*)pMsg->GetData());


			if (!pClient && pHeader->bIsConnectionInit) 
			{
				// No Client Connection, They Are Responding to our request
				// Since we don't create a new client until the ack the connection
				// (this packet)
				ConnectClient(pMsg->m_identityPeer);
				pMsg->Release();
				CSteamID csID = pMsg->m_identityPeer.GetSteamID();
				DEBUGCON(" [CrossCheatTalkNetwork::OnNewFrame] ConnectionInit From %d (Player : %s)\n", csID, Globals::g_pSteamFriends->GetFriendPersonaName(csID));

				continue;
			}

			if (!pClient) // Are We Even Still Connected To This Client?
			{
				pMsg->Release();
				continue;
			}

			if (pClient->AreRateLimiting())
			{
				if (pClient->ShouldDisconnectSpammer()) // Get da fook outta Here!
				{
					DisconnectClient(pClient);
					BanSteamID(pMsg->m_identityPeer.GetSteamID());
				}
				pMsg->Release();
				continue;
			}


			pClient->OnNewPacket();

			if (pHeader->bIsConnectionInit)
			{
				// So we have them, and they are sending a connect init.
				// This means we added the client in SessionRequestHandler
				// And now this is the connect packet. Lets just send one back,
				// which will basically ack the connection.
				if (!pClient->OurRequest())
				{
					// This is to stop a feedback loop where we both keep sending back bIsConnectionInit causing
					// both of us to be banned as spammers.
					// If both clients send a message and that causes the session to be accepted,
					// then this wont be called. And both clients can't both NOT send a connection
					// Except in some crazy edge case where we clear a client after sending a connect and before
					// recieving theirs connect ack, but even then the feedback loop would be
					// unlikely to occur anyways. And even all that considered, it's still ratelimited.
					SendConnectionRequestToClient(pClient->GetClientSteamID());
				}
				pMsg->Release();
				continue;
			}


			// 0-Sized packets are valid! but we don't want to malloc or memcpy that or attempt to process it!
			// So we knick them out here. Ha-ha take that valve, cheat networking code is better than yours!
			if (pHeader->nSize > (nDataSize - sizeof(MsgHeader_t)) || (pHeader->nSize < 1) || (pHeader->nSize > MAX_MESSAGE_SIZE)) { // Malicious Client? Or Fucked up Recieving code
				pMsg->Release();
				DEBUGCON(" [CrossCheatTalkNetwork::OnNewFrame] Message Recieved With Invalid Size In Header!");
				continue;
			}

			// Make sure something hasn't gone wrong somewhere
			if (strcmp("CHEAT", pHeader->CHEAT)) {
				DEBUGCON(" [CrossCheatTalkNetwork::OnNewFrame] Message Recieved With Invalid CHEAT In Header!");
				pMsg->Release();
				continue;
			}

			char* pBuffer = (char*)malloc(nDataSize);
			std::memcpy(pBuffer, pMsg->GetData(), nDataSize);
			pHeader = reinterpret_cast<MsgHeader_t*>(pBuffer);

			if (!pBuffer) { // uh oh!
				pMsg->Release();
				continue;
			}

			// Got all the way down here, Dispatch the protobuf to the client connection. 
			pClient->ProcessMessage(pHeader->nSize, pHeader->nType, (const char*)(pBuffer + sizeof(MsgHeader_t)));
			free(pBuffer);
			pMsg->Release();
		}
	}
}



void CrossCheatTalkNetwork::Search()
{
	for (int i = 0; i < g_pInterfaces->m_pEngine->GetMaxClients(); i++)
	{
		Entity* pEnt = g_pInterfaces->m_pEntityList->GetClientEntity(i);

		if (!pEnt || !pEnt->IsPlayer())
			continue;

		player_info_t player_info;
		if (!g_pInterfaces->m_pEngine->GetPlayerInfo(i, &player_info))
			continue;

		CSteamID csID(player_info.friendsId, k_EUniversePublic, k_EAccountTypeIndividual);
		if (!IsSteamIDBanned(csID) && !IsSteamIDConnected(csID) && IsSteamIDWhiteListed(csID))
		{
			SendConnectionRequestToClient(csID);
		}

	}

	return;
}


void CrossCheat_Initialize()
{
	// Create Our Network
	g_pCCNetwork = new CrossCheatTalkNetwork;


	// Now Lets Setup And Stop Our IP From Being Pulled Cause People Do That
	_beginthreadex(0, 0, BlockThread, 0, NULL, 0);

	// Open Our Port
	g_pCCNetwork->OpenPort(58);


	// Set Callbacks
	Globals::g_pSteamNetworkingUtils->SetGlobalCallback_MessagesSessionFailed(&SessionFailedHandler);
	Globals::g_pSteamNetworkingUtils->SetGlobalCallback_MessagesSessionRequest(&SessionRequestHandler);
}

void SessionFailedHandler(SteamNetworkingMessagesSessionFailed_t* pSessionFailed) 
{
	DEBUGCON(" [SessionFailedHandler] Session Request Failure Reason: %d SteamID %d \n" , pSessionFailed->m_info.m_eEndReason, pSessionFailed->m_info.m_identityRemote.GetSteamID().GetAccountID());
}


void SessionRequestHandler(SteamNetworkingMessagesSessionRequest_t* pRequest)
{
	CSteamID csID = pRequest->m_identityRemote.GetSteamID();

	if (g_pCCNetwork->IsSteamIDBanned(csID) || !g_pCCNetwork->IsSteamIDWhiteListed(csID))
	{
		Globals::g_pSteamNetworking->CloseP2PSessionWithUser(csID);
		return;
	}

	if (!IsPlayerInGame(csID))
	{
		Globals::g_pSteamNetworking->CloseP2PSessionWithUser(csID);
		return;
	}

	if (g_pCCNetwork->IsSteamIDConnected(csID))
	{
		CrossCheatClient* m_pClient = g_pCCNetwork->FindClientForID(csID);
		m_pClient->OnNewPacket(); // Yes We Ratelimit These

		if (m_pClient->ShouldDisconnectSpammer())
		{
			g_pCCNetwork->DisconnectClient(m_pClient);
			g_pCCNetwork->BanSteamID(csID);
		}
		return;
	}
	// Okay they aren't banned, and aren't already connected. Lets connect them up

	DEBUGCON(" [SessionRequestHandler] Accepting Communications with %d (Player : %s)\n", csID, Globals::g_pSteamFriends->GetFriendPersonaName(csID));
	g_pCCNetwork->ConnectClient(pRequest->m_identityRemote)->SetTheirRequest();

}


unsigned int WINAPI BlockThread(void*) // Now sometimes we need to protect our IP from LUA users!
{
	while (true)
	{

		IMatchSession* pMatchSession = g_pInterfaces->m_pMatchFramework->GetMatchSession();
		if (pMatchSession && g_bBlockP2PSessionRequests)
		{

			const uint64_t nLobby = CallVirtualFunction<uint64_t>(pMatchSession, 4);
			int nNumMembers = Globals::g_pSteamMatchmaking->GetNumLobbyMembers(nLobby);
			for (int i = 0; i < nNumMembers; i++)
			{
				CSteamID ClientID = Globals::g_pSteamMatchmaking->GetLobbyMemberByIndex(nLobby, i);
				if (!g_pCCNetwork->IsSteamIDWhiteListed(ClientID))
				{
					Globals::g_pSteamNetworking->CloseP2PSessionWithUser(ClientID);
				}
			}
			Sleep(.001);
		}
	}
} // ENRON - 7/8/2021


bool ChatMessage_Handler(CrossCheatClient* pClient, size_t nDataSize, const char* pMsg)
{
	ChatMessage ChatMessage;

	if (!ChatMessage.ParseFromArray(pMsg, nDataSize))
		return false;

	VCHAT("[%s] %s", Globals::g_pSteamFriends->GetFriendPersonaName(pClient->GetClientSteamID()), ChatMessage.message());
}