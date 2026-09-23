#pragma once

#include <stdint.h>

#ifdef BUILD_STREAMING_VIEWER_UI_DLL
#define STREAMING_VIEWER_UI_API __declspec(dllexport)
#else
#define STREAMING_VIEWER_UI_API __declspec(dllimport)
#endif

class D3D11ImageView;
class ServiceControlLayer;

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

// 화질 항목 하나.
//
// 라벨은 호스트가 정한다. 뷰어도 이 DLL 도 "1080p" 가 무슨 뜻인지 알 필요가
// 없고, 고른 결과는 인덱스로만 돌아간다.
struct StreamingQualityOption
{
	const wchar_t* label = nullptr;
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
	using VolumeCallback = void (*)(float volume, void* userData);

	// 사용자가 화질을 골랐다. index 는 SetQualityOptions 에 넘긴 배열의 위치다.
	using QualityCallback = void (*)(uint32_t index, void* userData);

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
	void SetVolumeCallback(VolumeCallback callback, void* userData);
	void SetQualityCallback(QualityCallback callback, void* userData);

public:
	// 재생 상태에 따라 버튼 모양이 바뀐다. 상태를 아는 것은 호스트이므로
	// 컨트롤 바가 스스로 바꾸지 않는다 — 눌렸다고 재생이 시작된다는 보장이
	// 없기 때문이다(연결이 끊겨 있을 수 있다).
	void SetPlaybackState(StreamingPlaybackState state);
	StreamingPlaybackState GetPlaybackState() const;

	// 0.0 ~ 1.0. 사용자가 슬라이더를 움직이면 콜백이 오고, 호스트가 이
	// 함수로 되돌려 주면 콜백은 오지 않는다(순환 방지).
	void SetVolume(float volume);
	float GetVolume() const;

	// --- 지연 표시 ---
	//
	// 라이브라 되감기가 없으므로 타임라인이 아니다. 대신 "지금 화면이
	// 실제보다 얼마나 뒤처져 있는가" 를 보여준다. 되감기 없는 스트림에서
	// 사용자가 알고 싶은 것은 그것뿐이다.
	//
	// 눈금 상한은 SetLatencyRange 로 정한다. 넘어가면 가득 찬 채로 멈춘다.
	void SetLatency(float milliseconds);
	void SetLatencyRange(float maxMilliseconds);
	float GetLatency() const;

	// --- 화질 선택 ---
	//
	// options 는 이 호출 동안만 읽는다. 라벨 문자열은 DLL 이 복사하므로
	// 호출자가 계속 들고 있을 필요가 없다.
	void SetQualityOptions(const StreamingQualityOption* options, uint32_t count);
	void SetSelectedQuality(uint32_t index);
	uint32_t GetSelectedQuality() const;

	// --- 자동 숨김 ---
	//
	// 마우스가 멈춰 있으면 바가 서서히 사라지고, 움직이면 다시 나타난다.
	// 숨겨진 동안에는 입력도 받지 않는다 — 안 보이는 버튼이 클릭을 먹으면
	// 사용자는 이유를 알 수 없다.
	//
	// 화질 메뉴가 열려 있는 동안에는 숨지 않는다.
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

private:
	D3D11ImageView* m_viewer = nullptr;
	ServiceControlLayer* m_layer = nullptr;

	// Attach 이전에 들어온 설정을 여기 담아 두었다가 레이어가 생기면
	// 그대로 넘긴다. 호스트가 Attach 순서를 신경 쓰지 않아도 되게 한다.
	CommandCallback m_commandCallback = nullptr;
	void* m_commandUserData = nullptr;

	VolumeCallback m_volumeCallback = nullptr;
	void* m_volumeUserData = nullptr;

	QualityCallback m_qualityCallback = nullptr;
	void* m_qualityUserData = nullptr;

	StreamingPlaybackState m_playbackState = StreamingPlaybackState::Stopped;
	float m_volume = 0.7f;
	float m_latency = 0.0f;
	float m_latencyRange = 500.0f;
	uint32_t m_selectedQuality = 0;
	bool m_autoHide = true;
	float m_autoHideDelay = 2.5f;
	bool m_visible = true;
};
