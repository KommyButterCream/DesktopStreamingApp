#include "ServiceMenuButton.h"

void ServiceMenuButton::SetCommandId(uint32_t commandId)
{
	m_commandId = commandId;
}

uint32_t ServiceMenuButton::GetCommandId() const
{
	return m_commandId;
}

void ServiceMenuButton::SetClickCallback(ClickCallback callback, void* userData)
{
	m_clickCallback = callback;
	m_clickUserData = userData;
}

void ServiceMenuButton::OnClick()
{
	if (m_clickCallback)
	{
		m_clickCallback(m_commandId, m_clickUserData);
	}
}
