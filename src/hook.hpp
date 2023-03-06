#include "Windows.h"

#include "vector"
#include <atlbase.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

#include "detours/detours.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "detours.x64.lib")

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam);

typedef HRESULT(__fastcall *Present)(IDXGISwapChain3 *pSwapChain,
                                     UINT SyncInterval, UINT Flags);

typedef void (*ExecuteCommandLists)(ID3D12CommandQueue *queue,
                                    UINT NumCommandLists,
                                    ID3D12CommandList *ppCommandLists);

typedef HRESULT(__fastcall *ResizeBuffers)(IDXGISwapChain3 *pSwapChain,
                                           UINT BufferCount, UINT Width,
                                           UINT Height, DXGI_FORMAT NewFormat,
                                           UINT SwapChainFlags);

static Present OriginalPresent;
static ExecuteCommandLists OriginalExecuteCommandLists;
static ResizeBuffers OriginalResizeBuffers;

struct FrameContext {
  CComPtr<ID3D12CommandAllocator> command_allocator = NULL;
  CComPtr<ID3D12Resource> main_render_target_resource = NULL;
  D3D12_CPU_DESCRIPTOR_HANDLE main_render_target_descriptor;
};

static std::vector<FrameContext> g_FrameContext;
static UINT g_FrameBufferCount = 0;

static CComPtr<ID3D12DescriptorHeap> g_pD3DRtvDescHeap = NULL;
static CComPtr<ID3D12DescriptorHeap> g_pD3DSrvDescHeap = NULL;
static CComPtr<ID3D12CommandQueue> g_pD3DCommandQueue = NULL;
static CComPtr<ID3D12GraphicsCommandList> g_pD3DCommandList = NULL;

static WNDPROC OriginalWndProc;
static HWND TargetWindow = nullptr;

static uint64_t *g_MethodsTable = NULL;
static bool g_Initialized = false;

LRESULT APIENTRY WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {

  if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) {

    return ERROR_SUCCESS;
  }

  // if (uMsg == WM_SETCURSOR) {
  //   ImGuiIO &io = ImGui::GetIO();
  //   ::SetCursor(io.WantCaptureMouse ? ::LoadCursor(nullptr, IDC_ARROW)
  //                                   : nullptr);
  //   return ERROR_SUCCESS;
  // }

  return CallWindowProc(OriginalWndProc, hWnd, uMsg, wParam, lParam);
}

HRESULT __fastcall HookPresent(IDXGISwapChain3 *pSwapChain, UINT SyncInterval,
                               UINT Flags) {
  if (g_pD3DCommandQueue == nullptr) {
    return OriginalPresent(pSwapChain, SyncInterval, Flags);
  }
  if (!g_Initialized) {
    ID3D12Device *pD3DDevice;

    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&pD3DDevice)))) {
      return OriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    {
      DXGI_SWAP_CHAIN_DESC desc;
      pSwapChain->GetDesc(&desc);
      TargetWindow = desc.OutputWindow;
      if (!OriginalWndProc) {
        OriginalWndProc = (WNDPROC)SetWindowLongPtr(
            TargetWindow, GWLP_WNDPROC, (__int3264)(LONG_PTR)WndProc);
      }
      g_FrameBufferCount = desc.BufferCount;
      g_FrameContext.clear();
      g_FrameContext.resize(g_FrameBufferCount);
    }

    {
      D3D12_DESCRIPTOR_HEAP_DESC desc = {};
      desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      desc.NumDescriptors = g_FrameBufferCount;
      desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

      if (pD3DDevice->CreateDescriptorHeap(
              &desc, IID_PPV_ARGS(&g_pD3DSrvDescHeap)) != S_OK) {
        return OriginalPresent(pSwapChain, SyncInterval, Flags);
      }
    }

    {
      D3D12_DESCRIPTOR_HEAP_DESC desc;
      desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      desc.NumDescriptors = g_FrameBufferCount;
      desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
      desc.NodeMask = 1;

      if (pD3DDevice->CreateDescriptorHeap(
              &desc, IID_PPV_ARGS(&g_pD3DRtvDescHeap)) != S_OK) {
        return OriginalPresent(pSwapChain, SyncInterval, Flags);
      }

      const auto rtvDescriptorSize =
          pD3DDevice->GetDescriptorHandleIncrementSize(
              D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
      D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
          g_pD3DRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

      for (UINT i = 0; i < g_FrameBufferCount; i++) {

        g_FrameContext[i].main_render_target_descriptor = rtvHandle;
        pSwapChain->GetBuffer(
            i, IID_PPV_ARGS(&g_FrameContext[i].main_render_target_resource));
        pD3DDevice->CreateRenderTargetView(
            g_FrameContext[i].main_render_target_resource, nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize;
      }
    }

    {
      ID3D12CommandAllocator *allocator;
      if (pD3DDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator)) !=
          S_OK) {
        return OriginalPresent(pSwapChain, SyncInterval, Flags);
      }

      for (size_t i = 0; i < g_FrameBufferCount; i++) {
        if (pD3DDevice->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&g_FrameContext[i].command_allocator)) != S_OK) {
          return OriginalPresent(pSwapChain, SyncInterval, Flags);
        }
      }

      if (pD3DDevice->CreateCommandList(
              0, D3D12_COMMAND_LIST_TYPE_DIRECT,
              g_FrameContext[0].command_allocator, NULL,
              IID_PPV_ARGS(&g_pD3DCommandList)) != S_OK ||
          g_pD3DCommandList->Close() != S_OK) {
        return OriginalPresent(pSwapChain, SyncInterval, Flags);
      }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(TargetWindow);
    ImGui_ImplDX12_Init(
        pD3DDevice, g_FrameBufferCount, DXGI_FORMAT_R8G8B8A8_UNORM,
        g_pD3DSrvDescHeap,
        g_pD3DSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
        g_pD3DSrvDescHeap->GetGPUDescriptorHandleForHeapStart());

    g_Initialized = true;

    pD3DDevice->Release();
  }

  ImGui_ImplWin32_NewFrame();
  ImGui_ImplDX12_NewFrame();
  ImGui::NewFrame();

  ImGui::ShowDemoWindow();

  FrameContext &currentFrameContext =
      g_FrameContext[pSwapChain->GetCurrentBackBufferIndex()];
  currentFrameContext.command_allocator->Reset();

  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource =
      currentFrameContext.main_render_target_resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  g_pD3DCommandList->Reset(currentFrameContext.command_allocator, nullptr);
  g_pD3DCommandList->ResourceBarrier(1, &barrier);
  g_pD3DCommandList->OMSetRenderTargets(
      1, &currentFrameContext.main_render_target_descriptor, FALSE, nullptr);
  g_pD3DCommandList->SetDescriptorHeaps(1, &g_pD3DSrvDescHeap);
  ImGui::Render();
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pD3DCommandList);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  g_pD3DCommandList->ResourceBarrier(1, &barrier);
  g_pD3DCommandList->Close();

  g_pD3DCommandQueue->ExecuteCommandLists(
      1, (ID3D12CommandList **)&g_pD3DCommandList);
  return OriginalPresent(pSwapChain, SyncInterval, Flags);
}

void HookExecuteCommandLists(ID3D12CommandQueue *queue, UINT NumCommandLists,
                             ID3D12CommandList *ppCommandLists) {
  if (!g_pD3DCommandQueue &&
      queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
    g_pD3DCommandQueue = queue;
  }

  OriginalExecuteCommandLists(queue, NumCommandLists, ppCommandLists);
}

long HookResizeBuffers(IDXGISwapChain3 *pSwapChain, UINT BufferCount,
                       UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                       UINT SwapChainFlags) {
  if (g_Initialized) {
    g_Initialized = false;
    ImGui_ImplWin32_Shutdown();
    ImGui_ImplDX12_Shutdown();
    ImGui::DestroyContext();
  }
  g_pD3DCommandQueue = nullptr;
  g_FrameContext.clear();
  g_pD3DCommandList = nullptr;
  g_pD3DRtvDescHeap = nullptr;
  g_pD3DSrvDescHeap = nullptr;

  return OriginalResizeBuffers(pSwapChain, BufferCount, Width, Height,
                               NewFormat, SwapChainFlags);
}

int Init() {
  Sleep(3000);
  // MessageBoxW(NULL, L"2", NULL, NULL);
  // Sleep(3000);
  HWND window = GetTopWindow(NULL);

  CComPtr<IDXGIFactory> factory;
  CreateDXGIFactory(IID_PPV_ARGS(&factory));

  CComPtr<ID3D12Device> device;
  D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));

  D3D12_COMMAND_QUEUE_DESC queueDesc;
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  queueDesc.Priority = 0;
  queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queueDesc.NodeMask = 0;

  CComPtr<ID3D12CommandQueue> commandQueue;
  device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue),
                             (void **)&commandQueue);

  CComPtr<ID3D12CommandAllocator> commandAllocator;
  device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                 __uuidof(ID3D12CommandAllocator),
                                 (void **)&commandAllocator);

  CComPtr<ID3D12GraphicsCommandList> commandList;
  device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator,
                            NULL, __uuidof(ID3D12GraphicsCommandList),
                            (void **)&commandList);

  DXGI_RATIONAL refreshRate;
  refreshRate.Numerator = 60;
  refreshRate.Denominator = 1;

  DXGI_MODE_DESC bufferDesc;
  bufferDesc.Width = 100;
  bufferDesc.Height = 100;
  bufferDesc.RefreshRate = refreshRate;
  bufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  bufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
  bufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

  DXGI_SAMPLE_DESC sampleDesc;
  sampleDesc.Count = 1;
  sampleDesc.Quality = 0;

  DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
  swapChainDesc.BufferDesc = bufferDesc;
  swapChainDesc.SampleDesc = sampleDesc;
  swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapChainDesc.BufferCount = 2;
  swapChainDesc.OutputWindow = window;
  swapChainDesc.Windowed = 1;
  swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

  CComPtr<IDXGISwapChain> swapChain;
  factory->CreateSwapChain(commandQueue, &swapChainDesc, &swapChain);

  g_MethodsTable = (uint64_t *)::calloc(150, sizeof(uint64_t));
  ::memcpy(g_MethodsTable, *(uint64_t **)(void *)device, 44 * sizeof(uint64_t));
  ::memcpy(g_MethodsTable + 44, *(uint64_t **)(void *)commandQueue,
           19 * sizeof(uint64_t));
  ::memcpy(g_MethodsTable + 44 + 19, *(uint64_t **)(void *)commandAllocator,
           9 * sizeof(uint64_t));
  ::memcpy(g_MethodsTable + 44 + 19 + 9, *(uint64_t **)(void *)commandList,
           60 * sizeof(uint64_t));
  ::memcpy(g_MethodsTable + 44 + 19 + 9 + 60, *(uint64_t **)(void *)swapChain,
           18 * sizeof(uint64_t));

  return 0;
}

int Hook(uint16_t _index, void **_original, void *_function) {
  void *target = (void *)g_MethodsTable[_index];

  *_original = target;
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID &)*_original, _function);
  DetourTransactionCommit();

  return 0;
}

int Unhook(uint16_t _index, void **_original, void *_function) {
  void *target = (void *)g_MethodsTable[_index];

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourDetach(&(PVOID &)*_original, _function);
  DetourTransactionCommit();

  return 0;
}

int InstallHooks() {

  DetourRestoreAfterWith();

  Hook(54, (void **)&OriginalExecuteCommandLists, HookExecuteCommandLists);
  Hook(140, (void **)&OriginalPresent, HookPresent);
  Hook(145, (void **)&OriginalResizeBuffers, HookResizeBuffers);

  return 0;
}

int RemoveHooks() {
  Unhook(54, (void **)&OriginalExecuteCommandLists, HookExecuteCommandLists);
  Unhook(140, (void **)&OriginalPresent, HookPresent);
  Unhook(145, (void **)&OriginalResizeBuffers, HookResizeBuffers);

  if (TargetWindow && OriginalWndProc) {
    SetWindowLongPtr(TargetWindow, GWLP_WNDPROC,
                     (__int3264)(LONG_PTR)OriginalWndProc);
  }

  if (g_Initialized) {
    g_Initialized = false;
    ImGui_ImplWin32_Shutdown();
    ImGui_ImplDX12_Shutdown();
  }
  g_pD3DCommandQueue = nullptr;
  g_FrameContext.clear();
  g_pD3DCommandList = nullptr;
  g_pD3DRtvDescHeap = nullptr;
  g_pD3DSrvDescHeap = nullptr;

  ImGui::DestroyContext();

  return 0;
}
