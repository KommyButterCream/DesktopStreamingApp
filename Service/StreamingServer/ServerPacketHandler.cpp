#include "ServerPacketHandler.h"

#include "../../../../Module/IOCPNetworkEngine/HandlerTable/PacketHandlerTable.h" // for PacketHandlerTable
#include "../../../../Module/IOCPNetworkEngine/Session/ClientSession.h" // for ClientSession
#include "../../../../Module/IOCPNetworkEngine/Memory/SlabMemoryPoolHelper.h" // for MEMORY_POOL

#include "../StreamingProtocol/StreamingPacket.h" // for Request, Response Packet Structure
#include "../StreamingProtocol/StreamingPacketID.h" // for PACKET_ID

#include "../StreamingServer/StreamingServer.h"

namespace PacketHandler
{
	namespace Server
	{
		bool RegisterHandlers(PacketHandlerTable* handlerTable)
		{
			bool registerResult = true;

			registerResult &= handlerTable->Register(ToPacketID(DESKTOP_STREAMING_PACKET_ID::CS_DESKTOP_STREAMING_SUBSCRIBE), HandleSubscribe);
			registerResult &= handlerTable->Register(ToPacketID(DESKTOP_STREAMING_PACKET_ID::CS_DESKTOP_STREAMING_UNSUBSCRIBE), HandleUnSubscribe);

			return registerResult;
		}

		bool HandleSubscribe(ISession* session, const char* packetData, uint32_t packetSize, const HandlerContext& context)
		{
			if (!session || !packetData || packetSize != sizeof(CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET))
				return false;

			ClientSession* clientSession = static_cast<ClientSession*>(session);
			StreamingServer* server = static_cast<StreamingServer*>(context.serviceContext);
			if (!clientSession || !server)
				return false;

			const CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET* requestPacket = reinterpret_cast<const CS_DESKTOP_STREAMING_SUBSCRIBE_PACKET*>(packetData);
			return server->HandleSubscribe(clientSession, requestPacket->streamId, context);
		}

		bool HandleUnSubscribe(ISession* session, const char* packetData, uint32_t packetSize, const HandlerContext& context)
		{
			if (!session || !packetData || packetSize != sizeof(CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET))
				return false;

			ClientSession* clientSession = static_cast<ClientSession*>(session);
			StreamingServer* server = static_cast<StreamingServer*>(context.serviceContext);
			if (!clientSession || !server)
				return false;

			const CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET* requestPacket = reinterpret_cast<const CS_DESKTOP_STREAMING_UNSUBSCRIBE_PACKET*>(packetData);
			return server->HandleUnsubscribe(clientSession, requestPacket->streamId);
		}
	}
}