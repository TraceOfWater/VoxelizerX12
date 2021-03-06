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

#pragma once

#include "StepTimer.h"
#include "Voxelizer.h"

using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().

class VoxelizerX : public DXFramework
{
public:
	VoxelizerX(uint32_t width, uint32_t height, std::wstring name);

	virtual void OnInit();
	virtual void OnUpdate();
	virtual void OnRender();
	virtual void OnDestroy();

	virtual void OnKeyUp(uint8_t /*key*/);
	virtual void OnLButtonDown(float posX, float posY);
	virtual void OnLButtonUp(float posX, float posY);
	virtual void OnMouseMove(float posX, float posY);
	virtual void OnMouseWheel(float deltaZ, float posX, float posY);
	virtual void OnMouseLeave();

private:
	XUSG::DescriptorTableCache m_descriptorTableCache;

	XUSG::SwapChain			m_swapChain;
	XUSG::CommandAllocator	m_commandAllocators[Voxelizer::FrameCount];
	XUSG::CommandQueue		m_commandQueue;

	XUSG::Device			m_device;
	XUSG::RenderTarget		m_renderTargets[Voxelizer::FrameCount];
	XUSG::CommandList		m_commandList;
	
	// App resources.
	std::unique_ptr<Voxelizer> m_voxelizer;
	XUSG::RenderTargetTable	m_rtvTables[Voxelizer::FrameCount];
	XUSG::DepthStencil	m_depth;
	XMFLOAT4X4			m_proj;
	XMFLOAT4X4	m_view;
	XMFLOAT3	m_focusPt;
	XMFLOAT3	m_eyePt;

	// Synchronization objects.
	uint32_t	m_frameIndex;
	HANDLE		m_fenceEvent;
	XUSG::Fence	m_fence;
	uint64_t	m_fenceValues[Voxelizer::FrameCount];

	// Application state
	bool		m_solid;
	bool		m_showFPS;
	bool		m_pausing;
	StepTimer	m_timer;
	Voxelizer::Method m_voxMethod;
	std::wstring m_voxMethodDesc;
	std::wstring m_solidDesc;

	// User camera interactions
	bool m_tracking;
	XMFLOAT2 m_mousePt;

	void LoadPipeline();
	void LoadAssets();

	void PopulateCommandList();
	void WaitForGpu();
	void MoveToNextFrame();
	double CalculateFrameStats(float *fTimeStep = nullptr);

	static const wchar_t *VoxMethodDescs[];
	static const wchar_t *SolidDescs[];
};
