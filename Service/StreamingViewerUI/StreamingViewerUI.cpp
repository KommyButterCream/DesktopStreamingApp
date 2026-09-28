#include "StreamingViewerUI.h"
#include "ServiceControlLayer.h"
#include "ServiceStatsLayer.h"

#include "../../../../Module/D3D11ImageView/D3D11ImageView/D3D11ImageView.h"

#include <new>

StreamingViewerUI::StreamingViewerUI() = default;

StreamingViewerUI::~StreamingViewerUI()
{
	Detach();
}

bool StreamingViewerUI::Attach(D3D11ImageView* viewer)
{
	if (!viewer)
		return false;

	// 이미 붙어 있으면 거절한다. 조용히 갈아끼우면 이전 레이어가 등록된
	// 채로 남아 뷰어가 죽은 객체를 부른다.
	if (m_viewer)
		return false;

	ServiceControlLayer* layer = new (std::nothrow) ServiceControlLayer();
	if (!layer)
		return false;

	// 뷰어가 Initialize 를 불러 준다. 그 안에서 레이어가 D2D 리소스를
	// 만들고 리스너로 등록한다.
	if (!viewer->AddRenderLayer(static_cast<IUIRenderLayer*>(layer), RenderLayerSlot::Topmost))
	{
		delete layer;
		return false;
	}

	// 통계 패널은 별도 레이어다. 컨트롤 바의 자식으로 두면 바가 자동
	// 숨김으로 사라질 때 통계도 같이 사라지는데, 한참 들여다봐야 하는
	// 값들이라 그러면 쓸 수가 없다.
	//
	// 실패해도 컨트롤 바는 살린다. 진단 패널이 없다고 재생까지 막을
	// 이유는 없다.
	ServiceStatsLayer* statsLayer = new (std::nothrow) ServiceStatsLayer();
	if (statsLayer)
	{
		if (viewer->AddRenderLayer(statsLayer, RenderLayerSlot::Topmost))
		{
			m_statsLayer = statsLayer;
		}
		else
		{
			delete statsLayer;
		}
	}

	m_viewer = viewer;
	m_layer = layer;

	ApplyPendingSettings();
	return true;
}

void StreamingViewerUI::Detach()
{
	if (!m_viewer)
	{
		// 붙인 적이 없어도 레이어만 남아 있을 수는 없다. 그래도 방어한다.
		delete m_statsLayer;
		delete m_layer;
		m_statsLayer = nullptr;
		m_layer = nullptr;
		return;
	}

	// 뷰어가 렌더 락 안에서 떼어내고 Shutdown 을 불러 준다.
	// 반환한 뒤에는 렌더 스레드가 이 레이어를 부르지 않는다.
	if (m_statsLayer)
	{
		m_viewer->RemoveRenderLayer(m_statsLayer);
		delete m_statsLayer;
		m_statsLayer = nullptr;
	}

	if (m_layer)
	{
		m_viewer->RemoveRenderLayer(static_cast<IUIRenderLayer*>(m_layer));
		delete m_layer;
		m_layer = nullptr;
	}

	m_viewer = nullptr;
}

bool StreamingViewerUI::IsAttached() const
{
	return m_layer != nullptr;
}

// Attach 전에 들어온 설정을 한 번에 밀어 넣는다.
//
// 설정마다 "레이어가 있으면 넘기고 없으면 저장" 을 흩어 두면 새 설정을
// 추가할 때마다 두 자리를 고쳐야 한다. 저장은 항상 하고, 반영은 여기
// 한 곳에서 한다.
void StreamingViewerUI::ApplyPendingSettings()
{
	if (m_layer)
	{
		m_layer->SetFrameRequestCallback(&StreamingViewerUI::RequestFrame, this);
		m_layer->SetStatsToggleCallback(&StreamingViewerUI::ToggleStats, this);

		m_layer->SetCommandCallback(m_commandCallback, m_commandUserData);

		m_layer->SetAutoHideDelay(m_autoHideDelay);
		m_layer->SetAutoHide(m_autoHide);

		m_layer->SetPlaybackState(m_playbackState);
		m_layer->SetLatencyRange(m_latencyRange);
		m_layer->SetLatency(m_latency);
		m_layer->SetQualityInfo(m_qualityInfo);
		m_layer->SetVisible(m_visible);
		m_layer->SetStatsActive(m_statsVisible);
	}

	if (m_statsLayer)
	{
		m_statsLayer->SetQualityInfo(m_qualityInfo);
		m_statsLayer->SetStatsInfo(m_statsInfo);
		m_statsLayer->SetVisible(m_statsVisible);
	}
}

void StreamingViewerUI::ToggleStats(void* userData)
{
	StreamingViewerUI* self = static_cast<StreamingViewerUI*>(userData);
	if (self)
	{
		self->SetStatsVisible(!self->m_statsVisible);
	}
}

void StreamingViewerUI::RequestFrame(void* userData)
{
	StreamingViewerUI* self = static_cast<StreamingViewerUI*>(userData);
	if (self && self->m_viewer)
	{
		self->m_viewer->InvalidateFrame();
	}
}

void StreamingViewerUI::SetCommandCallback(CommandCallback callback, void* userData)
{
	m_commandCallback = callback;
	m_commandUserData = userData;

	if (m_layer)
	{
		m_layer->SetCommandCallback(callback, userData);
	}
}

void StreamingViewerUI::SetPlaybackState(StreamingPlaybackState state)
{
	m_playbackState = state;

	if (m_layer)
	{
		m_layer->SetPlaybackState(state);
	}

	if (m_viewer)
	{
		m_viewer->InvalidateFrame();
	}
}

StreamingPlaybackState StreamingViewerUI::GetPlaybackState() const
{
	return m_playbackState;
}

void StreamingViewerUI::SetLatency(float milliseconds)
{
	m_latency = milliseconds;

	if (m_layer)
	{
		m_layer->SetLatency(milliseconds);
	}

	// 지연은 초당 몇 번씩 갱신된다. 매번 프레임을 강제하지 않는다 —
	// 스트리밍 중에는 어차피 매 프레임 그려지고, 정지 화면에서는
	// 다음 사용자 입력 때 반영되면 충분하다.
}

void StreamingViewerUI::SetLatencyRange(float maxMilliseconds)
{
	m_latencyRange = maxMilliseconds;

	if (m_layer)
	{
		m_layer->SetLatencyRange(maxMilliseconds);
	}
}

float StreamingViewerUI::GetLatency() const
{
	return m_latency;
}

void StreamingViewerUI::SetQualityInfo(const StreamingQualityInfo& info)
{
	m_qualityInfo = info;

	if (m_layer)
	{
		m_layer->SetQualityInfo(info);
	}

	if (m_statsLayer)
	{
		m_statsLayer->SetQualityInfo(info);
	}

	// 지연과 같은 이유로 프레임을 강제하지 않는다. 해상도가 바뀌면
	// 새 프레임이 뒤따라 오므로 그때 함께 반영된다.
}

void StreamingViewerUI::SetStatsInfo(const StreamingStatsInfo& stats)
{
	m_statsInfo = stats;

	if (m_statsLayer)
	{
		m_statsLayer->SetStatsInfo(stats);
	}

	// 보이는 동안에는 프레임을 요청한다. 정지 화면에서 오버레이를 켜 두면
	// 새 프레임이 오지 않아 숫자가 멈춘 것처럼 보이는데, 그때가 바로
	// 무엇이 멈췄는지 보려고 켜 둔 때다.
	if (m_statsVisible && m_viewer)
	{
		m_viewer->InvalidateFrame();
	}
}

void StreamingViewerUI::SetStatsVisible(bool visible)
{
	m_statsVisible = visible;

	if (m_statsLayer)
	{
		m_statsLayer->SetVisible(visible);
	}

	if (m_layer)
	{
		m_layer->SetStatsActive(visible);
	}

	if (m_viewer)
	{
		m_viewer->InvalidateFrame();
	}
}

bool StreamingViewerUI::IsStatsVisible() const
{
	return m_statsVisible;
}

void StreamingViewerUI::SetAutoHide(bool enabled)
{
	m_autoHide = enabled;

	if (m_layer)
	{
		m_layer->SetAutoHide(enabled);
	}
}

bool StreamingViewerUI::IsAutoHideEnabled() const
{
	return m_autoHide;
}

void StreamingViewerUI::SetAutoHideDelay(float seconds)
{
	m_autoHideDelay = seconds;

	if (m_layer)
	{
		m_layer->SetAutoHideDelay(seconds);
	}
}

void StreamingViewerUI::SetVisible(bool visible)
{
	m_visible = visible;

	if (m_layer)
	{
		m_layer->SetVisible(visible);
	}

	if (m_viewer)
	{
		m_viewer->InvalidateFrame();
	}
}

bool StreamingViewerUI::IsVisible() const
{
	return m_visible;
}
