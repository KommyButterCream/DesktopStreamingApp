#include "ClientPacketHandler.h"

#include "../../../../Module/IOCPNetworkEngine/HandlerTable/PacketHandlerTable.h" // for PacketHandlerTable
#include "../../../../Module/IOCPNetworkEngine/Session/ClientSession.h" // for ClientSession
#include "../../../../Module/Core/Util/Logger.h"

#include "../StreamingProtocol/StreamingPacket.h" // for Request, Response Packet Structure
#include "../StreamingProtocol/StreamingPacketID.h" // for ECHO_PACKET_ID

using namespace Core::Util;

namespace PacketHandler
{
	namespace Client
	{
		bool RegisterHandlers(PacketHandlerTable* handlerTable)
		{
			bool registerResult = true;

			registerResult &= handlerTable->Register(ToPacketID(ECHO_PACKET_ID::CS_ECHO_REQUEST), HandleEchoRequest);
			registerResult &= handlerTable->Register(ToPacketID(ECHO_PACKET_ID::SC_ECHO_RESPONSE), HandleEchoResponse);

			return registerResult;
		}

		bool HandleEchoRequest(ISession* session, const char* packetData, uint32_t packetSize, const HandlerContext& context)
		{
			if (!session || !packetData || packetSize < sizeof(CS_ECHO_REQUEST_PACKET))
				return false;

			ClientSession* clientSession = static_cast<ClientSession*>(session);

			if (!clientSession || !packetData)
				return false;

			void* sendData = const_cast<char*>(packetData);
			if (!clientSession->EnqueueSendPacket(&sendData, packetSize))
			{
				__debugbreak();
				return false;
			}

			// 현재 Send 중인 패킷 데이터에 대한 소유권 포기.
			// Send IO Complete 시점에 패킷 메모리 해제함.
			clientSession->ClearCurrentJobData();

			return true;
		}

		bool HandleEchoResponse(ISession* session, const char* packetData, uint32_t packetSize, const HandlerContext& context)
		{
			if (!session || !packetData || packetSize < sizeof(SC_ECHO_RESPONSE_PACKET))
				return false;

			ClientSession* clientSession = static_cast<ClientSession*>(session);

			if (!clientSession)
				return false;


			const SC_ECHO_RESPONSE_PACKET* responsePacket = reinterpret_cast<const SC_ECHO_RESPONSE_PACKET*>(packetData);

			Logger::Log(LogLevel::LOG_HIGH, "[%d][%s] Response[%s] : %s", (int32_t)session->GetClientSocket(), __FUNCTION__, "CHAT_ECHO_PACKET_RESPONSE", responsePacket->message);

			return true;
		}
	}
}