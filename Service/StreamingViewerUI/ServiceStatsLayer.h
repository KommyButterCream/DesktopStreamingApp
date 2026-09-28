#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <d2d1_3.h>

#include <memory>
#include <string>
#include <vector>

#include "../../../../Module/Core/ShapeType/Rect2f.h"
#include "../../../../Module/D3D11EngineInterface/IRenderLayer.h"
#include "../../../../Module/D3D11EngineInterface/IDeviceEventListener.h"
#include "../../../../Module/D3D11EngineInterface/IResizeEventListener.h"

#include "StreamingViewerUI.h"

class IRenderContext;
class UILabel;
class FontManager;

struct ID2D1SolidColorBrush;
struct ID2D1PathGeometry;

// 진단용 통계 패널.
//
// 화면 왼쪽 위에 고정으로 붙는다. 입력을 받지 않으므로 IUIRenderLayer 가
// 아니라 IRenderLayer 다 — 히트 테스트를 구현하면 패널 아래의 영상이
// 클릭을 못 받게 되고, 이 패널은 누를 것이 없다.
//
// 컨트롤 바와 별도 레이어인 이유는 자동 숨김 때문이다. 바의 자식으로
// 두면 바가 사라질 때 통계도 같이 사라지는데, 한참 들여다봐야 하는
// 값들이라 그러면 쓸 수가 없다.
//
// 역할
//   "끊긴다" 는 증상 하나에 원인이 다섯이다. 어느 카운터가 움직이는지가
//   곧 어느 단계가 막혔는지다. 그래서 값을 나열하는 것이 아니라 0 인
//   줄은 죽이고 0 이 아닌 줄만 살려서, 눈이 갈 곳을 한 줄로 줄인다.
//
//   수신량과 지연은 숫자보다 모양이 중요해서 스파크라인을 함께 그린다.
//   서버의 적응형 비트레이트가 내려갔다 올라오는 것은 값이 아니라
//   곡선이다.
class ServiceStatsLayer
	: public IRenderLayer
	, public IResizeEventListener
	, public IDeviceEventListener
{
public:
	ServiceStatsLayer();
	virtual ~ServiceStatsLayer();

	ServiceStatsLayer(const ServiceStatsLayer&) = delete;
	ServiceStatsLayer& operator=(const ServiceStatsLayer&) = delete;

public:
	// IRenderLayer Override
	bool Initialize(IRenderContext* context) override;
	void Shutdown() override;
	bool Prepare() override;
	bool Render() override;

	// IResizeEventListener Override
	void OnResize(uint32_t width, uint32_t height) override;

	// IDeviceEventListener Override
	void OnDeviceLost() override;
	void OnDeviceRestored() override;

public:
	void SetVisible(bool visible);
	bool IsVisible() const;

	// 호스트가 주기적으로 넣어 준다. 이 주기가 곧 스파크라인의
	// 표본 간격이다 — 500ms 로 넣으면 60칸이 30초 창이 된다.
	void SetStatsInfo(const StreamingStatsInfo& stats);

	// 해상도 / fps 는 화질 표시와 같은 값을 쓴다. 두 번 받지 않는다.
	void SetQualityInfo(const StreamingQualityInfo& quality);

private:
	// 값 한 줄. 라벨과 값을 따로 둔 이유는 값만 오른쪽 정렬해야
	// 자릿수가 바뀌어도 끝이 흔들리지 않기 때문이다.
	struct StatsRow
	{
		std::unique_ptr<UILabel> caption;
		std::unique_ptr<UILabel> value;

		// 캡션 문구. 레이아웃이 잡힌 뒤에야 넣을 수 있어서 들고 있는다.
		// (ApplyStaticText 주석 참고)
		std::wstring captionText;

		// 마지막으로 넣은 문자열. 같으면 SetText 를 건너뛴다 —
		// DWrite 레이아웃을 다시 잡을 이유가 없다.
		std::wstring lastValue;

		// 손실 줄인가. 손실 줄만 "0 이면 흐리게, 아니면 강조" 규칙을 따른다.
		// 나머지(수신량, 지연, resync 등)는 늘 보통 밝기다 — 그 값들은
		// 0 이 아닌 것이 정상이라 같은 규칙을 쓰면 화면 전체가 흐려지거나
		// 전체가 주황이 된다.
		bool lossRow = false;

		// 0 이 아닌 값이라 강조 중인가. 색을 바꿀 때만 스타일을 다시 넣는다.
		bool highlighted = false;
	};

	// 시계열 한 칸. 값과 함께 그릴 색을 들고 있다.
	struct Sparkline
	{
		std::vector<float> samples;
		size_t nextIndex = 0;
		bool filled = false;

		float lastMax = 0.0f;
		ID2D1PathGeometry* geometry = nullptr;
		bool dirty = true;

		D2D1_COLOR_F color = {};
		Core::ShapeType::Rect2f bounds = {};
	};

	bool CreateChildren(IRenderContext* context);
	bool CreateDeviceResources(IRenderContext* context);
	void ReleaseDeviceResources();

	bool CreateRow(IRenderContext* context, StatsRow& row, const wchar_t* caption, bool lossRow);
	void SetRowValue(StatsRow& row, const wchar_t* text, bool highlight);

	void PushSample(Sparkline& line, float value);
	void RebuildSparkline(Sparkline& line);
	void RenderSparkline(ID2D1DeviceContext* d2dContext, Sparkline& line);
	void ReleaseSparklineGeometry();

	void UpdateLayout();

	// 바뀌지 않는 문구(구역 제목, 행 캡션)를 넣는다.
	//
	// 반드시 UpdateLayout 뒤에 불러야 한다. UIElementBase::SetLayout 은
	// 값만 대입하고 OnLayoutChanged 를 부르지 않는다. 그래서 텍스트
	// 레이아웃이 만들어지는 자리는 Initialize 안(그때 사각형은 0)과
	// OnLayoutChanged 둘뿐이고, 레이아웃 전에 넣은 문자열은 0 크기로
	// 레이아웃을 시도했다가 실패한 뒤 더티 플래그까지 지워져서 영영
	// 그려지지 않는다. 같은 문자열을 다시 넣어도 SetText 가 걸러내므로
	// 회복되지도 않는다.
	//
	// 패널은 화면 왼쪽 위에 고정 크기로 붙으므로 창 크기가 바뀌어도
	// 자식 사각형이 달라지지 않는다. 그래서 한 번만 부르면 된다.
	void ApplyStaticText();

	void ApplyStats();

private:
	IRenderContext* m_context = nullptr;
	FontManager* m_fontManager = nullptr;
	bool m_visible = false;

	float m_viewWidth = 0.0f;
	float m_viewHeight = 0.0f;

	Core::ShapeType::Rect2f m_panelRect = {};
	ID2D1SolidColorBrush* m_panelBrush = nullptr;
	ID2D1SolidColorBrush* m_lineBrush = nullptr;

	std::unique_ptr<UILabel> m_headerLabel = nullptr;
	std::unique_ptr<UILabel> m_streamSection = nullptr;
	std::unique_ptr<UILabel> m_lossSection = nullptr;
	std::unique_ptr<UILabel> m_paceSection = nullptr;

	StatsRow m_bitrateRow = {};
	StatsRow m_latencyRow = {};
	StatsRow m_fpsRow = {};

	StatsRow m_rejectedRow = {};
	StatsRow m_discardedRow = {};
	StatsRow m_queueDropRow = {};
	StatsRow m_poolDropRow = {};
	StatsRow m_notConsumedRow = {};

	StatsRow m_bufferRow = {};
	StatsRow m_waitRow = {};
	StatsRow m_resyncRow = {};

	Sparkline m_bitrateLine = {};
	Sparkline m_latencyLine = {};

	StreamingStatsInfo m_stats = {};
	StreamingQualityInfo m_quality = {};
};
