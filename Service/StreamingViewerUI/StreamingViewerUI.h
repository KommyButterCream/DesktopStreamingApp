#pragma once

#include <stdint.h>

#ifdef BUILD_STREAMING_VIEWER_UI_DLL
#define STREAMING_VIEWER_UI_API __declspec(dllexport)
#else
#define STREAMING_VIEWER_UI_API __declspec(dllimport)
#endif

class D3D11ImageView;
class ServiceControlLayer;
class ServiceStatsLayer;

// 컨트롤 바에서 눌린 것.
//
// 이 열거형이 뷰어가 아니라 이 DLL 에 있다는 점이 중요하다. 재생이 무엇인지
// 아는 것은 서비스뿐이고, 뷰어는 그리기와 히트 테스트만 한다. 컨트롤을
// 추가할 때 뷰어 저장소는 바뀌지 않는다.
enum class StreamingViewerCommand : uint32_t
{
	None = 0,
	Play,
	Pause,
	Stop,
};

enum class StreamingPlaybackState : uint32_t
{
	Stopped = 0,
	Playing,
	Paused,
};

// 지금 받고 있는 스트림의 화질.
//
// 고르는 값이 아니라 보여주는 값이다. 서버는 인코더 하나로 모든 구독자에게
// 같은 스트림을 보내므로 뷰어별 해상도라는 개념이 없고, 비트레이트는 서버가
// 혼잡 신호를 보고 스스로 올리고 내린다. 여기에 선택 UI 를 두면 누른 대로
// 되지 않는 버튼이 된다.
//
// 0 인 항목은 표시에서 빠진다. 스트림 정보를 아직 못 받았으면 전부 0 으로
// 두면 되고, 그동안은 "--" 가 나온다.
struct StreamingQualityInfo
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fps = 0;

	// 최근 구간의 실측 수신 비트레이트. 서버가 설정한 값이 아니라
	// 이쪽에 실제로 도착한 양이다.
	float bitrateMbps = 0.0f;
};

// 진단 오버레이에 띄울 값들.
//
// 전부 클라이언트가 아는 것뿐이다. 서버의 캡처 fps, 인코더 큐 깊이,
// 설정된 비트레이트 목표치는 여기 없다 — 그쪽으로 오는 패킷이 없다.
// 넣으려면 피드백의 반대 방향 패킷이 필요하고, 그건 별도 작업이다.
//
// 손실 카운터가 다섯으로 나뉘어 있는 것이 이 구조체의 요점이다.
// 화면이 끊기는 증상은 하나인데 원인은 다섯이고, 어느 값이 움직이는지가
// 곧 어느 단계가 막혔는지다.
//
//   chunksRejected    검증에서 걸러냄      프로토콜 / 재조립
//   framesDiscarded   청크가 어긋남        네트워크
//   decodeQueueDrops  유입 큐 넘침        디코더가 못 따라감
//   poolExhausted     출력 풀 고갈        표시 쪽이 못 따라감
//   notConsumed       아무도 안 가져감     표시 쪽이 못 따라감
//
// 값은 누적이다. 오버레이는 증분이 아니라 누적을 보여준다 — 진단할 때
// 알고 싶은 것은 "지금 떨어지나" 보다 "지금까지 몇 장 잃었나" 다.
struct StreamingStatsInfo
{
	bool connected = false;

	// 시계열로도 그리는 값. latencyMs 는 음수면 "모름" 이다.
	float bitrateMbps = 0.0f;
	float latencyMs = -1.0f;
	float presentedFps = 0.0f;

	uint64_t chunksRejected = 0;
	uint64_t framesDiscarded = 0;
	uint64_t decodeQueueDrops = 0;
	uint64_t poolExhausted = 0;
	uint64_t notConsumed = 0;

	uint32_t jitterBufferMs = 0;
	float avgPaceWaitMs = 0.0f;
	uint64_t resyncCount = 0;
};

// 뷰어 위에 스트리밍 컨트롤 바를 얹는다.
//
// 사용 순서
//   1) 뷰어를 Initialize 한다
//   2) Attach(viewer) — 여기서 뷰어에 렌더 레이어로 등록된다
//   3) 콜백을 걸고 상태를 넣는다
//   4) Detach() 또는 소멸 — 뷰어보다 먼저 정리한다
//
// 수명
//   Detach 는 뷰어의 렌더 락 안에서 레이어를 떼어낸다. 반환한 뒤에는
//   렌더 스레드가 이 객체를 부르지 않는다. 뷰어를 먼저 파괴하는 경우에도
//   뷰어가 Shutdown 을 불러 주므로 리소스는 정리되지만, 순서를 지키는 편이
//   읽기에 명확하다.
//
// 스레드
//   콜백은 창 메시지를 처리하는 스레드(= 뷰어를 만든 스레드)에서 불린다.
//   블로킹 작업을 하면 입력이 그만큼 밀린다.
class STREAMING_VIEWER_UI_API StreamingViewerUI
{
public:
	using CommandCallback = void (*)(StreamingViewerCommand command, void* userData);

	StreamingViewerUI();
	~StreamingViewerUI();

	StreamingViewerUI(const StreamingViewerUI&) = delete;
	StreamingViewerUI& operator=(const StreamingViewerUI&) = delete;
	StreamingViewerUI(StreamingViewerUI&&) = delete;
	StreamingViewerUI& operator=(StreamingViewerUI&&) = delete;

public:
	bool Attach(D3D11ImageView* viewer);
	void Detach();
	bool IsAttached() const;

	// 콜백은 Attach 전후 어느 쪽에서 걸어도 된다.
	void SetCommandCallback(CommandCallback callback, void* userData);

public:
	// 재생 상태에 따라 버튼 모양이 바뀐다. 상태를 아는 것은 호스트이므로
	// 컨트롤 바가 스스로 바꾸지 않는다 — 눌렸다고 재생이 시작된다는 보장이
	// 없기 때문이다(연결이 끊겨 있을 수 있다).
	void SetPlaybackState(StreamingPlaybackState state);
	StreamingPlaybackState GetPlaybackState() const;

	// --- 지연 표시 ---
	//
	// 라이브라 되감기가 없으므로 타임라인이 아니다. 대신 "지금 화면이
	// 실제보다 얼마나 뒤처져 있는가" 를 보여준다. 되감기 없는 스트림에서
	// 사용자가 알고 싶은 것은 그것뿐이다.
	//
	// 눈금 상한은 SetLatencyRange 로 정한다. 넘어가면 가득 찬 채로 멈춘다.
	//
	// 음수는 "모른다" 는 뜻이고 "--" 로 표시된다. 재생이 멈춰 있으면
	// 지터 버퍼 깊이는 남아 있어도 그 값이 화면 지연을 뜻하지 않는다.
	// 0 ms 로 두면 지연이 없는 것처럼 보이므로 구분한다.
	void SetLatency(float milliseconds);
	void SetLatencyRange(float maxMilliseconds);
	float GetLatency() const;

	// --- 화질 표시 ---
	//
	// 값이 바뀔 때만 부르면 된다. 같은 값을 다시 넣으면 아무 일도 하지 않고
	// 돌아가므로, 통계 주기에 맞춰 매번 불러도 비용이 없다.
	void SetQualityInfo(const StreamingQualityInfo& info);

	// --- 진단 오버레이 ---
	//
	// 기본은 꺼짐이다. 컨트롤 바의 막대 그래프 버튼으로도 켜고 끌 수 있고,
	// 호스트가 여기서 직접 켜도 된다(단축키를 붙이는 경우).
	//
	// SetStatsInfo 를 부르는 주기가 곧 스파크라인의 표본 간격이다.
	// 500ms 로 넣으면 60칸이 30초 창이 된다. 오버레이가 꺼져 있어도
	// 표본은 쌓이므로, 켜는 순간 지난 30초가 이미 그려져 있다.
	void SetStatsInfo(const StreamingStatsInfo& stats);

	void SetStatsVisible(bool visible);
	bool IsStatsVisible() const;

	// --- 자동 숨김 ---
	//
	// 마우스가 멈춰 있으면 바가 서서히 사라지고, 움직이면 다시 나타난다.
	// 숨겨진 동안에는 입력도 받지 않는다 — 안 보이는 버튼이 클릭을 먹으면
	// 사용자는 이유를 알 수 없다.
	void SetAutoHide(bool enabled);
	bool IsAutoHideEnabled() const;
	void SetAutoHideDelay(float seconds);

	void SetVisible(bool visible);
	bool IsVisible() const;

private:
	// Attach 시점에 레이어로 넘길 설정을 한곳에서 적용한다.
	void ApplyPendingSettings();

	// 레이어가 애니메이션 중일 때 프레임을 더 요청하는 통로.
	// 뷰어는 움직이는 것이 없으면 그리지 않으므로, 페이드가 진행되려면
	// 누군가 프레임을 달라고 해야 한다.
	static void RequestFrame(void* userData);

	// 컨트롤 바의 오버레이 토글이 눌렸다.
	static void ToggleStats(void* userData);

private:
	D3D11ImageView* m_viewer = nullptr;
	ServiceControlLayer* m_layer = nullptr;
	ServiceStatsLayer* m_statsLayer = nullptr;

	// Attach 이전에 들어온 설정을 여기 담아 두었다가 레이어가 생기면
	// 그대로 넘긴다. 호스트가 Attach 순서를 신경 쓰지 않아도 되게 한다.
	CommandCallback m_commandCallback = nullptr;
	void* m_commandUserData = nullptr;

	StreamingPlaybackState m_playbackState = StreamingPlaybackState::Stopped;
	float m_latency = 0.0f;
	float m_latencyRange = 500.0f;
	StreamingQualityInfo m_qualityInfo = {};
	StreamingStatsInfo m_statsInfo = {};
	bool m_statsVisible = false;
	bool m_autoHide = true;
	float m_autoHideDelay = 2.5f;
	bool m_visible = true;
};
