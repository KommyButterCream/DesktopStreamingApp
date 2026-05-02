#pragma once

#include <stdint.h>

#include "../../../../Module/IOCPNetworkEngine/Job/JobDefs.h" // for HandlerContext

class ISession;
class PacketHandlerTable;

namespace PacketHandler
{
	namespace Server
	{
		// Register
		bool RegisterHandlers(PacketHandlerTable* handlerTable);

		// Packet Handlers
		bool HandleEchoRequest(ISession* session, const char* packetData, uint32_t packetSize, const HandlerContext& context);
	}
}