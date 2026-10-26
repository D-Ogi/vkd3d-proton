// The native SDK declares InfoQueue, which the cross-test's WIDL subset omits.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <vector>

extern "C" void ser_dump_infoqueue(void *device)
{
    ID3D12InfoQueue *queue = nullptr;
    HRESULT hr = static_cast<IUnknown *>(device)->QueryInterface(IID_PPV_ARGS(&queue));
    if (FAILED(hr))
    {
        std::printf("D3D12 InfoQueue unavailable HRESULT=%#lx (use --validate).\n", static_cast<unsigned long>(hr));
        return;
    }
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T bytes = 0;
        queue->GetMessage(i, nullptr, &bytes);
        std::vector<unsigned char> data(bytes);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(data.data());
        if (bytes && SUCCEEDED(queue->GetMessage(i, message, &bytes)))
            std::printf("D3D12_MESSAGE id=%u severity=%u %s\n", message->ID, message->Severity, message->pDescription);
    }
    queue->ClearStoredMessages();
    queue->Release();
}
