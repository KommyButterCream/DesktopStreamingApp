#include "ServiceStatsLayer.h"

#include "../../../../Module/D3D11EngineInterface/IRenderContext.h"
#include "../../../../Module/D3D11EngineInterface/IRenderEngine.h"
#include "../../../../Module/D3D11UIFramework/D3D11UIFramework/Label/UILabel.h"
#include "../../../../Module/Core/ShapeType/Point2f.h"
#include "../../../../Module/Core/DirectX/DxSafeRelease.h"

#include <algorithm>
#include <stdio.h>

using namespace Core::DirectX;
using namespace Core::ShapeType;

namespace
{
	// 패널 치수. 왼쪽 위에 고정이다.
	constexpr float kPanelMargin = 12.0f;
	constexpr float kPanelWidth = 268.0f;
	constexpr float kPanelPadding = 10.0f;

	constexpr float kHeaderHeight = 20.0f;
	constexpr float kSectionHeight = 16.0f;

	// 캡션이 한글(맑은 고딕)이라 줄 높이가 Consolas 보다 크다. UILabel 은
	// D2D1_DRAW_TEXT_OPTIONS_CLIP 으로 그리므로 칸이 줄 높이보다 작으면
	// 글자 위아래가 잘린다.
	constexpr float kRowHeight = 17.0f;
	constexpr float kSparkHeight = 18.0f;
	constexpr float kSectionGap = 7.0f;

	// 값 열의 폭. 오른쪽 정렬이라 자릿수가 바뀌어도 끝이 고정된다.
	constexpr float kValueWidth = 92.0f;

	// 스파크라인 표본 수. 호스트가 500ms 마다 넣으면 30초 창이다.
	constexpr size_t kSampleCount = 60;

	// 스파크라인 세로 눈금의 최소값. 이보다 작은 데이터만 들어와도
	// 눈금을 더 줄이지 않는다 — 0.1 Mbps 짜리 잡음이 화면 가득한
	// 산맥으로 보이면 읽는 사람이 규모를 잘못 읽는다.
	constexpr float kMinBitrateScale = 5.0f;
	constexpr float kMinLatencyScale = 50.0f;

	D2D1_COLOR_F Rgba(float r, float g, float b, float a)
	{
		return D2D1::ColorF(r, g, b, a);
	}

	// 값이 0 인 줄은 죽이고 0 이 아닌 줄만 살린다. 카운터 여덟 개를
	// 같은 밝기로 늘어놓으면 아무도 읽지 않는다.
	const D2D1_COLOR_F kDimColor = D2D1::ColorF(0.46f, 0.49f, 0.54f, 1.0f);
	const D2D1_COLOR_F kNormalColor = D2D1::ColorF(0.86f, 0.89f, 0.93f, 1.0f);
	const D2D1_COLOR_F kAlertColor = D2D1::ColorF(1.0f, 0.62f, 0.35f, 1.0f);
	const D2D1_COLOR_F kCaptionColor = D2D1::ColorF(0.58f, 0.62f, 0.68f, 1.0f);
	const D2D1_COLOR_F kSectionColor = D2D1::ColorF(0.42f, 0.66f, 0.95f, 1.0f);

	// 값 열 스타일. 생성할 때와 색을 바꿀 때 같은 모양을 써야 해서 한 곳에 둔다.
	UITextStyle MakeValueStyle(const D2D1_COLOR_F& color)
	{
		UITextStyle style = {};
		::wcscpy_s(style.fontName, L"Consolas");
		style.fontSize = 11.0f;
		style.hAlign = DWRITE_TEXT_ALIGNMENT_TRAILING;
		style.vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
		style.normal.fill = color;
		style.hover.fill = color;
		style.pressed.fill = color;
		style.disabled.fill = color;
		return style;
	}

	// 손실 줄은 0 이면 흐리게, 아니면 강조. 나머지 줄은 늘 보통 밝기.
	D2D1_COLOR_F ValueColor(bool lossRow, bool highlighted)
	{
		if (!lossRow)
			return kNormalColor;

		return highlighted ? kAlertColor : kDimColor;
	}
}

ServiceStatsLayer::ServiceStatsLayer() = default;

ServiceStatsLayer::~ServiceStatsLayer()
{
	Shutdown();
}

bool ServiceStatsLayer::Initialize(IRenderContext* context)
{
	if (!context)
		return false;

	m_context = context;

	if (IRenderEngine* engine = m_context->GetEngine())
	{
		m_fontManager = engine->GetFontManager();
	}

	m_bitrateLine.samples.assign(kSampleCount, 0.0f);
	m_bitrateLine.color = Rgba(0.40f, 0.80f, 0.55f, 1.0f);

	m_latencyLine.samples.assign(kSampleCount, 0.0f);
	m_latencyLine.color = Rgba(0.42f, 0.70f, 1.0f, 1.0f);

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

	m_context->AddResizeListener(this);
	m_context->AddDeviceListener(this);

	m_viewWidth = static_cast<float>(m_context->GetWidth());
	m_viewHeight = static_cast<float>(m_context->GetHeight());
	UpdateLayout();
	ApplyStaticText();
	ApplyStats();

	return true;
}

void ServiceStatsLayer::Shutdown()
{
	if (m_context)
	{
		m_context->RemoveResizeListener(this);
		m_context->RemoveDeviceListener(this);
	}

	ReleaseSparklineGeometry();

	StatsRow* rows[] = {
		&m_bitrateRow, &m_latencyRow, &m_fpsRow,
		&m_rejectedRow, &m_discardedRow, &m_queueDropRow, &m_poolDropRow, &m_notConsumedRow,
		&m_bufferRow, &m_waitRow, &m_resyncRow,
	};

	for (StatsRow* row : rows)
	{
		row->value.reset();
		row->caption.reset();
	}

	m_paceSection.reset();
	m_lossSection.reset();
	m_streamSection.reset();
	m_headerLabel.reset();

	ReleaseDeviceResources();

	m_context = nullptr;
}

bool ServiceStatsLayer::Prepare()
{
	if (!m_visible)
		return false;

	UILabel* labels[] = {
		m_headerLabel.get(), m_streamSection.get(), m_lossSection.get(), m_paceSection.get(),
	};

	for (UILabel* label : labels)
	{
		if (label)
			label->Prepare();
	}

	StatsRow* rows[] = {
		&m_bitrateRow, &m_latencyRow, &m_fpsRow,
		&m_rejectedRow, &m_discardedRow, &m_queueDropRow, &m_poolDropRow, &m_notConsumedRow,
		&m_bufferRow, &m_waitRow, &m_resyncRow,
	};

	// 색 보간을 진행시킨다.
	//
	// UILabel 은 SetTextStyle 로 받은 색을 목표로만 두고, 실제로 그리는 색은
	// Update(dt) 에서 지수 보간으로 따라간다. 부르지 않으면 Initialize 때의
	// 색에 영원히 머문다 — 손실 줄이 0 에서 올라가도 강조색이 안 뜬다.
	//
	// dt 는 실제 경과 시간이라 프레임률과 무관하게 수렴한다. 스트리밍 중
	// (60fps)에는 0.2초쯤, 일시정지 중(통계 주기 2Hz)에는 dt 가 0.1초로
	// 잘려 들어와 몇 프레임 안에 맞춰진다.
	const float deltaSeconds = m_context ? m_context->GetDeltaTime() : 0.0f;

	for (StatsRow* row : rows)
	{
		if (row->caption) row->caption->Prepare();

		if (row->value)
		{
			row->value->Update(deltaSeconds);
			row->value->Prepare();
		}
	}

	// 기하 객체는 표본이 바뀐 프레임에만 다시 만든다. 매 프레임 만들면
	// 초당 120개의 COM 객체가 생겼다 사라진다.
	RebuildSparkline(m_bitrateLine);
	RebuildSparkline(m_latencyLine);

	return true;
}

bool ServiceStatsLayer::Render()
{
	if (!m_visible || !m_context || !m_panelBrush)
		return false;

	if (m_panelRect.right <= m_panelRect.left)
		return false;

	ID2D1DeviceContext* d2dContext = m_context->GetD2DDeviceContext();
	if (!d2dContext)
		return false;

	const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
		D2D1::RectF(m_panelRect.left, m_panelRect.top, m_panelRect.right, m_panelRect.bottom),
		6.0f, 6.0f);

	d2dContext->FillRoundedRectangle(panel, m_panelBrush);

	if (m_headerLabel)   m_headerLabel->Render();
	if (m_streamSection) m_streamSection->Render();
	if (m_lossSection)   m_lossSection->Render();
	if (m_paceSection)   m_paceSection->Render();

	StatsRow* rows[] = {
		&m_bitrateRow, &m_latencyRow, &m_fpsRow,
		&m_rejectedRow, &m_discardedRow, &m_queueDropRow, &m_poolDropRow, &m_notConsumedRow,
		&m_bufferRow, &m_waitRow, &m_resyncRow,
	};

	for (StatsRow* row : rows)
	{
		if (row->caption) row->caption->Render();
		if (row->value)   row->value->Render();
	}

	RenderSparkline(d2dContext, m_bitrateLine);
	RenderSparkline(d2dContext, m_latencyLine);

	return true;
}

void ServiceStatsLayer::OnResize(uint32_t width, uint32_t height)
{
	m_viewWidth = static_cast<float>(width);
	m_viewHeight = static_cast<float>(height);

	UpdateLayout();
}

void ServiceStatsLayer::OnDeviceLost()
{
	ReleaseDeviceResources();
	ReleaseSparklineGeometry();

	UILabel* labels[] = {
		m_headerLabel.get(), m_streamSection.get(), m_lossSection.get(), m_paceSection.get(),
	};

	for (UILabel* label : labels)
	{
		if (label)
			label->DiscardDeviceResources();
	}

	StatsRow* rows[] = {
		&m_bitrateRow, &m_latencyRow, &m_fpsRow,
		&m_rejectedRow, &m_discardedRow, &m_queueDropRow, &m_poolDropRow, &m_notConsumedRow,
		&m_bufferRow, &m_waitRow, &m_resyncRow,
	};

	for (StatsRow* row : rows)
	{
		if (row->caption) row->caption->DiscardDeviceResources();
		if (row->value)   row->value->DiscardDeviceResources();
	}
}

void ServiceStatsLayer::OnDeviceRestored()
{
	if (!m_context)
		return;

	CreateDeviceResources(m_context);

	UILabel* labels[] = {
		m_headerLabel.get(), m_streamSection.get(), m_lossSection.get(), m_paceSection.get(),
	};

	for (UILabel* label : labels)
	{
		if (label)
			label->RestoreDeviceResources(m_context);
	}

	StatsRow* rows[] = {
		&m_bitrateRow, &m_latencyRow, &m_fpsRow,
		&m_rejectedRow, &m_discardedRow, &m_queueDropRow, &m_poolDropRow, &m_notConsumedRow,
		&m_bufferRow, &m_waitRow, &m_resyncRow,
	};

	for (StatsRow* row : rows)
	{
		if (row->caption) row->caption->RestoreDeviceResources(m_context);
		if (row->value)   row->value->RestoreDeviceResources(m_context);
	}

	UpdateLayout();
}

void ServiceStatsLayer::SetVisible(bool visible)
{
	m_visible = visible;
}

bool ServiceStatsLayer::IsVisible() const
{
	return m_visible;
}

void ServiceStatsLayer::SetStatsInfo(const StreamingStatsInfo& stats)
{
	m_stats = stats;

	// 오버레이가 꺼져 있어도 표본은 쌓는다. 켜는 순간 지난 30초가
	// 이미 그려져 있어야 쓸모가 있다 — 끊긴 뒤에 켜면 늦는다.
	PushSample(m_bitrateLine, stats.bitrateMbps);
	PushSample(m_latencyLine, (stats.latencyMs >= 0.0f) ? stats.latencyMs : 0.0f);

	ApplyStats();
}

void ServiceStatsLayer::SetQualityInfo(const StreamingQualityInfo& quality)
{
	m_quality = quality;
	ApplyStats();
}

bool ServiceStatsLayer::CreateChildren(IRenderContext* context)
{
	UITextStyle headerStyle = {};
	::wcscpy_s(headerStyle.fontName, L"Consolas");
	headerStyle.fontSize = 12.0f;
	headerStyle.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
	headerStyle.hAlign = DWRITE_TEXT_ALIGNMENT_LEADING;
	headerStyle.vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
	headerStyle.normal.fill = kNormalColor;
	headerStyle.hover.fill = kNormalColor;
	headerStyle.pressed.fill = kNormalColor;
	headerStyle.disabled.fill = kNormalColor;

	// 문구는 여기서 넣지 않는다. 레이아웃보다 먼저 넣으면 영영 그려지지
	// 않는다 — 이유는 ApplyStaticText 선언부 주석에 있다.
	m_headerLabel = std::make_unique<UILabel>();
	m_headerLabel->SetFontManager(m_fontManager);
	m_headerLabel->SetTextStyle(headerStyle);
	if (!m_headerLabel->Initialize(context))
		return false;

	// 구역 제목. 값이 아니라 칸막이라 색을 따로 준다.
	UITextStyle sectionStyle = headerStyle;
	sectionStyle.fontSize = 10.0f;
	sectionStyle.normal.fill = kSectionColor;
	sectionStyle.hover.fill = kSectionColor;
	sectionStyle.pressed.fill = kSectionColor;
	sectionStyle.disabled.fill = kSectionColor;

	auto createSection = [&](std::unique_ptr<UILabel>& label) -> bool
	{
		label = std::make_unique<UILabel>();
		label->SetFontManager(m_fontManager);
		label->SetTextStyle(sectionStyle);
		return label->Initialize(context);
	};

	if (!createSection(m_streamSection))
		return false;

	if (!createSection(m_lossSection))
		return false;

	if (!createSection(m_paceSection))
		return false;

	struct RowSpec { StatsRow* row; const wchar_t* caption; bool lossRow; };

	const RowSpec specs[] = {
		{ &m_bitrateRow,     L"수신",        false },
		{ &m_latencyRow,     L"지연",        false },
		{ &m_fpsRow,         L"표시",        false },
		{ &m_rejectedRow,    L"청크 거절",   true  },
		{ &m_discardedRow,   L"프레임 폐기", true  },
		{ &m_queueDropRow,   L"디코드 큐",   true  },
		{ &m_poolDropRow,    L"풀 고갈",     true  },
		{ &m_notConsumedRow, L"미소비",      true  },
		{ &m_bufferRow,      L"버퍼",        false },
		{ &m_waitRow,        L"평균 대기",   false },

		// resync 는 손실이 아니다. 재개할 때마다, 스트림이 바뀔 때마다
		// 오르는 값이라 강조하면 한 번 멈췄다 켠 뒤로 영영 주황이 된다.
		{ &m_resyncRow,      L"resync",      false },
	};

	for (const RowSpec& spec : specs)
	{
		if (!CreateRow(context, *spec.row, spec.caption, spec.lossRow))
			return false;
	}

	return true;
}

bool ServiceStatsLayer::CreateRow(IRenderContext* context, StatsRow& row, const wchar_t* caption, bool lossRow)
{
	row.lossRow = lossRow;

	UITextStyle captionStyle = {};
	::wcscpy_s(captionStyle.fontName, L"Malgun Gothic");
	captionStyle.fontSize = 11.0f;
	captionStyle.hAlign = DWRITE_TEXT_ALIGNMENT_LEADING;
	captionStyle.vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
	captionStyle.normal.fill = kCaptionColor;
	captionStyle.hover.fill = kCaptionColor;
	captionStyle.pressed.fill = kCaptionColor;
	captionStyle.disabled.fill = kCaptionColor;

	row.caption = std::make_unique<UILabel>();
	row.caption->SetFontManager(m_fontManager);
	row.caption->SetTextStyle(captionStyle);
	if (!row.caption->Initialize(context))
		return false;

	row.captionText = caption;

	// 값은 고정폭이다. 숫자가 바뀔 때마다 자릿수가 흔들리면 읽기 힘들다.
	//
	// 처음 색을 그 줄의 기본 색으로 넣는다. Initialize 가 이 색에 곧바로
	// 맞춰 두므로, 여기서 틀린 색을 넣으면 첫 프레임에 페이드가 보인다.
	row.value = std::make_unique<UILabel>();
	row.value->SetFontManager(m_fontManager);
	row.value->SetTextStyle(MakeValueStyle(ValueColor(lossRow, false)));
	if (!row.value->Initialize(context))
		return false;

	// 비워 둔다. 초기값을 여기서 넣으면 ApplyStats 가 같은 문자열이라며
	// 건너뛰고, 그 줄은 영영 비어 있게 된다. 손실 카운터처럼 계속 0 인
	// 줄이 정확히 그렇게 사라졌었다.
	row.lastValue.clear();
	row.highlighted = false;

	return true;
}

bool ServiceStatsLayer::CreateDeviceResources(IRenderContext* context)
{
	if (!context)
		return false;

	ID2D1DeviceContext* d2dContext = context->GetD2DDeviceContext();
	if (!d2dContext)
		return false;

	SafeRelease(m_panelBrush);
	SafeRelease(m_lineBrush);

	// 컨트롤 바보다 조금 더 진하다. 이쪽은 읽는 것이 목적이라
	// 뒤 영상이 비쳐 보이면 숫자가 안 읽힌다.
	if (FAILED(d2dContext->CreateSolidColorBrush(Rgba(0.0f, 0.0f, 0.0f, 0.72f), &m_panelBrush)))
		return false;

	if (FAILED(d2dContext->CreateSolidColorBrush(Rgba(1.0f, 1.0f, 1.0f, 1.0f), &m_lineBrush)))
		return false;

	return true;
}

void ServiceStatsLayer::ReleaseDeviceResources()
{
	SafeRelease(m_panelBrush);
	SafeRelease(m_lineBrush);
}

void ServiceStatsLayer::ReleaseSparklineGeometry()
{
	SafeRelease(m_bitrateLine.geometry);
	SafeRelease(m_latencyLine.geometry);

	m_bitrateLine.dirty = true;
	m_latencyLine.dirty = true;
}

void ServiceStatsLayer::PushSample(Sparkline& line, float value)
{
	if (line.samples.empty())
		return;

	line.samples[line.nextIndex] = (value > 0.0f) ? value : 0.0f;
	line.nextIndex = (line.nextIndex + 1) % line.samples.size();

	if (line.nextIndex == 0)
		line.filled = true;

	line.dirty = true;
}

void ServiceStatsLayer::RebuildSparkline(Sparkline& line)
{
	if (!line.dirty || !m_context)
		return;

	line.dirty = false;

	SafeRelease(line.geometry);

	if (line.samples.empty() || line.bounds.right <= line.bounds.left)
		return;

	ID2D1DeviceContext* d2dContext = m_context->GetD2DDeviceContext();
	if (!d2dContext)
		return;

	ID2D1Factory* factory = nullptr;
	d2dContext->GetFactory(&factory);
	if (!factory)
		return;

	ID2D1PathGeometry* geometry = nullptr;
	if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
	{
		SafeRelease(factory);
		return;
	}

	// 눈금은 창 안의 최대값으로 잡는다. 고정 눈금으로 두면 20 Mbps 짜리
	// 스트림과 3 Mbps 짜리 스트림 중 하나는 반드시 안 보인다.
	float maxValue = line.lastMax;
	for (float sample : line.samples)
	{
		if (sample > maxValue)
			maxValue = sample;
	}

	if (maxValue <= 0.0f)
		maxValue = 1.0f;

	line.lastMax = maxValue;

	const float width = line.bounds.right - line.bounds.left;
	const float height = line.bounds.bottom - line.bounds.top;
	const size_t count = line.samples.size();
	const float stepX = width / static_cast<float>(count - 1);

	ID2D1GeometrySink* sink = nullptr;
	if (SUCCEEDED(geometry->Open(&sink)) && sink)
	{
		// 링 버퍼를 오래된 것부터 읽는다. nextIndex 가 다음에 쓸 칸이니
		// 곧 가장 오래된 칸이다.
		auto sampleAt = [&](size_t offset) -> float
		{
			const size_t index = (line.nextIndex + offset) % count;
			return line.samples[index];
		};

		auto pointAt = [&](size_t offset) -> D2D1_POINT_2F
		{
			const float value = sampleAt(offset);
			const float ratio = (value / maxValue);
			return D2D1::Point2F(
				line.bounds.left + stepX * static_cast<float>(offset),
				line.bounds.bottom - height * ((ratio > 1.0f) ? 1.0f : ratio));
		};

		sink->BeginFigure(pointAt(0), D2D1_FIGURE_BEGIN_HOLLOW);

		for (size_t offset = 1; offset < count; ++offset)
		{
			sink->AddLine(pointAt(offset));
		}

		sink->EndFigure(D2D1_FIGURE_END_OPEN);
		sink->Close();

		line.geometry = geometry;
	}
	else
	{
		SafeRelease(geometry);
	}

	SafeRelease(sink);
	SafeRelease(factory);
}

void ServiceStatsLayer::RenderSparkline(ID2D1DeviceContext* d2dContext, Sparkline& line)
{
	if (!line.geometry || !m_lineBrush)
		return;

	// 바닥선. 값이 0 으로 깔린 구간과 데이터가 없는 구간을 구분해 준다.
	m_lineBrush->SetColor(Rgba(1.0f, 1.0f, 1.0f, 0.12f));
	d2dContext->DrawLine(
		D2D1::Point2F(line.bounds.left, line.bounds.bottom),
		D2D1::Point2F(line.bounds.right, line.bounds.bottom),
		m_lineBrush, 1.0f);

	m_lineBrush->SetColor(line.color);
	d2dContext->DrawGeometry(line.geometry, m_lineBrush, 1.4f);
}

void ServiceStatsLayer::UpdateLayout()
{
	if (m_viewWidth <= 0.0f || m_viewHeight <= 0.0f)
		return;

	// 세로로 쌓기만 하면 되므로 커서 하나로 끝난다.
	const float left = kPanelMargin;
	const float top = kPanelMargin;
	const float contentLeft = left + kPanelPadding;
	const float contentRight = left + kPanelWidth - kPanelPadding;
	const float contentWidth = contentRight - contentLeft;

	float cursorY = top + kPanelPadding;

	auto placeFull = [&](UILabel* label, float height)
	{
		if (label)
		{
			label->SetLayout({ contentLeft, cursorY, contentRight, cursorY + height });
		}
		cursorY += height;
	};

	auto placeRow = [&](StatsRow& row)
	{
		if (row.caption)
		{
			row.caption->SetLayout(
				{ contentLeft, cursorY, contentRight - kValueWidth, cursorY + kRowHeight });
		}

		if (row.value)
		{
			row.value->SetLayout(
				{ contentRight - kValueWidth, cursorY, contentRight, cursorY + kRowHeight });
		}

		cursorY += kRowHeight;
	};

	auto placeSpark = [&](Sparkline& line)
	{
		line.bounds.left = contentLeft;
		line.bounds.right = contentRight;
		line.bounds.top = cursorY;
		line.bounds.bottom = cursorY + kSparkHeight;
		line.dirty = true;

		cursorY += kSparkHeight;
	};

	placeFull(m_headerLabel.get(), kHeaderHeight);
	cursorY += kSectionGap;

	placeFull(m_streamSection.get(), kSectionHeight);
	placeRow(m_bitrateRow);
	placeSpark(m_bitrateLine);
	placeRow(m_latencyRow);
	placeSpark(m_latencyLine);
	placeRow(m_fpsRow);
	cursorY += kSectionGap;

	placeFull(m_lossSection.get(), kSectionHeight);
	placeRow(m_rejectedRow);
	placeRow(m_discardedRow);
	placeRow(m_queueDropRow);
	placeRow(m_poolDropRow);
	placeRow(m_notConsumedRow);
	cursorY += kSectionGap;

	placeFull(m_paceSection.get(), kSectionHeight);
	placeRow(m_bufferRow);
	placeRow(m_waitRow);
	placeRow(m_resyncRow);

	m_panelRect.left = left;
	m_panelRect.right = left + kPanelWidth;
	m_panelRect.top = top;
	m_panelRect.bottom = cursorY + kPanelPadding;

	(void)contentWidth;
}

void ServiceStatsLayer::ApplyStaticText()
{
	if (m_streamSection) m_streamSection->SetText(L"STREAM");
	if (m_lossSection)   m_lossSection->SetText(L"LOSS");
	if (m_paceSection)   m_paceSection->SetText(L"PACING");

	StatsRow* rows[] = {
		&m_bitrateRow, &m_latencyRow, &m_fpsRow,
		&m_rejectedRow, &m_discardedRow, &m_queueDropRow, &m_poolDropRow, &m_notConsumedRow,
		&m_bufferRow, &m_waitRow, &m_resyncRow,
	};

	for (StatsRow* row : rows)
	{
		if (row->caption && !row->captionText.empty())
		{
			row->caption->SetText(row->captionText.c_str());
		}
	}
}

void ServiceStatsLayer::SetRowValue(StatsRow& row, const wchar_t* text, bool highlight)
{
	if (!row.value)
		return;

	if (row.lastValue != text)
	{
		row.lastValue = text;
		row.value->SetText(text);
	}

	// 손실 줄이 아니면 색이 바뀔 일이 없다.
	if (!row.lossRow || row.highlighted == highlight)
		return;

	row.highlighted = highlight;

	// 스타일 전체를 다시 넣는 것은 색 하나 때문이지만, UITextStyle 에
	// 색만 바꾸는 통로가 없다. 값이 0 과 0 아님을 넘나들 때만 일어나므로
	// 실제로는 거의 불리지 않는다.
	//
	// 이것은 목표 색만 옮긴다. 실제로 그려지는 색은 UILabel 이
	// Update(dt) 에서 보간하므로 Prepare 가 매 프레임 불러 준다.
	row.value->SetTextStyle(MakeValueStyle(ValueColor(true, highlight)));
}

void ServiceStatsLayer::ApplyStats()
{
	wchar_t text[64] = {};

	// --- 머리줄 ---
	if (m_headerLabel)
	{
		if (m_quality.width > 0 && m_quality.height > 0)
		{
			::swprintf_s(text, L"%up%u  H.264  %s",
				m_quality.height, m_quality.fps,
				m_stats.connected ? L"연결됨" : L"끊김");
		}
		else
		{
			::swprintf_s(text, L"--  %s", m_stats.connected ? L"연결됨" : L"끊김");
		}

		m_headerLabel->SetText(text);
	}

	// --- STREAM ---
	::swprintf_s(text, L"%.1f Mbps", m_stats.bitrateMbps);
	SetRowValue(m_bitrateRow, text, false);

	if (m_stats.latencyMs >= 0.0f)
		::swprintf_s(text, L"%.0f ms", m_stats.latencyMs);
	else
		::swprintf_s(text, L"--");
	SetRowValue(m_latencyRow, text, false);

	::swprintf_s(text, L"%.0f fps", m_stats.presentedFps);
	SetRowValue(m_fpsRow, text, false);

	// --- LOSS ---
	//
	// 여기가 이 패널의 요점이다. 0 이 아닌 줄만 강조색으로 뜬다.
	struct LossSpec { StatsRow* row; uint64_t value; };

	const LossSpec losses[] = {
		{ &m_rejectedRow,    m_stats.chunksRejected },
		{ &m_discardedRow,   m_stats.framesDiscarded },
		{ &m_queueDropRow,   m_stats.decodeQueueDrops },
		{ &m_poolDropRow,    m_stats.poolExhausted },
		{ &m_notConsumedRow, m_stats.notConsumed },
	};

	for (const LossSpec& loss : losses)
	{
		::swprintf_s(text, L"%llu", static_cast<unsigned long long>(loss.value));
		SetRowValue(*loss.row, text, loss.value > 0);
	}

	// --- PACING ---
	::swprintf_s(text, L"%u ms", m_stats.jitterBufferMs);
	SetRowValue(m_bufferRow, text, false);

	::swprintf_s(text, L"%.1f ms", m_stats.avgPaceWaitMs);
	SetRowValue(m_waitRow, text, false);

	::swprintf_s(text, L"%llu", static_cast<unsigned long long>(m_stats.resyncCount));
	SetRowValue(m_resyncRow, text, false);
}
