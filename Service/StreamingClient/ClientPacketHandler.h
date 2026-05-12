#pragma once

#include <stdint.h>

#include "../../../../Module/IOCPNetworkEngine/Job/JobDefs.h"

class ISession;
class PacketHandlerTable;

namespace PacketHandler
{
	namespace Client
	{
		bool RegisterHandlers(PacketHandlerTable* handlerTable);

		bool HandleStreamInfo(ISession* session, const char* packetData, uint32_t packetSize, const HandlerContext& context);
		bool HandleFrameChunk(ISession* session, const char* packetData, uint32_t packetSize, const HandlerContext& context);
	}
}
