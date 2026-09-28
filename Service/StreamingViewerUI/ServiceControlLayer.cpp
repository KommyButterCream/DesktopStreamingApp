#include "ServiceControlLayer.h"

#include "../../../../Module/D3D11EngineInterface/IRenderContext.h"
#include "../../../../Module/D3D11EngineInterface/IRenderEngine.h"
#include "../../../../Module/D3D11UIFramework/D3D11UIFramework/Slider/UISlider.h"
#include "../../../../Module/D3D11UIFramework/D3D11UIFramework/Label/UILabel.h"
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

	// "1440p60 · 18.4 Mbps" 가 잘리지 않을 만큼.
	constexpr float kQualityWidth = 150.0f;

	// 오버레이 토글이 쓰는 명령 값. StreamingViewerCommand 와 겹치지
	// 않게 띄운다 — 두 콜백이 같은 함수 시그니처를 쓴다.
	constexpr uint32_t kStatsButtonCommandId = 2000;
	constexpr float kLatencyLabelWidth = 62.0f;
	constexpr float kLatencyBarMinWidth = 60.0f;

	// 바가 차지하는 폭의 상한. 창이 아주 넓어도 컨트롤이 끝까지 늘어나면
	// 시선이 양쪽 끝으로 갈라진다.
	constexpr float kMaxBarWidth = 720.0f;

	// 이보다 좁으면 오른쪽 것부터 접는다.
	constexpr float kMinBarWidth = 240.0f;

	// 페이드가 완전히 열리고 닫히는 데 걸리는 시간.
	constexpr float kFadeSeconds = 0.18f;

	D2D1_COLOR_F Rgba(float r, float g, float b, float a)
	{
		return D2D1::ColorF(r, g, b, a);
	}

	bool SameQualityInfo(const StreamingQualityInfo& lhs, const StreamingQualityInfo& rhs)
	{
		return lhs.width == rhs.width
			&& lhs.height == rhs.height
			&& lhs.fps == rhs.fps
			&& lhs.bitrateMbps == rhs.bitrateMbps;
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

	// 텍스트를 쓰는 요소(지연 라벨, 화질 라벨)가 폰트 매니저를 요구한다.
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
	ApplyQualityInfo();

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
	m_statsButton.reset();
	m_qualityLabel.reset();
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

	if (m_qualityLabel)
		m_qualityLabel->Prepare();

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

	m_idleSeconds += deltaSeconds;

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
		const D2D1_RECT_F bounds = D2D1::RectF(
			m_barRect.left, m_barRect.top, m_barRect.right, m_barRect.bottom);

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
	if (m_qualityLabel)    m_qualityLabel->Render();
	if (m_statsButton)     m_statsButton->Render();

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

	return (x >= m_barRect.left) && (x <= m_barRect.right)
		&& (y >= m_barRect.top) && (y <= m_barRect.bottom);
}

bool ServiceControlLayer::OnMouseEvent(UIMouseEventType type, float x, float y)
{
	if (!m_visible)
		return false;

	// 창을 벗어났다. 모든 자식의 hover 를 푼다. 바 밖이라고 건너뛰면
	// 하이라이트가 고착된다.
	if (type == UIMouseEventType::Leave)
	{
		if (m_playPauseButton) m_playPauseButton->OnMouseEvent(type, x, y);
		if (m_stopButton)      m_stopButton->OnMouseEvent(type, x, y);
		if (m_statsButton)     m_statsButton->OnMouseEvent(type, x, y);
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

	const bool insideBar = (x >= m_barRect.left) && (x <= m_barRect.right)
		&& (y >= m_barRect.top) && (y <= m_barRect.bottom);

	if (!insideBar)
	{
		// 바 밖이다. 자식들의 hover 만 풀어 주고 이벤트는 넘긴다 —
		// 여기서 소비하면 영상 위 팬/줌이 죽는다.
		if (m_playPauseButton) m_playPauseButton->OnMouseEvent(UIMouseEventType::Move, -1.0f, -1.0f);
		if (m_stopButton)      m_stopButton->OnMouseEvent(UIMouseEventType::Move, -1.0f, -1.0f);
		if (m_statsButton)     m_statsButton->OnMouseEvent(UIMouseEventType::Move, -1.0f, -1.0f);
		return false;
	}

	NotifyUserActivity();

	if (m_playPauseButton && m_playPauseButton->OnMouseEvent(type, x, y))
	{
		// 소비됨
	}
	else if (m_stopButton && m_stopButton->OnMouseEvent(type, x, y))
	{
		// 소비됨
	}
	else if (m_statsButton)
	{
		m_statsButton->OnMouseEvent(type, x, y);
	}

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

	UpdateLayout();
}

void ServiceControlLayer::OnDeviceLost()
{
	ReleaseDeviceResources();

	if (m_playPauseButton) m_playPauseButton->DiscardDeviceResources();
	if (m_stopButton)      m_stopButton->DiscardDeviceResources();
	if (m_latencyBar)      m_latencyBar->DiscardDeviceResources();
	if (m_latencyLabel)    m_latencyLabel->DiscardDeviceResources();
	if (m_qualityLabel)    m_qualityLabel->DiscardDeviceResources();
	if (m_statsButton)     m_statsButton->DiscardDeviceResources();
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
	if (m_qualityLabel)    m_qualityLabel->RestoreDeviceResources(m_context);
	if (m_statsButton)     m_statsButton->RestoreDeviceResources(m_context);

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

void ServiceControlLayer::SetLatency(float milliseconds)
{
	// 음수는 그대로 둔다. "모른다" 는 뜻이라 0 으로 접으면 안 된다.
	m_latency = milliseconds;
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

void ServiceControlLayer::SetQualityInfo(const StreamingQualityInfo& info)
{
	// 호스트가 통계 주기마다 불러도 되도록, 바뀌지 않았으면 텍스트를
	// 다시 만들지 않는다. SetText 는 DWrite 레이아웃을 새로 잡는다.
	if (SameQualityInfo(m_qualityInfo, info))
		return;

	m_qualityInfo = info;
	ApplyQualityInfo();
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

	// 두 라벨 모두 여기서 문구를 넣지 않는다. Initialize 가 끝난 뒤
	// UpdateLayout → ApplyLatency / ApplyQualityInfo 순으로 넣는다.
	//
	// UIElementBase::SetLayout 은 텍스트 레이아웃을 무효화하지 않는다.
	// 레이아웃 전에 넣은 문구는 0 크기로 레이아웃을 시도했다가 실패하고
	// 더티 플래그까지 지워진다. 그 뒤 같은 문구를 다시 넣으면 SetText 가
	// 바뀐 게 없다며 걸러서, 화질 라벨은 스트림 정보가 오기 전까지
	// 비어 있었다("--" → "--").
	m_latencyLabel = std::make_unique<UILabel>();
	m_latencyLabel->SetFontManager(m_fontManager);
	m_latencyLabel->SetTextStyle(textStyle);
	if (!m_latencyLabel->Initialize(context))
		return false;

	// 화질은 바의 오른쪽 끝에 붙으므로 오른쪽 정렬이다. 왼쪽 정렬로
	// 두면 값의 자릿수가 바뀔 때마다 끝이 들쭉날쭉해진다.
	UITextStyle qualityTextStyle = textStyle;
	qualityTextStyle.hAlign = DWRITE_TEXT_ALIGNMENT_TRAILING;
	qualityTextStyle.normal.fill = Rgba(0.72f, 0.76f, 0.82f, 1.0f);
	qualityTextStyle.hover.fill = qualityTextStyle.normal.fill;
	qualityTextStyle.pressed.fill = qualityTextStyle.normal.fill;

	m_qualityLabel = std::make_unique<UILabel>();
	m_qualityLabel->SetFontManager(m_fontManager);
	m_qualityLabel->SetTextStyle(qualityTextStyle);
	if (!m_qualityLabel->Initialize(context))
		return false;

	// 진단 오버레이 토글. 바의 맨 오른쪽 끝이다.
	m_statsButton = std::make_unique<ServiceGlyphButton>();
	m_statsButton->SetStyle(buttonStyle);
	m_statsButton->SetGlyph(ServiceGlyph::Stats);
	m_statsButton->SetCommandId(kStatsButtonCommandId);
	m_statsButton->SetClickCallback(&ServiceControlLayer::OnStatsButtonClicked, this);
	if (!m_statsButton->Initialize(context))
		return false;

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

	// --- 오른쪽부터: 오버레이 토글, 화질 표시 ---
	float cursorRight = m_barRect.right - kBarPadding;

	if (m_statsButton)
	{
		m_statsButton->SetLayout(placeRow(cursorRight - kButtonSize, kButtonSize));
		cursorRight -= kButtonSize + kGap;
	}

	if (m_qualityLabel)
	{
		m_qualityLabel->SetLayout(placeRow(cursorRight - kQualityWidth, kQualityWidth));
		cursorRight -= kQualityWidth + kGap;
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
	const bool known = (m_latency >= 0.0f);

	if (m_latencyBar)
	{
		m_latencyBar->SetValue(known ? m_latency : 0.0f);
	}

	if (m_latencyLabel)
	{
		if (known)
		{
			wchar_t text[32] = {};
			::swprintf_s(text, L"%.0f ms", m_latency);
			m_latencyLabel->SetText(text);
		}
		else
		{
			m_latencyLabel->SetText(L"--");
		}
	}
}

void ServiceControlLayer::ApplyQualityInfo()
{
	if (!m_qualityLabel)
		return;

	// 해상도를 모르면 나머지도 의미가 없다. 아직 스트림 정보를 못 받은
	// 상태이므로 "--" 하나로 둔다.
	if (m_qualityInfo.width == 0 || m_qualityInfo.height == 0)
	{
		m_qualityLabel->SetText(L"--");
		return;
	}

	wchar_t text[64] = {};
	int written = 0;

	// 1440p / 1080p 식 표기. 세로 픽셀로 쓰는 것이 이 바닥의 관행이고,
	// 가로까지 적으면 바에서 차지하는 폭이 두 배가 된다.
	written = ::swprintf_s(text, L"%up", m_qualityInfo.height);
	if (written < 0)
		return;

	// 60fps 와 30fps 는 체감이 전혀 다르다. 해상도에 붙여 쓴다.
	if (m_qualityInfo.fps > 0)
	{
		const int added = ::swprintf_s(text + written, _countof(text) - written,
			L"%u", m_qualityInfo.fps);
		if (added > 0)
			written += added;
	}

	// 비트레이트는 서버가 혼잡에 따라 계속 움직이는 값이다. 이 바에서
	// 실제로 변하는 것이 이것뿐이라, 붙여 두면 적응 동작이 눈에 보인다.
	if (m_qualityInfo.bitrateMbps > 0.0f)
	{
		::swprintf_s(text + written, _countof(text) - written,
			L" · %.1f Mbps", m_qualityInfo.bitrateMbps);
	}

	m_qualityLabel->SetText(text);
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

void ServiceControlLayer::OnGlyphButtonClicked(uint32_t commandId, void* userData)
{
	ServiceControlLayer* self = static_cast<ServiceControlLayer*>(userData);
	if (self)
	{
		self->InvokeCommand(static_cast<StreamingViewerCommand>(commandId));
	}
}

void ServiceControlLayer::OnStatsButtonClicked(uint32_t, void* userData)
{
	ServiceControlLayer* self = static_cast<ServiceControlLayer*>(userData);
	if (self && self->m_statsToggleCallback)
	{
		// 켜짐 표시는 여기서 뒤집지 않는다. 실제로 켜졌는지 아는 것은
		// 파사드이고, 그쪽이 SetStatsActive 로 돌려준다. 재생 버튼이
		// 스스로 상태를 바꾸지 않는 것과 같은 이유다.
		self->m_statsToggleCallback(self->m_statsToggleUserData);
	}
}

void ServiceControlLayer::SetStatsToggleCallback(FrameRequestCallback callback, void* userData)
{
	m_statsToggleCallback = callback;
	m_statsToggleUserData = userData;
}

void ServiceControlLayer::SetStatsActive(bool active)
{
	if (m_statsButton)
	{
		m_statsButton->SetActive(active);
	}

	RequestFrame();
}

void ServiceControlLayer::InvokeCommand(StreamingViewerCommand command)
{
	if (m_commandCallback)
	{
		m_commandCallback(command, m_commandUserData);
	}
}
