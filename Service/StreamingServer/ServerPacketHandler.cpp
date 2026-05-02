#include "ServerPacketHandler.h"

#include "../../../../Module/IOCPNetworkEngine/HandlerTable/PacketHandlerTable.h" // for PacketHandlerTable
#include "../../../../Module/IOCPNetworkEngine/Session/ClientSession.h" // for ClientSession
#include "../../../../Module/IOCPNetworkEngine/Memory/SlabMemoryPoolHelper.h" // for MEMORY_POOL

#include "../StreamingProtocol/StreamingPacket.h" // for Request, Response Packet Structure
#include "../StreamingProtocol/StreamingPacketID.h" // for ECHO_PACKET_ID

namespace PacketHandler
{
	namespace Server
	{
		bool RegisterHandlers(PacketHandlerTable* handlerTable)
		{
			bool registerResult = true;

			registerResult &= handlerTable->Register(ToPacketID(ECHO_PACKET_ID::CS_ECHO_REQUEST), HandleEchoRequest);

			return registerResult;
		}

		bool HandleEchoRequest(ISession* session, const char* packetData, uint32_t packetSize, const HandlerContext& context)
		{
			ClientSession* clientSession = static_cast<ClientSession*>(session);

			if (!clientSession || !packetData)
				return false;

			const CS_ECHO_REQUEST_PACKET* requestPacket = reinterpret_cast<const CS_ECHO_REQUEST_PACKET*>(packetData);

			void* packetMemory = MEMORY_POOL::CreatePacket(*context.packetMemoryPool, sizeof(SC_ECHO_RESPONSE_PACKET));
			SC_ECHO_RESPONSE_PACKET* responsePacket = reinterpret_cast<SC_ECHO_RESPONSE_PACKET*>(packetMemory);
			if (!responsePacket)
				return false;

			responsePacket->header.packetId = ToPacketID(ECHO_PACKET_ID::SC_ECHO_RESPONSE);
			responsePacket->header.packetSize = sizeof(SC_ECHO_RESPONSE_PACKET);

			responsePacket->requestId = requestPacket->requestId;

			size_t messageLength = strnlen_s(requestPacket->message, sizeof(requestPacket->message) - 1);
			memcpy_s(responsePacket->message, sizeof(responsePacket->message), requestPacket->message, messageLength);
			responsePacket->message[messageLength] = '\0';

			if (!clientSession->EnqueueSendPacket(reinterpret_cast<void**>(&responsePacket), packetSize))
			{
				__debugbreak();
				return false;
			}

			return true;
		}
	}
}