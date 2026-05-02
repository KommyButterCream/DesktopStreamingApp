#pragma once

#include "StreamingPacketID.h"
#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketHeader.h"

#pragma pack(push)

struct CS_ECHO_REQUEST_PACKET
{
	PACKET_HEADER header;
	uint32_t requestId;
	char message[256];

	CS_ECHO_REQUEST_PACKET()
		: header(static_cast<uint16_t>(ECHO_PACKET_ID::CS_ECHO_REQUEST), sizeof(CS_ECHO_REQUEST_PACKET))
		, requestId(0)
		, message{}
	{
	}
};

struct SC_ECHO_RESPONSE_PACKET
{
	PACKET_HEADER header;
	uint32_t requestId;
	char message[256];

	SC_ECHO_RESPONSE_PACKET()
		: header(static_cast<uint16_t>(ECHO_PACKET_ID::SC_ECHO_RESPONSE), sizeof(SC_ECHO_RESPONSE_PACKET))
		, requestId(0)
		, message{}
	{
	}
};

#pragma pack(pop)