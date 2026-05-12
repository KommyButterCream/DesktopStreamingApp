#include "ClientPacketHandler.h"

#include <cstring>

#include "../../../../Module/IOCPNetworkEngine/HandlerTable/PacketHandlerTable.h"
#include "../../../../Module/IOCPNetworkEngine/Session/ClientSession.h"
#include "../../../../Module/Core/Util/Logger.h"

#include "../StreamingProtocol/StreamingPacket.h"
#include "../StreamingProtocol/StreamingPacketID.h"
#include "StreamingClient.h"

using namespace Core::Util;

namespace PacketHandler
{
	namespace Client
	{
		bool RegisterHandlers(PacketHandlerTable* handlerTable)
		{
			if (!handlerTable)
				return false;

			bool registerResult = true;

			registerResult &= handlerTable->Register(ToPacketID(DESKTOP_STREAMING_PACKET_ID::SC_DESKTOP_STREAMING_INFO), HandleStreamInfo);
			registerResult &= handlerTable->Register(ToPacketID(DESKTOP_STREAMING_PACKET_ID::SC_DESKTOP_STREAMING_FRAME_CHUNK), HandleFrameChunk);

			return registerResult;
		}

		bool HandleStreamInfo(ISession* session, const char* packetData, uint32_t packetSize, const HandlerContext& context)
		{
			if (!session || !packetData || packetSize != sizeof(CS_DESKTOP_STREAMING_INFO_PACKET))
				return false;

			StreamingClient* streamingClient = static_cast<StreamingClient*>(context.serviceContext);
			if (!streamingClient)
				return false;

			const CS_DESKTOP_STREAMING_INFO_PACKET* infoPacket = reinterpret_cast<const CS_DESKTOP_STREAMING_INFO_PACKET*>(packetData);

			Logger::Log(LogLevel::LOG_INFO, "[%s][Session : %u] stream info received (streamId=%u, %ux%u, codec=%u, infoVersion=%u, configVersion=%u)",
				__FUNCTION__,
				session->GetSessionID(),
				infoPacket->streamId,
				infoPacket->width,
				infoPacket->height,
				infoPacket->codecType,
				infoPacket->streamInfoVersion,
				infoPacket->codecConfigVersion);

			return streamingClient->HandleStreamInfo(infoPacket);
		}

		bool HandleFrameChunk(ISession* session, const char* packetData, uint32_t packetSize, const HandlerContext& context)
		{
			constexpr uint32_t chunkHeaderSize = GetDesktopStreamingFrameChunkHeaderSize();
			if (!session || !packetData || packetSize < chunkHeaderSize)
				return false;

			ClientSession* clientSession = static_cast<ClientSession*>(session);
			DesktopStreamClientSessionContext* streamContext = clientSession ? dynamic_cast<DesktopStreamClientSessionContext*>(clientSession->GetSessionContext()) : nullptr;
			StreamingClient* streamingClient = static_cast<StreamingClient*>(context.serviceContext);
			if (!clientSession || !streamContext || !streamingClient)
				return false;

			const SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET* chunkPacket = reinterpret_cast<const SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET*>(packetData);
			const uint32_t expectedPacketSize = MakeDesktopStreamingVariablePacketSize(chunkHeaderSize, chunkPacket->chunkDataSize);
			if (packetSize != expectedPacketSize)
				return false;

			if (chunkPacket->streamId != streamContext->streamId && streamContext->hasStreamInfo)
				return false;

			if (chunkPacket->totalFrameSize == 0 || chunkPacket->totalFrameSize > DESKTOP_STREAM_MAX_FRAME_SIZE)
				return false;

			if (chunkPacket->chunkCount == 0 || chunkPacket->chunkIndex >= chunkPacket->chunkCount)
				return false;

			if ((chunkPacket->chunkOffset + chunkPacket->chunkDataSize) > chunkPacket->totalFrameSize)
				return false;

			if (!streamContext->reassembling || streamContext->currentFrameId != chunkPacket->frameId)
			{
				streamContext->reassembling = true;
				streamContext->currentFrameId = chunkPacket->frameId;
				streamContext->currentTimestamp = chunkPacket->timestamp;
				streamContext->expectedFrameSize = chunkPacket->totalFrameSize;
				streamContext->receivedFrameSize = 0;
				streamContext->expectedChunkCount = chunkPacket->chunkCount;
				streamContext->nextChunkIndex = 0;
				streamContext->frameType = chunkPacket->frameType;
			}

			if (streamContext->expectedFrameSize != chunkPacket->totalFrameSize ||
				streamContext->expectedChunkCount != chunkPacket->chunkCount ||
				streamContext->nextChunkIndex != chunkPacket->chunkIndex)
			{
				streamContext->reassembling = false;
				streamContext->receivedFrameSize = 0;
				streamContext->nextChunkIndex = 0;
				return false;
			}

			memcpy(streamContext->frameBuffer + chunkPacket->chunkOffset, chunkPacket->chunkData, chunkPacket->chunkDataSize);
			streamContext->receivedFrameSize += chunkPacket->chunkDataSize;
			++streamContext->nextChunkIndex;

			if (streamContext->receivedFrameSize == streamContext->expectedFrameSize &&
				streamContext->nextChunkIndex == streamContext->expectedChunkCount)
			{
				streamContext->reassembling = false;

				Logger::Log(LogLevel::LOG_HIGH, "[%s][Session : %u] frame reassembly success (frameId=%llu, frameType=%u, bytes=%u, chunks=%u)",
					__FUNCTION__,
					session->GetSessionID(),
					static_cast<unsigned long long>(streamContext->currentFrameId),
					streamContext->frameType,
					streamContext->receivedFrameSize,
					streamContext->expectedChunkCount);

				return streamingClient->HandleFrameComplete(*streamContext);
			}

			return true;
		}
	}
}
