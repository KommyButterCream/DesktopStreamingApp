#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <memory>
#include <string>
#include <vector>

#include "../../../../Module/D3D11EngineInterface/IUIRenderLayer.h"
#include "../../../../Module/D3D11EngineInterface/IDeviceEventListener.h"
#include "../../../../Module/D3D11EngineInterface/IResizeEventListener.h"

#include "StreamingViewerUI.h"
#include "ServiceGlyphButton.h"
#include "ServiceMenuButton.h"

class IRenderContext;
class UISlider;
class UILabel;
class UIContextMenuPanel;
class FontManager;

struct ID2D1SolidColorBrush;

// 뷰어 위에 얹히는 스트리밍 컨트롤 바.
//
// 뷰어 내부 레이어와 같은 계약을 쓴다(IRenderLayer / IUIRenderLayer +
// 두 리스너). 새로 배울 인터페이스가 없고, 뷰어는 이 클래스의 존재를
// 컴파일 타임에 알지 못한다.
//
// 배치
//   화면 하단 가운데에 가로로 붙는다. 창 크기가 바뀌면 OnResize 가 다시
//   잡는다. 좌표계는 창 픽셀이다(뷰어의 윈도우 오버레이와 같다).
//
//   [▶] [■]   지연 ---------- 123 ms   [화질 ▾]   볼륨 ----●--
class ServiceControlLayer
	: public IUIRenderLayer
	, public IResizeEventListener
	, public IDeviceEventListener
{
public:
	// 애니메이션이 진행되려면 프레임이 계속 필요하다. 뷰어는 움직이는
	// 것이 없으면 그리지 않으므로 여기서 요청한다.
	using FrameRequestCallback = void (*)(void* userData);

	ServiceControlLayer();
	virtual ~ServiceControlLayer();

	ServiceControlLayer(const ServiceControlLayer&) = delete;
	ServiceControlLayer& operator=(const ServiceControlLayer&) = delete;

public:
	// IRenderLayer Override
	bool Initialize(IRenderContext* context) override;
	void Shutdown() override;
	bool Prepare() override;
	bool Render() override;

	// IUIRenderLayer Override
	bool HitTest(float x, float y) const override;
	bool OnMouseEvent(UIMouseEventType type, float x, float y) override;
	UIElementState GetState() const override;
	void SetState(UIElementState state) override;
	bool IsVisible() const override;
	void SetVisible(bool visible) override;

	// IResizeEventListener Override
	void OnResize(uint32_t width, uint32_t height) override;

	// IDeviceEventListener Override
	void OnDeviceLost() override;
	void OnDeviceRestored() override;

public:
	void SetFrameRequestCallback(FrameRequestCallback callback, void* userData);

	void SetCommandCallback(StreamingViewerUI::CommandCallback callback, void* userData);
	void SetVolumeCallback(StreamingViewerUI::VolumeCallback callback, void* userData);
	void SetQualityCallback(StreamingViewerUI::QualityCallback callback, void* userData);

	void SetPlaybackState(StreamingPlaybackState state);
	StreamingPlaybackState GetPlaybackState() const;

	void SetVolume(float volume);
	float GetVolume() const;

	void SetLatency(float milliseconds);
	void SetLatencyRange(float maxMilliseconds);
	float GetLatency() const;

	void SetQualityOptions(const StreamingQualityOption* options, uint32_t count);
	void SetSelectedQuality(uint32_t index);
	uint32_t GetSelectedQuality() const;

	void SetAutoHide(bool enabled);
	bool IsAutoHideEnabled() const;
	void SetAutoHideDelay(float seconds);

private:
	bool CreateChildren(IRenderContext* context);
	bool CreateQualityMenu(IRenderContext* context);
	bool CreateDeviceResources(IRenderContext* context);
	void ReleaseDeviceResources();

	// 하단 바와 그 안의 요소 위치를 다시 잡는다.
	// 뷰 크기를 모르면(초기화 직후) 아무것도 하지 않는다.
	void UpdateLayout();

	// 재생 상태에 따라 가운데 버튼을 재생/일시정지로 바꾼다.
	void ApplyPlaybackState();

	// 지연 값을 바와 라벨에 반영한다.
	void ApplyLatency();

	// 화질 버튼의 표시 문구를 현재 선택으로 맞춘다.
	void ApplyQualityLabel();

	// 자동 숨김 알파를 dt 만큼 진행한다. 값이 움직였으면 true.
	bool AdvanceFade(float deltaSeconds);

	// 사용자가 뭔가 했다. 숨김 타이머를 처음으로 되돌린다.
	void NotifyUserActivity();

	void RequestFrame();

	bool IsQualityMenuOpen() const;
	void OpenQualityMenu();
	void CloseQualityMenu();

	static void OnGlyphButtonClicked(uint32_t commandId, void* userData);
	static void OnQualityButtonClicked(uint32_t commandId, void* userData);
	static void OnQualityItemClicked(uint32_t index, void* userData);
	static void OnVolumeChanged(float value, void* userData);

	void InvokeCommand(StreamingViewerCommand command);

private:
	IRenderContext* m_context = nullptr;
	FontManager* m_fontManager = nullptr;
	bool m_visible = true;

	// 창 크기. OnResize 가 채우고 레이아웃이 읽는다.
	float m_viewWidth = 0.0f;
	float m_viewHeight = 0.0f;

	// 바 배경. 영상 위에 반투명으로 깔린다.
	Core::ShapeType::Rect2f m_barRect = {};
	ID2D1SolidColorBrush* m_barBrush = nullptr;

	std::unique_ptr<ServiceGlyphButton> m_playPauseButton = nullptr;
	std::unique_ptr<ServiceGlyphButton> m_stopButton = nullptr;
	std::unique_ptr<UISlider> m_latencyBar = nullptr;
	std::unique_ptr<UILabel> m_latencyLabel = nullptr;
	std::unique_ptr<ServiceMenuButton> m_qualityButton = nullptr;
	std::unique_ptr<UISlider> m_volumeSlider = nullptr;

	// 화질 팝업. 프레임워크의 컨텍스트 메뉴를 그대로 쓴다.
	std::unique_ptr<UIContextMenuPanel> m_qualityMenu = nullptr;
	std::vector<std::shared_ptr<ServiceMenuButton>> m_qualityItems;
	std::vector<std::wstring> m_qualityLabels;
	uint32_t m_selectedQuality = 0;

	StreamingPlaybackState m_playbackState = StreamingPlaybackState::Stopped;

	float m_latency = 0.0f;
	float m_latencyRange = 500.0f;

	// --- 자동 숨김 ---
	bool m_autoHide = true;
	float m_autoHideDelay = 2.5f;
	float m_idleSeconds = 0.0f;

	// 0 = 완전히 숨김, 1 = 완전히 보임. 그 사이는 페이드 중이다.
	float m_alpha = 1.0f;

	FrameRequestCallback m_frameRequestCallback = nullptr;
	void* m_frameRequestUserData = nullptr;

	StreamingViewerUI::CommandCallback m_commandCallback = nullptr;
	void* m_commandUserData = nullptr;

	StreamingViewerUI::VolumeCallback m_volumeCallback = nullptr;
	void* m_volumeUserData = nullptr;

	StreamingViewerUI::QualityCallback m_qualityCallback = nullptr;
	void* m_qualityUserData = nullptr;
};
