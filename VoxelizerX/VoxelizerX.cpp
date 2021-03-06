//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "VoxelizerX.h"

using namespace std;
using namespace XUSG;

const wchar_t *VoxelizerX::VoxMethodDescs[] =
{
	L"Axis-aligned projection of max projected area",
	L"Tessellation for axis-aligned projection view of max projected area",
	L"Union of 3 axis-aligned projection views"
};

const wchar_t *VoxelizerX::SolidDescs[] =
{
	L"Render surface voxels as box array",
	L"Render solid voxels with raycasting"
};

VoxelizerX::VoxelizerX(uint32_t width, uint32_t height, std::wstring name) :
	DXFramework(width, height, name),
	m_frameIndex(0),
	m_solid(false),
	m_showFPS(true),
	m_pausing(false),
	m_tracking(false),
	m_voxMethod(Voxelizer::TRI_PROJ),
	m_voxMethodDesc(VoxMethodDescs[m_voxMethod]),
	m_solidDesc(SolidDescs[m_solid])
{
}

void VoxelizerX::OnInit()
{
	LoadPipeline();
	LoadAssets();
}

// Load the rendering pipeline dependencies.
void VoxelizerX::LoadPipeline()
{
	auto dxgiFactoryFlags = 0u;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		com_ptr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	com_ptr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	if (m_useWarpDevice)
	{
		com_ptr<IDXGIAdapter> warpAdapter;
		ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

		ThrowIfFailed(D3D12CreateDevice(
			warpAdapter.get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&m_device)
			));
	}
	else
	{
		com_ptr<IDXGIAdapter1> hardwareAdapter;
		GetHardwareAdapter(factory.get(), &hardwareAdapter);

		ThrowIfFailed(D3D12CreateDevice(
			hardwareAdapter.get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&m_device)
			));
	}

	// Describe and create the command queue.
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

	ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = Voxelizer::FrameCount;
	swapChainDesc.Width = m_width;
	swapChainDesc.Height = m_height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	com_ptr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		m_commandQueue.get(),		// Swap chain needs the queue so that it can force a flush on it.
		Win32Application::GetHwnd(),
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain
		));

	// This sample does not support fullscreen transitions.
	ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain.As(&m_swapChain));
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	m_descriptorTableCache.SetDevice(m_device);

	// Create frame resources.
	// Create a RTV and a command allocator for each frame.
	for (auto n = 0u; n < Voxelizer::FrameCount; n++)
	{
		m_renderTargets[n].CreateFromSwapChain(m_device, m_swapChain, n);

		Util::DescriptorTable rtvTable;
		rtvTable.SetDescriptors(0, 1, &m_renderTargets[n].GetRTV());
		m_rtvTables[n] = rtvTable.GetRtvTable(m_descriptorTableCache);

		ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[n])));
	}

	// Create a DSV
	m_depth.Create(m_device, m_width, m_height, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}

// Load the sample assets.
void VoxelizerX::LoadAssets()
{
	// Create the command list.
	ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_commandAllocators[m_frameIndex].get(), nullptr, IID_PPV_ARGS(&m_commandList.GetCommandList())));

	m_voxelizer = make_unique<Voxelizer>(m_device, m_commandList);
	if (!m_voxelizer) ThrowIfFailed(E_FAIL);

	Resource vbUpload, ibUpload;
	if (!m_voxelizer->Init(m_width, m_height, m_renderTargets[0].GetResource()->GetDesc().Format,
		m_depth.GetResource()->GetDesc().Format, vbUpload, ibUpload))
		ThrowIfFailed(E_FAIL);

	// Close the command list and execute it to begin the initial GPU setup.
	ThrowIfFailed(m_commandList.Close());
	ID3D12CommandList *const ppCommandLists[] = { m_commandList.GetCommandList().get() };
	m_commandQueue->ExecuteCommandLists(static_cast<uint32_t>(size(ppCommandLists)), ppCommandLists);

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		ThrowIfFailed(m_device->CreateFence(m_fenceValues[m_frameIndex]++, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
		
		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForGpu();
	}

	// Projection
	const auto aspectRatio = m_width / static_cast<float>(m_height);
	const auto proj = XMMatrixPerspectiveFovLH(g_FOVAngleY, aspectRatio, g_zNear, g_zFar);
	XMStoreFloat4x4(&m_proj, proj);

	// View initialization
	m_focusPt = XMFLOAT3(0.0f, 4.0f, 0.0f);
	m_eyePt = XMFLOAT3(-8.0f, 12.0f, 14.0f);
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMMatrixLookAtLH(eyePt, focusPt, XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f));
	XMStoreFloat4x4(&m_view, view);
}

// Update frame-based values.
void VoxelizerX::OnUpdate()
{
	// Timer
	static auto time = 0.0, pauseTime = 0.0;

	m_timer.Tick();
	const auto totalTime = CalculateFrameStats();
	pauseTime = m_pausing ? totalTime - time : pauseTime;
	time = totalTime - pauseTime;

	// View
	const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMLoadFloat4x4(&m_view); 
	const auto proj = XMLoadFloat4x4(&m_proj);
	m_voxelizer->UpdateFrame(m_frameIndex, eyePt, view * proj);
}

// Render the scene.
void VoxelizerX::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	ID3D12CommandList *const ppCommandLists[] = { m_commandList.GetCommandList().get() };
	m_commandQueue->ExecuteCommandLists(static_cast<uint32_t>(size(ppCommandLists)), ppCommandLists);

	// Present the frame.
	ThrowIfFailed(m_swapChain->Present(0, 0));

	MoveToNextFrame();
}

void VoxelizerX::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	WaitForGpu();

	CloseHandle(m_fenceEvent);
}

// User hot-key interactions.
void VoxelizerX::OnKeyUp(uint8_t key)
{
	switch (key)
	{
	case 0x20:	// case VK_SPACE:
		m_pausing = !m_pausing;
		break;
	case 0x70:	//case VK_F1:
		m_showFPS = !m_showFPS;
		break;
	case 'V':
		m_voxMethod = static_cast<Voxelizer::Method>((m_voxMethod + 1) % Voxelizer::NUM_METHOD);
		m_voxMethodDesc = VoxMethodDescs[m_voxMethod];
		break;
	case 'S':
		m_solid = !m_solid;
		m_solidDesc = SolidDescs[m_solid];
		break;
	}
}

// User camera interactions.
void VoxelizerX::OnLButtonDown(float posX, float posY)
{
	m_tracking = true;
	m_mousePt = XMFLOAT2(posX, posY);
}

void VoxelizerX::OnLButtonUp(float posX, float posY)
{
	m_tracking = false;
}

void VoxelizerX::OnMouseMove(float posX, float posY)
{
	if (m_tracking)
	{
		const auto dPos = XMFLOAT2(m_mousePt.x - posX, m_mousePt.y - posY);
		
		XMFLOAT2 radians;
		radians.x = XM_2PI * dPos.y / m_height;
		radians.y = XM_2PI * dPos.x / m_width;

		const auto focusPt = XMLoadFloat3(&m_focusPt);
		auto eyePt = XMLoadFloat3(&m_eyePt);

		const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
		auto transform = XMMatrixTranslation(0.0f, 0.0f, -len);
		transform *= XMMatrixRotationRollPitchYaw(radians.x, radians.y, 0.0f);
		transform *= XMMatrixTranslation(0.0f, 0.0f, len);

		const auto view = XMLoadFloat4x4(&m_view) * transform;
		const auto viewInv = XMMatrixInverse(nullptr, view);
		eyePt = viewInv.r[3];

		XMStoreFloat3(&m_eyePt, eyePt);
		XMStoreFloat4x4(&m_view, view);

		m_mousePt = XMFLOAT2(posX, posY);
	}
}

void VoxelizerX::OnMouseWheel(float deltaZ, float posX, float posY)
{
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	auto eyePt = XMLoadFloat3(&m_eyePt);

	const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
	const auto transform = XMMatrixTranslation(0.0f, 0.0f, -len * deltaZ / 16.0f);
	
	const auto view = XMLoadFloat4x4(&m_view) * transform;
	const auto viewInv = XMMatrixInverse(nullptr, view);
	eyePt = viewInv.r[3];

	XMStoreFloat3(&m_eyePt, eyePt);
	XMStoreFloat4x4(&m_view, view);
}

void VoxelizerX::OnMouseLeave()
{
	m_tracking = false;
}

void VoxelizerX::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	ThrowIfFailed(m_commandList.Reset(m_commandAllocators[m_frameIndex], nullptr));

	// Indicate that the back buffer will be used as a render target.
	m_renderTargets[m_frameIndex].Barrier(m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

	// Record commands.
	if (!m_solid)
	{
		const float clearColor[] = { CLEAR_COLOR, 0.0f };
		m_commandList.ClearRenderTargetView(*m_rtvTables[m_frameIndex], clearColor);
	}
	m_commandList.ClearDepthStencilView(m_depth.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, 1.0f);

	// Voxelizer rendering
	m_voxelizer->Render(m_solid, m_voxMethod, m_frameIndex, m_rtvTables[m_frameIndex], m_depth.GetDSV());

	// Indicate that the back buffer will now be used to present.
	m_renderTargets[m_frameIndex].Barrier(m_commandList, D3D12_RESOURCE_STATE_PRESENT);

	ThrowIfFailed(m_commandList.Close());
}

// Wait for pending GPU work to complete.
void VoxelizerX::WaitForGpu()
{
	// Schedule a Signal command in the queue.
	ThrowIfFailed(m_commandQueue->Signal(m_fence.get(), m_fenceValues[m_frameIndex]));

	// Wait until the fence has been processed.
	ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
	WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);

	// Increment the fence value for the current frame.
	m_fenceValues[m_frameIndex]++;
}

// Prepare to render the next frame.
void VoxelizerX::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const auto currentFenceValue = m_fenceValues[m_frameIndex];
	ThrowIfFailed(m_commandQueue->Signal(m_fence.get(), currentFenceValue));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}

	// Set the fence value for the next frame.
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

double VoxelizerX::CalculateFrameStats(float *pTimeStep)
{
	static int frameCnt = 0;
	static double elapsedTime = 0.0;
	static double previousTime = 0.0;
	const auto totalTime = m_timer.GetTotalSeconds();
	++frameCnt;

	const auto timeStep = static_cast<float>(totalTime - elapsedTime);

	// Compute averages over one second period.
	if ((totalTime - elapsedTime) >= 1.0f)
	{
		float fps = static_cast<float>(frameCnt) / timeStep;	// Normalize to an exact second.

		frameCnt = 0;
		elapsedTime = totalTime;

		wstringstream windowText;
		windowText << L"    fps: ";
		if (m_showFPS) windowText << setprecision(2) << fixed << fps;
		else windowText << L"[F1]";
		windowText << L"    [V] " << m_voxMethodDesc << L"    [S] " << m_solidDesc;
		SetCustomWindowText(windowText.str().c_str());
	}

	if (pTimeStep) *pTimeStep = static_cast<float>(totalTime - previousTime);
	previousTime = totalTime;

	return totalTime;
}
