#include "ServiceControlLayer.h"

#include "../../../../Module/D3D11EngineInterface/IRenderContext.h"
#include "../../../../Module/D3D11EngineInterface/IRenderEngine.h"
#include "../../../../Module/D3D11UIFramework/D3D11UIFramework/Slider/UISlider.h"
#include "../../../../Module/D3D11UIFramework/D3D11UIFramework/Label/UILabel.h"
#include "../../../../Module/D3D11UIFramework/D3D11UIFramework/Panel/UIContextMenuPanel.h"
#include "../../../../Module/Core/ShapeType/Point2f.h"
#include "../../../../Module/Core/DirectX/DxSafeRelease.h"

#include <algorithm>
#include <stdio.h>

using namespace Core::DirectX;
using namespace Core::ShapeType;

namespace
{
	// 바 치수. 창 크기와 무관한 고정값이다 — 컨트롤은 영상 크기가 아니라
	// 손가락 크기에 맞춰야 한다.
	constexpr float kBarHeight = 44.0f;
	constexpr float kBarMargin = 12.0f;
	constexpr float kBarPadding = 10.0f;
	constexpr float kButtonSize = 28.0f;
	constexpr float kGap = 8.0f;

	constexpr float kVolumeWidth = 96.0f;
	constexpr float kQualityWidth = 76.0f;
	constexpr float kLatencyLabelWidth = 62.0f;
	constexpr float kLatencyBarMinWidth = 60.0f;

	// 바가 차지하는 폭의 상한. 창이 아주 넓어도 컨트롤이 끝까지 늘어나면
	// 시선이 양쪽 끝으로 갈라진다.
	constexpr float kMaxBarWidth = 720.0f;

	// 이보다 좁으면 오른쪽 것부터 접는다.
	constexpr float kMinBarWidth = 240.0f;

	constexpr float kMenuWidth = 120.0f;
	constexpr float kMenuItemHeight = 26.0f;

	// 메뉴 안쪽 여백과 항목 간격. 패널의 UpdateVerticalLayout 과
	// 높이 계산이 둘 다 이 값을 쓴다. 한 곳에 모아 둔다.
	constexpr float kMenuPadding = 4.0f;
	constexpr float kMenuSpacing = 0.0f;

	// 페이드가 완전히 열리고 닫히는 데 걸리는 시간.
	constexpr float kFadeSeconds = 0.18f;

	// 화질 버튼이 쓰는 명령 값. StreamingViewerCommand 와 겹치지 않게
	// 큰 값으로 띄운다 — 두 콜백이 같은 함수 시그니처를 쓰기 때문이다.
	constexpr uint32_t kQualityButtonCommandId = 1000;

	D2D1_COLOR_F Rgba(float r, float g, float b, float a)
	{
		return D2D1::ColorF(r, g, b, a);
	}

	// 알파를 곱한 사본. 페이드 중에 자식 색을 통째로 바꾸는 대신
	// 레이어 불투명도로 처리하므로, 이 함수는 바 배경에만 쓴다.
	D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& color, float alpha)
	{
		return D2D1::ColorF(color.r, color.g, color.b, color.a * alpha);
	}
}

ServiceControlLayer::ServiceControlLayer() = default;

ServiceControlLayer::~ServiceControlLayer()
{
	Shutdown();
}

bool ServiceControlLayer::Initialize(IRenderContext* context)
{
	if (!context)
		return false;

	m_context = context;

	// 텍스트를 쓰는 요소(지연 라벨, 화질 메뉴)가 폰트 매니저를 요구한다.
	// 뷰어가 자기 엔진을 만들 때 initFontManager 를 켜므로 여기 있다.
	if (IRenderEngine* engine = m_context->GetEngine())
	{
		m_fontManager = engine->GetFontManager();
	}

	if (!CreateChildren(context))
	{
		Shutdown();
		return false;
	}

	if (!CreateQualityMenu(context))
	{
		Shutdown();
		return false;
	}

	if (!CreateDeviceResources(context))
	{
		Shutdown();
		return false;
	}

	// 뷰어 내부 레이어와 같은 방식으로 스스로 등록한다. 뷰어가 대신
	// 등록해 주는 통로를 두지 않은 이유는, 그러면 레이어마다 무엇을
	// 구현했는지 뷰어가 알아야 하기 때문이다.
	m_context->AddResizeListener(this);
	m_context->AddDeviceListener(this);

	m_viewWidth = static_cast<float>(m_context->GetWidth());
	m_viewHeight = static_cast<float>(m_context->GetHeight());
	UpdateLayout();

	ApplyPlaybackState();
	ApplyLatency();
	ApplyQualityLabel();

	return true;
}

void ServiceControlLayer::Shutdown()
{
	if (m_context)
	{
		m_context->RemoveResizeListener(this);
		m_context->RemoveDeviceListener(this);
	}

	// 자식이 들고 있는 D2D 리소스가 컨텍스트보다 오래 살면 안 된다.
	m_qualityItems.clear();
	m_qualityMenu.reset();

	m_volumeSlider.reset();
	m_qualityButton.reset();
	m_latencyLabel.reset();
	m_latencyBar.reset();
	m_stopButton.reset();
	m_playPauseButton.reset();

	ReleaseDeviceResources();

	m_context = nullptr;
}

bool ServiceControlLayer::Prepare()
{
	// 텍스트 레이아웃은 그리기 전에 확정돼야 한다. 뷰어가 Render 직전에
	// 이 함수를 불러 준다.
	if (m_latencyLabel)
		m_latencyLabel->Prepare();

	if (m_qualityButton)
		m_qualityButton->Prepare();

	if (m_qualityMenu && m_qualityMenu->IsVisible())
		m_qualityMenu->Prepare();

	return true;
}

bool ServiceControlLayer::Render()
{
	if (!m_visible || !m_context || !m_barBrush)
		return false;

	ID2D1DeviceContext* d2dContext = m_context->GetD2DDeviceContext();
	if (!d2dContext)
		return false;

	// 창이 아직 배치를 못 받았다.
	if (m_barRect.right <= m_barRect.left)
		return false;

	// 시간 진행. 뷰어가 계산해 둔 프레임 간격을 그대로 쓴다.
	const float deltaSeconds = m_context->GetDeltaTime();

	if (m_qualityMenu && m_qualityMenu->IsVisible())
	{
		// 메뉴가 열려 있는 동안에는 숨지 않는다. 고르는 도중에 사라지면
		// 그보다 나쁜 동작이 없다.
		m_idleSeconds = 0.0f;
		m_qualityMenu->Update(deltaSeconds);
	}
	else
	{
		m_idleSeconds += deltaSeconds;
	}

	if (AdvanceFade(deltaSeconds))
	{
		// 아직 움직이는 중이다. 다음 프레임을 달라고 한다.
		RequestFrame();
	}

	if (m_alpha <= 0.0f)
		return false;

	// 페이드 중에만 레이어를 쌓는다.
	//
	// 다 보이거나 다 숨은 상태에서는 불투명도를 섞을 이유가 없고,
	// PushLayer 는 중간 표면을 잡으므로 공짜가 아니다.
	const bool needsOpacityLayer = (m_alpha < 1.0f);
	if (needsOpacityLayer)
	{
		// 바와 (열려 있다면) 메뉴를 함께 덮는 범위로 한정한다.
		D2D1_RECT_F bounds = D2D1::RectF(
			m_barRect.left, m_barRect.top, m_barRect.right, m_barRect.bottom);

		if (m_qualityMenu && m_qualityMenu->IsVisible())
		{
			const auto& menu = m_qualityMenu->GetLayout();
			bounds.left = (std::min)(bounds.left, menu.left);
			bounds.top = (std::min)(bounds.top, menu.top);
			bounds.right = (std::max)(bounds.right, menu.right);
			bounds.bottom = (std::max)(bounds.bottom, menu.bottom);
		}

		d2dContext->PushLayer(
			D2D1::LayerParameters1(
				bounds, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
				D2D1::IdentityMatrix(), m_alpha, nullptr, D2D1_LAYER_OPTIONS1_NONE),
			nullptr);
	}

	const D2D1_ROUNDED_RECT bar = D2D1::RoundedRect(
		D2D1::RectF(m_barRect.left, m_barRect.top, m_barRect.right, m_barRect.bottom), 8.0f, 8.0f);

	d2dContext->FillRoundedRectangle(bar, m_barBrush);

	if (m_playPauseButton) m_playPauseButton->Render();
	if (m_stopButton)      m_stopButton->Render();
	if (m_latencyBar)      m_latencyBar->Render();
	if (m_latencyLabel)    m_latencyLabel->Render();
	if (m_qualityButton)   m_qualityButton->Render();
	if (m_volumeSlider)    m_volumeSlider->Render();

	// 팝업은 바 위로 삐져나오므로 마지막에 그린다.
	if (m_qualityMenu && m_qualityMenu->IsVisible())
	{
		m_qualityMenu->Render();
	}

	if (needsOpacityLayer)
	{
		d2dContext->PopLayer();
	}

	return true;
}

bool ServiceControlLayer::HitTest(float x, float y) const
{
	if (!m_visible || m_alpha <= 0.0f)
		return false;

	if (m_qualityMenu && m_qualityMenu->IsVisible() && m_qualityMenu->HitTest(x, y))
		return true;

	return (x >= m_barRect.left) && (x <= m_barRect.right)
		&& (y >= m_barRect.top) && (y <= m_barRect.bottom);
}

bool ServiceControlLayer::OnMouseEvent(UIMouseEventType type, float x, float y)
{
	if (!m_visible)
		return false;

	// 창을 벗어났다. 모든 자식의 hover / 드래그를 푼다. 바 밖이라고
	// 건너뛰면 하이라이트가 고착된다.
	if (type == UIMouseEventType::Leave)
	{
		if (m_playPauseButton) m_playPauseButton->OnMouseEvent(type, x, y);
		if (m_stopButton)      m_stopButton->OnMouseEvent(type, x, y);
		if (m_qualityButton)   m_qualityButton->OnMouseEvent(type, x, y);
		if (m_volumeSlider)    m_volumeSlider->OnMouseEvent(type, x, y);
		if (m_qualityMenu)     m_qualityMenu->OnMouseEvent(type, x, y);
		return false;
	}

	// 마우스가 움직였다는 것만으로 바를 다시 띄운다. 숨어 있는 동안에도
	// 이 경로는 살아 있어야 한다 — 그래야 다시 나타날 수 있다.
	if (type == UIMouseEventType::Move)
	{
		NotifyUserActivity();
	}

	// 숨어 있는 동안에는 클릭을 받지 않는다. 안 보이는 버튼이 클릭을
	// 먹으면 사용자는 이유를 알 수 없다.
	if (m_alpha <= 0.0f)
		return false;

	// 열린 메뉴가 최우선이다. 바깥을 누르면 닫고 그 클릭은 삼킨다 —
	// 메뉴를 닫는 클릭이 뒤의 영상까지 건드리면 안 된다.
	if (m_qualityMenu && m_qualityMenu->IsVisible())
	{
		if (m_qualityMenu->OnMouseEvent(type, x, y))
		{
			NotifyUserActivity();
			RequestFrame();
			return true;
		}

		if (type == UIMouseEventType::LButtonDown && !m_qualityMenu->HitTest(x, y))
		{
			CloseQualityMenu();
			RequestFrame();
			return true;
		}
	}

	// 끌고 있는 요소가 있으면 좌표가 밖으로 나가도 그쪽이 먼저 받는다.
	// 슬라이더를 잡고 바 밖으로 끌어도 값이 따라와야 한다.
	if (m_volumeSlider && m_volumeSlider->IsDragging())
	{
		if (m_volumeSlider->OnMouseEvent(type, x, y))
		{
			NotifyUserActivity();
			RequestFrame();
			return true;
		}
	}

	const bool insideBar = (x >= m_barRect.left) && (x <= m_barRect.right)
		&& (y >= m_barRect.top) && (y <= m_barRect.bottom);

	if (!insideBar)
	{
		// 바 밖이다. 자식들의 hover 만 풀어 주고 이벤트는 넘긴다 —
		// 여기서 소비하면 영상 위 팬/줌이 죽는다.
		if (m_playPauseButton) m_playPauseButton->OnMouseEvent(UIMouseEventType::Move, -1.0f, -1.0f);
		if (m_stopButton)      m_stopButton->OnMouseEvent(UIMouseEventType::Move, -1.0f, -1.0f);
		if (m_qualityButton)   m_qualityButton->OnMouseEvent(UIMouseEventType::Move, -1.0f, -1.0f);
		if (m_volumeSlider)    m_volumeSlider->OnMouseEvent(UIMouseEventType::Move, -1.0f, -1.0f);
		return false;
	}

	NotifyUserActivity();

	bool consumed = false;
	if (m_playPauseButton && m_playPauseButton->OnMouseEvent(type, x, y)) consumed = true;
	else if (m_stopButton && m_stopButton->OnMouseEvent(type, x, y))      consumed = true;
	else if (m_qualityButton && m_qualityButton->OnMouseEvent(type, x, y)) consumed = true;
	else if (m_volumeSlider && m_volumeSlider->OnMouseEvent(type, x, y))  consumed = true;

	RequestFrame();

	// 바 안이면 어떤 자식에도 맞지 않아도 소비한다 — 컨트롤 바의 빈 자리를
	// 눌렀는데 뒤의 영상이 팬 되면 안 된다.
	return true;
}

UIElementState ServiceControlLayer::GetState() const
{
	return UIElementState::Normal;
}

void ServiceControlLayer::SetState(UIElementState)
{
	// 컨테이너 자체는 상태를 갖지 않는다. 상태는 자식마다 따로다.
}

bool ServiceControlLayer::IsVisible() const
{
	return m_visible;
}

void ServiceControlLayer::SetVisible(bool visible)
{
	m_visible = visible;
}

void ServiceControlLayer::OnResize(uint32_t width, uint32_t height)
{
	m_viewWidth = static_cast<float>(width);
	m_viewHeight = static_cast<float>(height);

	CloseQualityMenu();
	UpdateLayout();
}

void ServiceControlLayer::OnDeviceLost()
{
	ReleaseDeviceResources();

	if (m_playPauseButton) m_playPauseButton->DiscardDeviceResources();
	if (m_stopButton)      m_stopButton->DiscardDeviceResources();
	if (m_latencyBar)      m_latencyBar->DiscardDeviceResources();
	if (m_latencyLabel)    m_latencyLabel->DiscardDeviceResources();
	if (m_qualityButton)   m_qualityButton->DiscardDeviceResources();
	if (m_volumeSlider)    m_volumeSlider->DiscardDeviceResources();
	if (m_qualityMenu)     m_qualityMenu->DiscardDeviceResources();
}

void ServiceControlLayer::OnDeviceRestored()
{
	if (!m_context)
		return;

	CreateDeviceResources(m_context);

	if (m_playPauseButton) m_playPauseButton->RestoreDeviceResources(m_context);
	if (m_stopButton)      m_stopButton->RestoreDeviceResources(m_context);
	if (m_latencyBar)      m_latencyBar->RestoreDeviceResources(m_context);
	if (m_latencyLabel)    m_latencyLabel->RestoreDeviceResources(m_context);
	if (m_qualityButton)   m_qualityButton->RestoreDeviceResources(m_context);
	if (m_volumeSlider)    m_volumeSlider->RestoreDeviceResources(m_context);
	if (m_qualityMenu)     m_qualityMenu->RestoreDeviceResources(m_context);

	UpdateLayout();
}

void ServiceControlLayer::SetFrameRequestCallback(FrameRequestCallback callback, void* userData)
{
	m_frameRequestCallback = callback;
	m_frameRequestUserData = userData;
}

void ServiceControlLayer::SetCommandCallback(StreamingViewerUI::CommandCallback callback, void* userData)
{
	m_commandCallback = callback;
	m_commandUserData = userData;
}

void ServiceControlLayer::SetVolumeCallback(StreamingViewerUI::VolumeCallback callback, void* userData)
{
	m_volumeCallback = callback;
	m_volumeUserData = userData;
}

void ServiceControlLayer::SetQualityCallback(StreamingViewerUI::QualityCallback callback, void* userData)
{
	m_qualityCallback = callback;
	m_qualityUserData = userData;
}

void ServiceControlLayer::SetPlaybackState(StreamingPlaybackState state)
{
	if (m_playbackState == state)
		return;

	m_playbackState = state;
	ApplyPlaybackState();
}

StreamingPlaybackState ServiceControlLayer::GetPlaybackState() const
{
	return m_playbackState;
}

void ServiceControlLayer::SetVolume(float volume)
{
	if (m_volumeSlider)
	{
		m_volumeSlider->SetValue(volume);
	}
}

float ServiceControlLayer::GetVolume() const
{
	return m_volumeSlider ? m_volumeSlider->GetValue() : 0.0f;
}

void ServiceControlLayer::SetLatency(float milliseconds)
{
	m_latency = (milliseconds > 0.0f) ? milliseconds : 0.0f;
	ApplyLatency();
}

void ServiceControlLayer::SetLatencyRange(float maxMilliseconds)
{
	if (maxMilliseconds <= 0.0f)
		return;

	m_latencyRange = maxMilliseconds;

	if (m_latencyBar)
	{
		m_latencyBar->SetRange(0.0f, m_latencyRange);
	}

	ApplyLatency();
}

float ServiceControlLayer::GetLatency() const
{
	return m_latency;
}

void ServiceControlLayer::SetQualityOptions(const StreamingQualityOption* options, uint32_t count)
{
	// 라벨을 복사한다. 호출자가 리터럴을 넘기든 임시 버퍼를 넘기든
	// 이쪽 수명과 얽히지 않게 한다.
	m_qualityLabels.clear();
	for (uint32_t index = 0; index < count; ++index)
	{
		const wchar_t* label = options ? options[index].label : nullptr;
		m_qualityLabels.emplace_back(label ? label : L"");
	}

	if (m_selectedQuality >= m_qualityLabels.size())
	{
		m_selectedQuality = 0;
	}

	if (m_context)
	{
		CloseQualityMenu();
		CreateQualityMenu(m_context);
		UpdateLayout();
	}

	ApplyQualityLabel();
}

void ServiceControlLayer::SetSelectedQuality(uint32_t index)
{
	if (index >= m_qualityLabels.size())
		return;

	m_selectedQuality = index;

	for (size_t itemIndex = 0; itemIndex < m_qualityItems.size(); ++itemIndex)
	{
		if (m_qualityItems[itemIndex])
		{
			m_qualityItems[itemIndex]->SetChecked(itemIndex == m_selectedQuality);
		}
	}

	ApplyQualityLabel();
}

uint32_t ServiceControlLayer::GetSelectedQuality() const
{
	return m_selectedQuality;
}

void ServiceControlLayer::SetAutoHide(bool enabled)
{
	m_autoHide = enabled;

	if (!enabled)
	{
		m_alpha = 1.0f;
	}

	m_idleSeconds = 0.0f;
	RequestFrame();
}

bool ServiceControlLayer::IsAutoHideEnabled() const
{
	return m_autoHide;
}

void ServiceControlLayer::SetAutoHideDelay(float seconds)
{
	if (seconds < 0.0f)
		return;

	m_autoHideDelay = seconds;
	m_idleSeconds = 0.0f;
}

bool ServiceControlLayer::CreateChildren(IRenderContext* context)
{
	UIStyle buttonStyle = {};
	buttonStyle.borderThickness = 0.0f;
	buttonStyle.normal.fill = Rgba(1.0f, 1.0f, 1.0f, 0.10f);
	buttonStyle.hover.fill = Rgba(1.0f, 1.0f, 1.0f, 0.24f);
	buttonStyle.pressed.fill = Rgba(1.0f, 1.0f, 1.0f, 0.36f);
	buttonStyle.disabled.fill = Rgba(1.0f, 1.0f, 1.0f, 0.05f);

	m_playPauseButton = std::make_unique<ServiceGlyphButton>();
	m_playPauseButton->SetStyle(buttonStyle);
	m_playPauseButton->SetGlyph(ServiceGlyph::Play);
	m_playPauseButton->SetCommandId(static_cast<uint32_t>(StreamingViewerCommand::Play));
	m_playPauseButton->SetClickCallback(&ServiceControlLayer::OnGlyphButtonClicked, this);
	if (!m_playPauseButton->Initialize(context))
		return false;

	m_stopButton = std::make_unique<ServiceGlyphButton>();
	m_stopButton->SetStyle(buttonStyle);
	m_stopButton->SetGlyph(ServiceGlyph::Stop);
	m_stopButton->SetCommandId(static_cast<uint32_t>(StreamingViewerCommand::Stop));
	m_stopButton->SetClickCallback(&ServiceControlLayer::OnGlyphButtonClicked, this);
	if (!m_stopButton->Initialize(context))
		return false;

	// 지연 바. 라이브라 되감기가 없으므로 끌 수 없다.
	m_latencyBar = std::make_unique<UISlider>();
	m_latencyBar->SetRange(0.0f, m_latencyRange);
	m_latencyBar->SetValue(0.0f);
	m_latencyBar->SetInteractive(false);
	m_latencyBar->SetTrackThickness(4.0f);
	m_latencyBar->SetThumbRadius(0.0f);
	m_latencyBar->SetTrackColor(Rgba(1.0f, 1.0f, 1.0f, 0.18f));
	m_latencyBar->SetFillColor(Rgba(0.40f, 0.80f, 0.55f, 1.0f));
	if (!m_latencyBar->Initialize(context))
		return false;

	UITextStyle textStyle = {};
	textStyle.fontSize = 12.0f;
	textStyle.normal.fill = Rgba(0.88f, 0.90f, 0.93f, 1.0f);
	textStyle.hover.fill = textStyle.normal.fill;
	textStyle.pressed.fill = textStyle.normal.fill;
	textStyle.disabled.fill = Rgba(0.55f, 0.57f, 0.60f, 1.0f);
	textStyle.hAlign = DWRITE_TEXT_ALIGNMENT_LEADING;
	textStyle.vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

	m_latencyLabel = std::make_unique<UILabel>();
	m_latencyLabel->SetFontManager(m_fontManager);
	m_latencyLabel->SetTextStyle(textStyle);
	m_latencyLabel->SetText(L"-- ms");
	if (!m_latencyLabel->Initialize(context))
		return false;

	// 화질 버튼. 항목이 하나뿐이거나 없으면 레이아웃에서 숨긴다.
	UITextStyle qualityTextStyle = textStyle;
	qualityTextStyle.hAlign = DWRITE_TEXT_ALIGNMENT_CENTER;

	m_qualityButton = std::make_unique<ServiceMenuButton>();
	m_qualityButton->SetFontManager(m_fontManager);
	m_qualityButton->SetStyle(buttonStyle);
	m_qualityButton->SetTextStyle(qualityTextStyle);
	m_qualityButton->SetCheckable(false);
	m_qualityButton->SetHasSubMenu(false);
	m_qualityButton->SetIconAreaWidth(0.0f);
	m_qualityButton->SetExtraAreaWidth(0.0f);
	m_qualityButton->SetCommandId(kQualityButtonCommandId);
	m_qualityButton->SetClickCallback(&ServiceControlLayer::OnQualityButtonClicked, this);
	if (!m_qualityButton->Initialize(context))
		return false;

	m_volumeSlider = std::make_unique<UISlider>();
	m_volumeSlider->SetRange(0.0f, 1.0f);
	m_volumeSlider->SetValue(0.7f);
	m_volumeSlider->SetTrackThickness(4.0f);
	m_volumeSlider->SetThumbRadius(6.0f);
	m_volumeSlider->SetTrackColor(Rgba(1.0f, 1.0f, 1.0f, 0.22f));
	m_volumeSlider->SetFillColor(Rgba(0.42f, 0.70f, 1.0f, 1.0f));
	m_volumeSlider->SetThumbColor(Rgba(1.0f, 1.0f, 1.0f, 1.0f));
	m_volumeSlider->SetValueChangedCallback(&ServiceControlLayer::OnVolumeChanged, this);
	if (!m_volumeSlider->Initialize(context))
		return false;

	return true;
}

bool ServiceControlLayer::CreateQualityMenu(IRenderContext* context)
{
	// 목록이 바뀔 때마다 통째로 다시 만든다. 항목 수가 한 자릿수라
	// 재사용할 이유가 없고, 부분 갱신은 체크 상태를 놓치기 쉽다.
	m_qualityItems.clear();
	m_qualityMenu.reset();

	if (m_qualityLabels.empty())
		return true;

	UIStyle menuStyle = {};
	menuStyle.borderThickness = 1.0f;
	menuStyle.normal.fill = Rgba(0.13f, 0.14f, 0.16f, 0.97f);
	menuStyle.normal.border = Rgba(1.0f, 1.0f, 1.0f, 0.14f);
	menuStyle.hover.fill = menuStyle.normal.fill;
	menuStyle.pressed.fill = menuStyle.normal.fill;
	menuStyle.disabled.fill = menuStyle.normal.fill;

	m_qualityMenu = std::make_unique<UIContextMenuPanel>();
	m_qualityMenu->SetStyle(menuStyle);
	m_qualityMenu->SetMenuWidth(kMenuWidth);
	m_qualityMenu->SetPadding(kMenuPadding);
	m_qualityMenu->SetSpacing(kMenuSpacing);

	// 기본값은 None 이라 패널이 자식 위치를 손대지 않는다.
	// 그러면 항목이 제 자리 값(0,0)을 그대로 쓰며 창 좌상단에
	// 겹쳐 그려진다. 메뉴는 세로 목록이다.
	m_qualityMenu->SetLayoutType(UILayoutType::Vertical);
	m_qualityMenu->SetRounded(true);
	m_qualityMenu->SetCornerRadius(4.0f);

	if (!m_qualityMenu->Initialize(context))
	{
		m_qualityMenu.reset();
		return false;
	}

	UIStyle itemStyle = {};
	itemStyle.borderThickness = 0.0f;
	itemStyle.normal.fill = Rgba(0.0f, 0.0f, 0.0f, 0.0f);
	itemStyle.hover.fill = Rgba(1.0f, 1.0f, 1.0f, 0.14f);
	itemStyle.pressed.fill = Rgba(1.0f, 1.0f, 1.0f, 0.22f);
	itemStyle.disabled.fill = Rgba(0.0f, 0.0f, 0.0f, 0.0f);

	UITextStyle itemTextStyle = {};
	itemTextStyle.fontSize = 12.0f;
	itemTextStyle.normal.fill = Rgba(0.88f, 0.90f, 0.93f, 1.0f);
	itemTextStyle.hover.fill = Rgba(1.0f, 1.0f, 1.0f, 1.0f);
	itemTextStyle.pressed.fill = itemTextStyle.hover.fill;
	itemTextStyle.disabled.fill = Rgba(0.55f, 0.57f, 0.60f, 1.0f);
	itemTextStyle.hAlign = DWRITE_TEXT_ALIGNMENT_LEADING;
	itemTextStyle.vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

	for (size_t index = 0; index < m_qualityLabels.size(); ++index)
	{
		auto item = std::make_shared<ServiceMenuButton>();
		item->SetFontManager(m_fontManager);
		item->SetStyle(itemStyle);
		item->SetTextStyle(itemTextStyle);
		item->SetText(m_qualityLabels[index].c_str());
		item->SetCheckable(true);
		item->SetChecked(index == m_selectedQuality);
		item->SetCommandId(static_cast<uint32_t>(index));
		item->SetClickCallback(&ServiceControlLayer::OnQualityItemClicked, this);

		Rect2f itemRect = {};
		itemRect.left = 0.0f;
		itemRect.top = 0.0f;
		// 가로 폭은 패널이 다시 잡는다. 높이만 여기서 정한다.
		itemRect.right = kMenuWidth - kMenuPadding * 2.0f;
		itemRect.bottom = kMenuItemHeight;
		item->SetLayout(itemRect);

		if (!item->Initialize(context))
			return false;

		m_qualityMenu->AddChild(item);
		m_qualityItems.push_back(item);
	}

	m_qualityMenu->Hide();
	return true;
}

bool ServiceControlLayer::CreateDeviceResources(IRenderContext* context)
{
	if (!context)
		return false;

	ID2D1DeviceContext* d2dContext = context->GetD2DDeviceContext();
	if (!d2dContext)
		return false;

	SafeRelease(m_barBrush);

	// 영상 위에 얹히므로 불투명하게 깔지 않는다. 화면을 가리는 면적이
	// 곧 사용자가 못 보는 영역이다.
	return SUCCEEDED(d2dContext->CreateSolidColorBrush(
		Rgba(0.0f, 0.0f, 0.0f, 0.58f), &m_barBrush));
}

void ServiceControlLayer::ReleaseDeviceResources()
{
	SafeRelease(m_barBrush);
}

void ServiceControlLayer::UpdateLayout()
{
	if (m_viewWidth <= 0.0f || m_viewHeight <= 0.0f)
		return;

	const float available = m_viewWidth - kBarMargin * 2.0f;
	const float barWidth = (available < kMaxBarWidth) ? available : kMaxBarWidth;
	if (barWidth < kMinBarWidth)
	{
		// 창이 너무 좁다. 이번 프레임은 그리지 않는다.
		m_barRect = {};
		return;
	}

	m_barRect.left = (m_viewWidth - barWidth) * 0.5f;
	m_barRect.right = m_barRect.left + barWidth;
	m_barRect.bottom = m_viewHeight - kBarMargin;
	m_barRect.top = m_barRect.bottom - kBarHeight;

	const float centerY = (m_barRect.top + m_barRect.bottom) * 0.5f;
	const float rowTop = centerY - kButtonSize * 0.5f;
	const float rowBottom = centerY + kButtonSize * 0.5f;

	auto placeRow = [rowTop, rowBottom](float left, float width)
	{
		Rect2f rect = {};
		rect.left = left;
		rect.top = rowTop;
		rect.right = left + width;
		rect.bottom = rowBottom;
		return rect;
	};

	// --- 왼쪽부터: 재생/일시정지, 정지 ---
	float cursorLeft = m_barRect.left + kBarPadding;

	if (m_playPauseButton)
	{
		m_playPauseButton->SetLayout(placeRow(cursorLeft, kButtonSize));
		cursorLeft += kButtonSize + kGap * 0.75f;
	}

	if (m_stopButton)
	{
		m_stopButton->SetLayout(placeRow(cursorLeft, kButtonSize));
		cursorLeft += kButtonSize + kGap;
	}

	// --- 오른쪽부터: 볼륨, 화질 ---
	float cursorRight = m_barRect.right - kBarPadding;

	if (m_volumeSlider)
	{
		m_volumeSlider->SetLayout(placeRow(cursorRight - kVolumeWidth, kVolumeWidth));
		m_volumeSlider->SetVisible(true);
		cursorRight -= kVolumeWidth + kGap;
	}

	const bool hasQualityMenu = (m_qualityLabels.size() > 1);
	if (m_qualityButton)
	{
		m_qualityButton->SetVisible(hasQualityMenu);
		if (hasQualityMenu)
		{
			m_qualityButton->SetLayout(placeRow(cursorRight - kQualityWidth, kQualityWidth));
			cursorRight -= kQualityWidth + kGap;
		}
	}

	// --- 가운데 남은 자리: 지연 바 + 라벨 ---
	const float middleWidth = cursorRight - cursorLeft;
	const bool hasLatencyRoom = (middleWidth >= kLatencyBarMinWidth + kLatencyLabelWidth + kGap);

	if (m_latencyBar && m_latencyLabel)
	{
		m_latencyBar->SetVisible(hasLatencyRoom);
		m_latencyLabel->SetVisible(hasLatencyRoom);

		if (hasLatencyRoom)
		{
			const float barW = middleWidth - kLatencyLabelWidth - kGap;
			m_latencyBar->SetLayout(placeRow(cursorLeft, barW));
			m_latencyLabel->SetLayout(placeRow(cursorLeft + barW + kGap, kLatencyLabelWidth));
		}
	}

	// 메뉴는 화질 버튼 위로 올린다. 아래로 열면 창 밖으로 나간다.
	if (m_qualityMenu && hasQualityMenu && m_qualityButton)
	{
		const auto& anchor = m_qualityButton->GetLayout();
		// UIContextMenuPanel::CalculateContentHeight 와 같은 식이다.
		// 어긋나면 앵커로 잡은 위치와 실제 메뉴가 어긋난다.
		const float itemCount = static_cast<float>(m_qualityItems.size());
		const float menuHeight = kMenuPadding * 2.0f + kMenuItemHeight * itemCount
			+ (itemCount > 1.0f ? (itemCount - 1.0f) * kMenuSpacing : 0.0f);

		Rect2f menuRect = {};
		menuRect.left = anchor.left;
		menuRect.right = anchor.left + kMenuWidth;
		menuRect.bottom = m_barRect.top - 6.0f;
		menuRect.top = menuRect.bottom - menuHeight;

		// 오른쪽으로 삐져나가면 창 안으로 당긴다.
		if (menuRect.right > m_viewWidth - kBarMargin)
		{
			const float shift = menuRect.right - (m_viewWidth - kBarMargin);
			menuRect.left -= shift;
			menuRect.right -= shift;
		}

		m_qualityMenu->SetLayout(menuRect);
	}
}

void ServiceControlLayer::ApplyPlaybackState()
{
	if (!m_playPauseButton)
		return;

	// 재생 중이면 다음 동작은 일시정지다. 버튼은 "지금 상태" 가 아니라
	// "누르면 일어날 일" 을 보여준다.
	if (m_playbackState == StreamingPlaybackState::Playing)
	{
		m_playPauseButton->SetGlyph(ServiceGlyph::Pause);
		m_playPauseButton->SetCommandId(static_cast<uint32_t>(StreamingViewerCommand::Pause));
	}
	else
	{
		m_playPauseButton->SetGlyph(ServiceGlyph::Play);
		m_playPauseButton->SetCommandId(static_cast<uint32_t>(StreamingViewerCommand::Play));
	}
}

void ServiceControlLayer::ApplyLatency()
{
	if (m_latencyBar)
	{
		m_latencyBar->SetValue(m_latency);
	}

	if (m_latencyLabel)
	{
		wchar_t text[32] = {};
		::swprintf_s(text, L"%.0f ms", m_latency);
		m_latencyLabel->SetText(text);
	}
}

void ServiceControlLayer::ApplyQualityLabel()
{
	if (!m_qualityButton)
		return;

	if (m_selectedQuality < m_qualityLabels.size())
	{
		m_qualityButton->SetText(m_qualityLabels[m_selectedQuality].c_str());
	}
	else
	{
		m_qualityButton->SetText(L"--");
	}
}

bool ServiceControlLayer::AdvanceFade(float deltaSeconds)
{
	const float target = (!m_autoHide || m_idleSeconds < m_autoHideDelay) ? 1.0f : 0.0f;
	if (m_alpha == target)
		return false;

	// 선형으로 간다. 0.18 초짜리 페이드에 이징을 넣어도 눈에 보이지 않는다.
	const float step = (kFadeSeconds > 0.0f) ? (deltaSeconds / kFadeSeconds) : 1.0f;

	if (target > m_alpha)
	{
		m_alpha = (std::min)(1.0f, m_alpha + step);
	}
	else
	{
		m_alpha = (std::max)(0.0f, m_alpha - step);
	}

	return true;
}

void ServiceControlLayer::NotifyUserActivity()
{
	const bool wasHidden = (m_alpha < 1.0f);

	m_idleSeconds = 0.0f;

	if (wasHidden)
	{
		RequestFrame();
	}
}

void ServiceControlLayer::RequestFrame()
{
	if (m_frameRequestCallback)
	{
		m_frameRequestCallback(m_frameRequestUserData);
	}
}

bool ServiceControlLayer::IsQualityMenuOpen() const
{
	return m_qualityMenu && m_qualityMenu->IsVisible();
}

void ServiceControlLayer::OpenQualityMenu()
{
	if (!m_qualityMenu || m_qualityItems.empty())
		return;

	const auto& menuRect = m_qualityMenu->GetLayout();
	m_qualityMenu->Show(menuRect.left, menuRect.top);

	RequestFrame();
}

void ServiceControlLayer::CloseQualityMenu()
{
	if (m_qualityMenu && m_qualityMenu->IsVisible())
	{
		m_qualityMenu->Hide();
	}
}

void ServiceControlLayer::OnGlyphButtonClicked(uint32_t commandId, void* userData)
{
	ServiceControlLayer* self = static_cast<ServiceControlLayer*>(userData);
	if (self)
	{
		self->InvokeCommand(static_cast<StreamingViewerCommand>(commandId));
	}
}

void ServiceControlLayer::OnQualityButtonClicked(uint32_t, void* userData)
{
	ServiceControlLayer* self = static_cast<ServiceControlLayer*>(userData);
	if (!self)
		return;

	if (self->IsQualityMenuOpen())
	{
		self->CloseQualityMenu();
	}
	else
	{
		self->OpenQualityMenu();
	}
}

void ServiceControlLayer::OnQualityItemClicked(uint32_t index, void* userData)
{
	ServiceControlLayer* self = static_cast<ServiceControlLayer*>(userData);
	if (!self)
		return;

	self->CloseQualityMenu();
	self->SetSelectedQuality(index);

	if (self->m_qualityCallback)
	{
		self->m_qualityCallback(index, self->m_qualityUserData);
	}

	self->RequestFrame();
}

void ServiceControlLayer::OnVolumeChanged(float value, void* userData)
{
	ServiceControlLayer* self = static_cast<ServiceControlLayer*>(userData);
	if (self && self->m_volumeCallback)
	{
		self->m_volumeCallback(value, self->m_volumeUserData);
	}
}

void ServiceControlLayer::InvokeCommand(StreamingViewerCommand command)
{
	if (m_commandCallback)
	{
		m_commandCallback(command, m_commandUserData);
	}
}
