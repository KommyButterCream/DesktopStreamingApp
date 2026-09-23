#include "ServiceGlyphButton.h"

#include "../../../../Module/D3D11EngineInterface/IRenderContext.h"
#include "../../../../Module/Core/ShapeType/Point2f.h"
#include "../../../../Module/Core/DirectX/DxSafeRelease.h"

using namespace Core::DirectX;

ServiceGlyphButton::~ServiceGlyphButton()
{
	Shutdown();
}

bool ServiceGlyphButton::AcquireDeviceResources(IRenderContext* context, bool reset)
{
	return CreateBrushes(context, reset);
}

void ServiceGlyphButton::Shutdown()
{
	SafeRelease(m_backgroundBrush);
	SafeRelease(m_glyphBrush);

	UIElementBase::Shutdown();
}

void ServiceGlyphButton::DiscardDeviceResources()
{
	SafeRelease(m_backgroundBrush);
	SafeRelease(m_glyphBrush);
}

// 상태 전이가 즉시 반영된다(페이드 없음). 애니메이션이 없으므로
// 더 그릴 이유도 없다.
bool ServiceGlyphButton::Update(float)
{
	return false;
}

bool ServiceGlyphButton::Render()
{
	if (!IsVisible() || !m_context)
		return false;

	if (!m_backgroundBrush || !m_glyphBrush)
		return false;

	ID2D1DeviceContext* d2dContext = m_context->GetD2DDeviceContext();
	if (!d2dContext)
		return false;

	if (m_updateColor)
	{
		m_glyphBrush->SetColor(m_glyphColor);
		m_updateColor = false;
	}

	const auto& layout = LayoutData();
	const UIColorSet& colorSet = CurrentColorSet();

	// 배경. 상태마다 색이 다르므로 그릴 때마다 넣는다 —
	// 브러시 하나를 돌려 쓰는 편이 상태별로 네 개를 들고 있는 것보다 싸다.
	m_backgroundBrush->SetColor(colorSet.fill);

	const D2D1_ROUNDED_RECT background = D2D1::RoundedRect(
		D2D1::RectF(layout.left, layout.top, layout.right, layout.bottom), 3.0f, 3.0f);

	d2dContext->FillRoundedRectangle(background, m_backgroundBrush);

	if (m_style.borderThickness > 0.0f)
	{
		m_backgroundBrush->SetColor(colorSet.border);
		d2dContext->DrawRoundedRectangle(background, m_backgroundBrush, m_style.borderThickness);
	}

	DrawGlyph(d2dContext);

	return true;
}

void ServiceGlyphButton::OnActivated()
{
	if (m_clickCallback)
	{
		m_clickCallback(m_commandId, m_clickUserData);
	}
}

void ServiceGlyphButton::SetGlyph(ServiceGlyph glyph)
{
	m_glyph = glyph;
}

ServiceGlyph ServiceGlyphButton::GetGlyph() const
{
	return m_glyph;
}

void ServiceGlyphButton::SetCommandId(uint32_t commandId)
{
	m_commandId = commandId;
}

void ServiceGlyphButton::SetClickCallback(ClickCallback callback, void* userData)
{
	m_clickCallback = callback;
	m_clickUserData = userData;
}

void ServiceGlyphButton::SetGlyphColor(const D2D1_COLOR_F& color)
{
	m_glyphColor = color;
	m_updateColor = true;
}

void ServiceGlyphButton::SetGlyphScale(float scale)
{
	if (scale <= 0.0f || scale > 1.0f)
		return;

	m_glyphScale = scale;
}

bool ServiceGlyphButton::CreateBrushes(IRenderContext* context, bool resetState)
{
	if (!BindRenderContext(context, resetState))
		return false;

	ID2D1DeviceContext* d2dContext = m_context->GetD2DDeviceContext();
	if (!d2dContext)
		return false;

	SafeRelease(m_backgroundBrush);
	SafeRelease(m_glyphBrush);

	if (FAILED(d2dContext->CreateSolidColorBrush(m_style.normal.fill, &m_backgroundBrush)))
		return false;

	if (FAILED(d2dContext->CreateSolidColorBrush(m_glyphColor, &m_glyphBrush)))
		return false;

	m_updateColor = false;
	return true;
}

const UIColorSet& ServiceGlyphButton::CurrentColorSet() const
{
	switch (GetState())
	{
	case UIElementState::Hovered:  return m_style.hover;
	case UIElementState::Pressed:  return m_style.pressed;
	case UIElementState::Disabled: return m_style.disabled;
	default:                       return m_style.normal;
	}
}

// 도형은 버튼 사각형 안쪽에 정규화 비율로 그린다. 버튼 크기가 바뀌어도
// 같은 모양이 유지되고, 미리 만들어 둘 기하 객체가 없다.
void ServiceGlyphButton::DrawGlyph(ID2D1DeviceContext* d2dContext) const
{
	const auto& layout = LayoutData();
	const Point2f center = layout.Center();

	const float side = ((layout.right - layout.left) < (layout.bottom - layout.top))
		? (layout.right - layout.left)
		: (layout.bottom - layout.top);

	const float half = side * m_glyphScale * 0.5f;
	if (half <= 0.0f)
		return;

	switch (m_glyph)
	{
	case ServiceGlyph::Play:
	{
		// 오른쪽을 가리키는 삼각형. 눈에 가운데로 보이도록 살짝 오른쪽으로
		// 민다 — 무게중심이 왼쪽에 쏠려 있어서 기하학적 중앙에 두면
		// 왼쪽으로 치우쳐 보인다.
		const float nudge = half * 0.12f;

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

		ID2D1GeometrySink* sink = nullptr;
		if (SUCCEEDED(geometry->Open(&sink)) && sink)
		{
			sink->BeginFigure(
				D2D1::Point2F(center.x - half + nudge, center.y - half),
				D2D1_FIGURE_BEGIN_FILLED);
			sink->AddLine(D2D1::Point2F(center.x + half + nudge, center.y));
			sink->AddLine(D2D1::Point2F(center.x - half + nudge, center.y + half));
			sink->EndFigure(D2D1_FIGURE_END_CLOSED);
			sink->Close();

			d2dContext->FillGeometry(geometry, m_glyphBrush);
		}

		SafeRelease(sink);
		SafeRelease(geometry);
		SafeRelease(factory);
		break;
	}

	case ServiceGlyph::Pause:
	{
		const float barWidth = half * 0.42f;
		const float gap = half * 0.32f;

		d2dContext->FillRectangle(
			D2D1::RectF(center.x - gap - barWidth, center.y - half, center.x - gap, center.y + half),
			m_glyphBrush);

		d2dContext->FillRectangle(
			D2D1::RectF(center.x + gap, center.y - half, center.x + gap + barWidth, center.y + half),
			m_glyphBrush);
		break;
	}

	case ServiceGlyph::Stop:
	{
		d2dContext->FillRectangle(
			D2D1::RectF(center.x - half, center.y - half, center.x + half, center.y + half),
			m_glyphBrush);
		break;
	}

	default:
		break;
	}
}
