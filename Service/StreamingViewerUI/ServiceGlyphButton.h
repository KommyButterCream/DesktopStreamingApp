#pragma once

#include "../../../../Module/D3D11UIFramework/D3D11UIFramework/Base/UIElementBase.h"

#include <stdint.h>

// 이 버튼이 그릴 도형.
//
// 뷰어의 UIIconShape 를 쓰지 않는다. 그 열거형에 항목을 추가하면 서비스가
// 하나 늘 때마다 뷰어 저장소가 바뀌고, 그러면 공용 DLL 로 관리할 수 없다.
// 서비스 UI 는 네이티브라 D2D 를 직접 쓸 수 있으므로 자기 도형을 자기가
// 그리는 것이 맞다.
enum class ServiceGlyph : uint32_t
{
	None = 0,
	Play,    // 오른쪽을 가리키는 삼각형
	Pause,   // 세로 막대 둘
	Stop,    // 정사각형
	Stats,   // 높이가 다른 막대 셋 (통계 오버레이 토글)
};

// 도형 하나를 그리는 버튼.
//
// UIButton 을 상속하지 않는 이유는 그쪽이 UIIconShape / UICommand 와 묶여
// 있기 때문이다. 여기서 필요한 것은 배경 + 도형 + 클릭 통지뿐이라
// UIElementBase 에서 바로 내려오는 편이 얇다.
class ServiceGlyphButton : public UIElementBase
{
public:
	// commandId 는 이 DLL 안에서만 의미가 있는 값이다.
	// (StreamingViewerCommand 를 그대로 캐스팅해 넣는다)
	using ClickCallback = void (*)(uint32_t commandId, void* userData);

	ServiceGlyphButton() = default;
	virtual ~ServiceGlyphButton();

public:
	// IRenderLayer Override
	void Shutdown() override;
	bool Update(float dt) override;
	bool Render() override;
	void DiscardDeviceResources() override;

public:
	void SetGlyph(ServiceGlyph glyph);
	ServiceGlyph GetGlyph() const;

	// 토글 버튼이 켜져 있는 상태. 배경이 항상 눌린 색으로 남는다.
	//
	// UIElementBase 의 상태 머신에는 "켜짐" 이 없다. Pressed 는 마우스를
	// 누르고 있는 동안만이라 손을 떼면 풀린다. 눌러서 켜 두는 버튼은
	// 뗀 뒤에도 켜져 보여야 하므로 별도 표시가 필요하다.
	void SetActive(bool active);
	bool IsActive() const;

	void SetCommandId(uint32_t commandId);
	void SetClickCallback(ClickCallback callback, void* userData);

	// 도형 색. 배경은 UIStyle 의 4상태를 그대로 쓴다.
	void SetGlyphColor(const D2D1_COLOR_F& color);

	// 도형이 버튼 안에서 차지하는 비율(0 ~ 1). 기본 0.42.
	void SetGlyphScale(float scale);

protected:
	// UIElementBase Override
	bool AcquireDeviceResources(IRenderContext* context, bool reset) override;

	// 누른 자리에서 뗐다. 기반 클래스가 클릭을 판정해 여기로 보낸다.
	void OnActivated() override;

private:
	bool CreateBrushes(IRenderContext* context, bool resetState);

	// 지금 상태에 해당하는 배경색.
	const UIColorSet& CurrentColorSet() const;

	void DrawGlyph(ID2D1DeviceContext* d2dContext) const;

private:
	ServiceGlyph m_glyph = ServiceGlyph::None;
	bool m_active = false;
	uint32_t m_commandId = 0;

	float m_glyphScale = 0.42f;

	bool m_updateColor = true;
	D2D1_COLOR_F m_glyphColor = { 1.0f, 1.0f, 1.0f, 1.0f };

	ID2D1SolidColorBrush* m_backgroundBrush = nullptr;
	ID2D1SolidColorBrush* m_glyphBrush = nullptr;

	ClickCallback m_clickCallback = nullptr;
	void* m_clickUserData = nullptr;
};
