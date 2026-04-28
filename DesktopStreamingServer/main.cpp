
#include "../../../Module/Core/DirectX/DxDebugUtils.h"

int main()
{



#if defined(_DEBUG)
	Core::DirectX::DxReportLiveObjects();
#endif

	return 0;
}