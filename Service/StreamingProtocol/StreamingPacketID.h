#pragma once

#include <stdint.h>

#include "../../../../Module/IOCPNetworkEngine/Protocol/PacketID.h"

enum class DESKTOP_STREAMING_PACKET_ID : uint16_t
{
	CS_DESKTOP_STREAMING_SUBSCRIBE = ToPacketID(PACKET_ID::SERVICE_BEGIN),
	CS_DESKTOP_STREAMING_UNSUBSCRIBE,
	SC_DESKTOP_STREAMING_INFO,
	SC_DESKTOP_STREAMING_FRAME_CHUNK,
};

constexpr PACKET_ID_TYPE ToPacketID(DESKTOP_STREAMING_PACKET_ID packetID)
{
	return static_cast<PACKET_ID_TYPE>(packetID);
};
