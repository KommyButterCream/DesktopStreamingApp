#include "StreamingViewerUI.h"
#include "ServiceControlLayer.h"

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

	m_viewer = viewer;
	m_layer = layer;

	ApplyPendingSettings();
	return true;
}

void StreamingViewerUI::Detach()
{
	if (!m_viewer || !m_layer)
	{
		// 붙인 적이 없어도 레이어만 남아 있을 수는 없다. 그래도 방어한다.
		delete m_layer;
		m_layer = nullptr;
		m_viewer = nullptr;
		return;
	}

	// 뷰어가 렌더 락 안에서 떼어내고 Shutdown 을 불러 준다.
	// 반환한 뒤에는 렌더 스레드가 이 레이어를 부르지 않는다.
	m_viewer->RemoveRenderLayer(static_cast<IUIRenderLayer*>(m_layer));

	delete m_layer;
	m_layer = nullptr;
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
	if (!m_layer)
		return;

	m_layer->SetFrameRequestCallback(&StreamingViewerUI::RequestFrame, this);

	m_layer->SetCommandCallback(m_commandCallback, m_commandUserData);
	m_layer->SetVolumeCallback(m_volumeCallback, m_volumeUserData);
	m_layer->SetQualityCallback(m_qualityCallback, m_qualityUserData);

	m_layer->SetAutoHideDelay(m_autoHideDelay);
	m_layer->SetAutoHide(m_autoHide);

	m_layer->SetPlaybackState(m_playbackState);
	m_layer->SetVolume(m_volume);
	m_layer->SetLatencyRange(m_latencyRange);
	m_layer->SetLatency(m_latency);
	m_layer->SetSelectedQuality(m_selectedQuality);
	m_layer->SetVisible(m_visible);
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

void StreamingViewerUI::SetVolumeCallback(VolumeCallback callback, void* userData)
{
	m_volumeCallback = callback;
	m_volumeUserData = userData;

	if (m_layer)
	{
		m_layer->SetVolumeCallback(callback, userData);
	}
}

void StreamingViewerUI::SetQualityCallback(QualityCallback callback, void* userData)
{
	m_qualityCallback = callback;
	m_qualityUserData = userData;

	if (m_layer)
	{
		m_layer->SetQualityCallback(callback, userData);
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

void StreamingViewerUI::SetVolume(float volume)
{
	m_volume = volume;

	if (m_layer)
	{
		m_layer->SetVolume(volume);
	}

	if (m_viewer)
	{
		m_viewer->InvalidateFrame();
	}
}

float StreamingViewerUI::GetVolume() const
{
	return m_layer ? m_layer->GetVolume() : m_volume;
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

void StreamingViewerUI::SetQualityOptions(const StreamingQualityOption* options, uint32_t count)
{
	if (m_layer)
	{
		m_layer->SetQualityOptions(options, count);
	}

	if (m_viewer)
	{
		m_viewer->InvalidateFrame();
	}
}

void StreamingViewerUI::SetSelectedQuality(uint32_t index)
{
	m_selectedQuality = index;

	if (m_layer)
	{
		m_layer->SetSelectedQuality(index);
	}

	if (m_viewer)
	{
		m_viewer->InvalidateFrame();
	}
}

uint32_t StreamingViewerUI::GetSelectedQuality() const
{
	return m_layer ? m_layer->GetSelectedQuality() : m_selectedQuality;
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
