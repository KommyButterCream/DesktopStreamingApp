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
		namespace
		{
			// 재조립을 처음부터 다시 시작한다.
			//
			// 전에는 실패할 때마다 세 줄(reassembling / receivedFrameSize /
			// nextChunkIndex)을 손으로 되돌렸고, 한 곳에서는 그중 하나를
			// 빠뜨려도 티가 나지 않았다.
			inline void ResetReassembly(DesktopStreamClientSessionContext& streamContext)
			{
				streamContext.SetReassembling(false);
				streamContext.receivedFrameSize = 0;
				streamContext.nextChunkIndex = 0;
				streamContext.expectedFrameSize = 0;
				streamContext.expectedChunkCount = 0;
			}
		}

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
			if (!session || !packetData || packetSize != sizeof(SC_DESKTOP_STREAMING_INFO_PACKET))
				return false;

			StreamingClient* streamingClient = static_cast<StreamingClient*>(context.serviceContext);
			if (!streamingClient)
				return false;

			const SC_DESKTOP_STREAMING_INFO_PACKET* infoPacket = reinterpret_cast<const SC_DESKTOP_STREAMING_INFO_PACKET*>(packetData);

			LOGI("[Session : %u] stream info received (streamId=%u, %ux%u, codec=%u, infoVersion=%u, configVersion=%u)",
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
			constexpr uint32_t chunkHeaderSize = DESKTOP_STREAM_CHUNK_HEADER_SIZE;
			if (!session || !packetData || packetSize < chunkHeaderSize)
				return false;

			ClientSession* clientSession = static_cast<ClientSession*>(session);
			StreamingClient* streamingClient = static_cast<StreamingClient*>(context.serviceContext);
			if (!clientSession || !streamingClient)
				return false;

			DesktopStreamClientSessionContext* streamContext =
				static_cast<DesktopStreamClientSessionContext*>(clientSession->GetSessionContext());
			if (!streamContext)
				return false;

			const SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET* chunkPacket = reinterpret_cast<const SC_DESKTOP_STREAMING_FRAME_CHUNK_PACKET*>(packetData);

			// --- 검증 ---
			//
			// 여기서 통과시킨 값이 그대로 memcpy 의 오프셋과 길이가 된다.
			// 하나라도 빠지면 512KB 짜리 재조립 버퍼 밖으로 쓴다.

			const uint32_t expectedPacketSize = MakeDesktopStreamingVariablePacketSize(chunkHeaderSize, chunkPacket->chunkDataSize);
			if (expectedPacketSize == 0 || packetSize != expectedPacketSize)
			{
				streamingClient->NoteChunkRejected();
				return false;
			}

			// 우리가 아는 서버가 보낼 수 있는 것보다 큰 청크다.
			// (예전에는 이 검사가 없었다 — packetSize 일치만 봤으므로
			//  엔진의 수신 상한까지는 통과했다)
			if (chunkPacket->chunkDataSize == 0 ||
				chunkPacket->chunkDataSize > DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE)
			{
				streamingClient->NoteChunkRejected();
				return false;
			}

			if (streamContext->hasStreamInfo && chunkPacket->streamId != streamContext->streamId)
			{
				streamingClient->NoteChunkRejected();
				return false;
			}

			if (chunkPacket->totalFrameSize == 0 || chunkPacket->totalFrameSize > DESKTOP_STREAM_MAX_FRAME_SIZE)
			{
				streamingClient->NoteChunkRejected();
				return false;
			}

			if (chunkPacket->chunkCount == 0 ||
				chunkPacket->chunkCount > DESKTOP_STREAM_MAX_CHUNK_COUNT ||
				chunkPacket->chunkIndex >= chunkPacket->chunkCount)
			{
				streamingClient->NoteChunkRejected();
				return false;
			}

			// 오프셋 + 길이가 프레임 안에 들어가는가. 덧셈이 32비트에서
			// 넘치지 않는 것은 위의 두 상한이 보장한다.
			if ((chunkPacket->chunkOffset + chunkPacket->chunkDataSize) > chunkPacket->totalFrameSize)
			{
				streamingClient->NoteChunkRejected();
				return false;
			}

			// 오프셋이 청크 순번과 맞는가.
			//
			// 이 검사가 없었다. 위의 경계 검사만으로도 버퍼 밖으로 나가지는
			// 않지만, 같은 자리에 두 번 쓰면서 receivedFrameSize 는 계속
			// 늘어나는 조합이 가능했다. 그러면 구멍이 난 프레임이
			// "완성" 으로 판정되어 깨진 NAL 이 디코더로 간다.
			if (chunkPacket->chunkOffset != static_cast<uint32_t>(chunkPacket->chunkIndex) * DESKTOP_STREAM_MAX_CHUNK_DATA_SIZE)
			{
				streamingClient->NoteChunkRejected();
				return false;
			}

			// --- 재조립 ---

			const bool startsNewFrame =
				!streamContext->IsReassembling() || streamContext->currentFrameId != chunkPacket->frameId;

			if (startsNewFrame)
			{
				// 앞 프레임을 끝내지 못한 채 새 프레임이 시작됐다.
				// 서버가 중간에 포기했다는 뜻이므로 버린 프레임으로 센다.
				if (streamContext->IsReassembling())
				{
					streamingClient->NoteFrameDiscarded();
				}

				// 새 프레임은 0번 청크부터 시작해야 한다. 중간부터 들어온
				// 것은 앞을 이미 놓친 것이므로 받아도 완성되지 않는다.
				if (chunkPacket->chunkIndex != 0)
				{
					ResetReassembly(*streamContext);
					streamingClient->NoteChunkRejected();
					return true;
				}

				streamContext->SetReassembling(true);
				streamContext->currentFrameId = chunkPacket->frameId;
				streamContext->currentTimestamp = chunkPacket->timestamp;
				streamContext->expectedFrameSize = chunkPacket->totalFrameSize;
				streamContext->receivedFrameSize = 0;
				streamContext->expectedChunkCount = chunkPacket->chunkCount;
				streamContext->nextChunkIndex = 0;
				streamContext->frameType = chunkPacket->frameType;
			}

			// 같은 프레임 안에서 앞선 청크와 앞뒤가 맞는가.
			if (streamContext->expectedFrameSize != chunkPacket->totalFrameSize ||
				streamContext->expectedChunkCount != chunkPacket->chunkCount ||
				streamContext->nextChunkIndex != chunkPacket->chunkIndex)
			{
				ResetReassembly(*streamContext);
				streamingClient->NoteFrameDiscarded();
				return true;
			}

			memcpy(streamContext->frameBuffer + chunkPacket->chunkOffset, chunkPacket->chunkData, chunkPacket->chunkDataSize);
			streamContext->receivedFrameSize += chunkPacket->chunkDataSize;
			++streamContext->nextChunkIndex;

			streamingClient->NoteChunkAccepted(chunkPacket->chunkDataSize);

			if (streamContext->receivedFrameSize == streamContext->expectedFrameSize &&
				streamContext->nextChunkIndex == streamContext->expectedChunkCount)
			{
				streamContext->SetReassembling(false);

				// LOG_HIGH 였다. 로거는 level < m_logLevel 일 때만 거르는데
				// LOG_HIGH(5) 는 기본 레벨 LOG_INFO(1) 보다 높으므로 이 줄은
				// 항상 나갔다 — 30fps 면 초당 30줄이고, 콘솔 쓰기는 프레임
				// 처리 자체보다 비싸다. 프레임 단위 추적은 디버그 레벨이다.
				LOGD("[Session : %u] frame reassembled (frameId=%llu, frameType=%u, bytes=%u, chunks=%u)",
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
