#pragma once

#include <stdint.h>

#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketID.h"

enum class ECHO_PACKET_ID : uint16_t
{
	CS_ECHO_REQUEST = ToPacketID(PACKET_ID::SERVICE_BEGIN),
	SC_ECHO_RESPONSE,
};

constexpr PACKET_ID_TYPE ToPacketID(ECHO_PACKET_ID packetID)
{
	return static_cast<PACKET_ID_TYPE>(packetID);
};
