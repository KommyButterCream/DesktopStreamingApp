#pragma once

#include "../../../../Module/D3D11UIFramework/D3D11UIFramework/Button/UIContextMenuButton.h"

#include <stdint.h>

// 컨텍스트 메뉴 항목 하나, 그리고 그 메뉴를 여는 버튼.
//
// UIContextMenuButton 을 그대로 쓰되 클릭을 함수 포인터로 돌려받는다.
// 기반 클래스는 UICommand 열거형으로 디스패치하는데, 그 열거형은 뷰어 것이라
// 서비스 항목을 추가할 수 없다. OnClick 이 virtual 이라 상속 한 겹으로 끝난다.
//
// 화질 목록처럼 항목 수가 런타임에 정해지는 경우, 열거형으로는 애초에
// 표현할 수 없다는 점도 있다.
class ServiceMenuButton : public UIContextMenuButton
{
public:
	// commandId 는 이 DLL 안에서만 의미가 있다.
	// 메뉴 항목이면 목록 인덱스, 여는 버튼이면 명령 값이다.
	using ClickCallback = void (*)(uint32_t commandId, void* userData);

	ServiceMenuButton() = default;
	virtual ~ServiceMenuButton() = default;

public:
	void SetCommandId(uint32_t commandId);
	uint32_t GetCommandId() const;

	void SetClickCallback(ClickCallback callback, void* userData);

protected:
	// UIContextMenuButton Override
	void OnClick() override;

private:
	uint32_t m_commandId = 0;

	ClickCallback m_clickCallback = nullptr;
	void* m_clickUserData = nullptr;
};
