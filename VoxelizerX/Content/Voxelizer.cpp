#include "ObjLoader.h"
#include "Voxelizer.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

Voxelizer::Voxelizer(const Device &device, const GraphicsCommandList &commandList) :
	m_device(device),
	m_commandList(commandList),
	m_vertexStride(0),
	m_numIndices(0)
{
	m_pipelinePool.SetDevice(device);
	m_descriptorTablePool.SetDevice(device);
}

Voxelizer::~Voxelizer()
{
}

void Voxelizer::Init(uint32_t width, uint32_t height, Resource &vbUpload, Resource &ibUpload,
	const char *fileName)
{
	m_viewport.x = static_cast<float>(width);
	m_viewport.y = static_cast<float>(height);

	// Create shaders
	createShaders();

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return;

	//createInputLayout();
	createVB(objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), vbUpload);
	createIB(objLoader.GetNumIndices(), objLoader.GetIndices(), ibUpload);

	// Extract boundary
	const auto center = objLoader.GetCenter();
	m_bound = XMFLOAT4(center.x, center.y, center.z, objLoader.GetRadius());

	m_numLevels = max(static_cast<uint32_t>(log2(GRID_SIZE)), 1);
	createCBs();

	m_grid = make_unique<Texture3D>(m_device);
	m_grid->Create(GRID_SIZE, GRID_SIZE, GRID_SIZE, DXGI_FORMAT_R10G10B10A2_UNORM, BIND_PACKED_UAV);

	m_KBufferDepth = make_unique<Texture2D>(m_device);
	m_KBufferDepth->Create(GRID_SIZE, GRID_SIZE, DXGI_FORMAT_R32_UINT, static_cast<uint32_t>(GRID_SIZE * DEPTH_SCALE),
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
}

void Voxelizer::UpdateFrame(CXMVECTOR eyePt, CXMMATRIX viewProj)
{
	// General matrices
	const auto world = XMMatrixScaling(m_bound.w, m_bound.w, m_bound.w) *
		XMMatrixTranslation(m_bound.x, m_bound.y, m_bound.z);
	const auto worldI = XMMatrixInverse(nullptr, world);
	const auto worldViewProj = world * viewProj;
	const auto pCbMatrices = reinterpret_cast<CBMatrices*>(m_cbMatrices->Map());
	pCbMatrices->worldViewProj = XMMatrixTranspose(worldViewProj);
	pCbMatrices->world = world;
	pCbMatrices->worldIT = XMMatrixIdentity();
	
	// Screen space matrices
	const auto pCbPerObject = reinterpret_cast<CBPerObject*>(m_cbPerObject->Map());
	pCbPerObject->localSpaceLightPt = XMVector3TransformCoord(XMVectorSet(10.0f, 45.0f, 75.0f, 0.0f), worldI);
	pCbPerObject->localSpaceEyePt = XMVector3TransformCoord(eyePt, worldI);

	const auto mToScreen = XMMATRIX
	(
		0.5f * m_viewport.x, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f * m_viewport.y, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f * m_viewport.x, 0.5f * m_viewport.y, 0.0f, 1.0f
	);
	const auto localToScreen = XMMatrixMultiply(worldViewProj, mToScreen);
	const auto screenToLocal = XMMatrixInverse(nullptr, localToScreen);
	pCbPerObject->screenToLocal = XMMatrixTranspose(screenToLocal);

	// Per-frame data
	const auto pCbPerFrame = reinterpret_cast<CBPerFrame*>(m_cbPerFrame->Map());
	XMStoreFloat4(&pCbPerFrame->eyePos, eyePt);
}

void Voxelizer::Render(const RenderTargetTable &rtvs, const Descriptor &dsv)
{
	voxelize();
	renderBoxArray(rtvs, dsv);
}

void Voxelizer::createShaders()
{
	m_shaderPool.CreateShader(Shader::Stage::VS, VS_TRI_PROJ, L"VSTriProj.cso");
	m_shaderPool.CreateShader(Shader::Stage::VS, VS_BOX_ARRAY, L"VSBoxArray.cso");

	m_shaderPool.CreateShader(Shader::Stage::PS, PS_TRI_PROJ, L"PSTriProj.cso");
	m_shaderPool.CreateShader(Shader::Stage::PS, PS_SIMPLE, L"PSSimple.cso");
}

void Voxelizer::createInputLayout()
{
	const auto offset = D3D12_APPEND_ALIGNED_ELEMENT;

	// Define the vertex input layout.
	InputElementTable inputElementDescs =
	{
		{ "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,		D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",		0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offset,	D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	m_inputLayout = m_pipelinePool.CreateInputLayout(inputElementDescs);
}

void Voxelizer::createVB(uint32_t numVert, uint32_t stride, const uint8_t *pData, Resource &vbUpload)
{
	m_vertexStride = stride;
	m_vertexBuffer = make_unique<RawBuffer>(m_device);
	m_vertexBuffer->Create(stride * numVert, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_COPY_DEST);
	m_vertexBuffer->Upload(m_commandList, vbUpload, pData, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void Voxelizer::createIB(uint32_t numIndices, const uint32_t *pData, Resource &ibUpload)
{
	m_numIndices = numIndices;
	m_indexbuffer = make_unique<RawBuffer>(m_device);
	m_indexbuffer->Create(sizeof(uint32_t) * numIndices, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_COPY_DEST);
	m_indexbuffer->Upload(m_commandList, ibUpload, pData, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void Voxelizer::createCBs()
{
	// Common CBs
	{
		m_cbMatrices = make_unique<ConstantBuffer>(m_device);
		m_cbMatrices->Create(sizeof(XMMATRIX) * 4 * 32, sizeof(CBMatrices));
		
		m_cbPerFrame = make_unique<ConstantBuffer>(m_device);
		m_cbPerFrame->Create(sizeof(XMFLOAT4) * 512, sizeof(CBPerFrame));

		m_cbPerObject = make_unique<ConstantBuffer>(m_device);
		m_cbPerObject->Create(sizeof(XMMATRIX) * 2 * 64, sizeof(CBPerObject));
	}

	// Immutable CBs
	{
		m_cbBound = make_unique<ConstantBuffer>(m_device);
		m_cbBound->Create(256, sizeof(XMFLOAT4));

		const auto pCbBound = reinterpret_cast<XMFLOAT4*>(m_cbBound->Map());
		*pCbBound = m_bound;
		m_cbBound->Unmap();
	}

	m_cbPerMipLevels.resize(m_numLevels);
	for (auto i = 0u; i < m_numLevels; ++i)
	{
		auto &cb = m_cbPerMipLevels[i];
		const auto gridSize = static_cast<float>(GRID_SIZE >> i);
		cb = make_unique<ConstantBuffer>(m_device);
		cb->Create(256, sizeof(XMFLOAT4));

		const auto pCbData = reinterpret_cast<float*>(cb->Map());
		*pCbData = gridSize;
		cb->Unmap();
	}
}

void Voxelizer::voxelize(bool depthPeel, uint8_t mipLevel)
{
	// Get UAVs
	Util::DescriptorTable utilUavGridTable;
	utilUavGridTable.SetDescriptors(0, 1, &m_grid->GetUAV());
	const auto uavGridTable = utilUavGridTable.GetCbvSrvUavTable(m_descriptorTablePool);

	Util::DescriptorTable utilUavKBufferTable;
	utilUavKBufferTable.SetDescriptors(0, 1, &m_KBufferDepth->GetUAV());
	const auto uavKBufferTable = utilUavKBufferTable.GetCbvSrvUavTable(m_descriptorTablePool);

	// Get SRVs
	const Descriptor srvs[] = { m_indexbuffer->GetSRV(), m_vertexBuffer->GetSRV() };
	Util::DescriptorTable utilSrvTable;
	utilSrvTable.SetDescriptors(0, _countof(srvs), srvs);
	const auto srvTable = utilSrvTable.GetCbvSrvUavTable(m_descriptorTablePool);

	// Get CBVs
	Util::DescriptorTable utilCbvPerObjectTable;
	utilCbvPerObjectTable.SetDescriptors(0, 1, &m_cbBound->GetCBV());
	const auto cbvPerObjectTable = utilCbvPerObjectTable.GetCbvSrvUavTable(m_descriptorTablePool);

	Util::DescriptorTable utilCbvPerMipLevelTable;
	utilCbvPerMipLevelTable.SetDescriptors(0, 1, &m_cbPerMipLevels[mipLevel]->GetCBV());
	const auto cbvPerMipLevel = utilCbvPerMipLevelTable.GetCbvSrvUavTable(m_descriptorTablePool);

	// Get pipeline layout
	Util::PipelineLayout utilPipelineLayout;
	utilPipelineLayout.SetRange(0, DescriptorType::UAV, depthPeel ? 2 : 1, 0);
	utilPipelineLayout.SetRange(1, DescriptorType::SRV, _countof(srvs), 0);
	utilPipelineLayout.SetRange(2, DescriptorType::CBV, 1, 0);
	utilPipelineLayout.SetRange(3, DescriptorType::CBV, 1, 1);
	utilPipelineLayout.SetRange(4, DescriptorType::CBV, 1, 0);
	utilPipelineLayout.SetShaderStage(0, Shader::Stage::PS);
	utilPipelineLayout.SetShaderStage(1, Shader::Stage::VS);
	utilPipelineLayout.SetShaderStage(2, Shader::Stage::VS);
	utilPipelineLayout.SetShaderStage(3, Shader::Stage::VS);
	utilPipelineLayout.SetShaderStage(4, Shader::Stage::PS);
	const auto pipelineLayout = utilPipelineLayout.GetPipelineLayout(m_pipelinePool, D3D12_ROOT_SIGNATURE_FLAG_NONE);

	// Get pipeline
	Graphics::State state;
	state.SetPipelineLayout(pipelineLayout);
	state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, VS_TRI_PROJ));
	state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, PS_TRI_PROJ));
	state.DSSetState(Graphics::DepthStencilPreset::DEPTH_STENCIL_NONE, m_pipelinePool);
	state.IASetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
	state.RSSetState(Graphics::RasterizerPreset::CULL_NONE, m_pipelinePool);
	state.OMSetNumRenderTargets(0);
	//state.OMSetRTVFormat(0, DXGI_FORMAT_R8G8B8A8_UNORM);
	const auto pipelineState = state.GetPipeline(m_pipelinePool);

	static auto warmUp = true;
	if (warmUp) warmUp = false;
	else
	{
		// Set pipeline state
		m_commandList->SetPipelineState(pipelineState.Get());
		m_commandList->SetGraphicsRootSignature(pipelineLayout.Get());

		// Set descriptor tables
		m_grid->Barrier(m_commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		m_commandList->SetDescriptorHeaps(1, m_descriptorTablePool.GetCbvSrvUavPool().GetAddressOf());
		m_commandList->SetGraphicsRootDescriptorTable(0, *uavGridTable);
		m_commandList->SetGraphicsRootDescriptorTable(1, *srvTable);
		m_commandList->SetGraphicsRootDescriptorTable(2, *cbvPerObjectTable);
		m_commandList->SetGraphicsRootDescriptorTable(3, *cbvPerMipLevel);
		m_commandList->SetGraphicsRootDescriptorTable(4, *cbvPerMipLevel);

		// Set viewport
		const auto gridSize = GRID_SIZE >> mipLevel;
		const auto fGridSize = static_cast<float>(gridSize);
		CD3DX12_VIEWPORT viewport(0.0f, 0.0f, fGridSize, fGridSize);
		CD3DX12_RECT scissorRect(0, 0, gridSize, gridSize);
		m_commandList->RSSetViewports(1, &viewport);
		m_commandList->RSSetScissorRects(1, &scissorRect);

		// Record commands.
		m_commandList->ClearUnorderedAccessViewUint(*uavGridTable, m_grid->GetUAV(), m_grid->GetResource().Get(), XMVECTORU32{ 0 }.u, 0, nullptr);
		if (depthPeel) m_commandList->ClearUnorderedAccessViewUint(*uavKBufferTable, m_KBufferDepth->GetUAV(),
			m_KBufferDepth->GetResource().Get(), XMVECTORU32{ UINT32_MAX }.u, 0, nullptr);

		m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_commandList->DrawInstanced(3, m_numIndices / 3, 0, 0);
	}
}

void Voxelizer::renderBoxArray(const RenderTargetTable &rtvs, const Descriptor &dsv)
{
	// Get SRV
	Util::DescriptorTable utilSrvTable;
	utilSrvTable.SetDescriptors(0, 1, &m_grid->GetSRV());
	const auto srvTable = utilSrvTable.GetCbvSrvUavTable(m_descriptorTablePool);

	// Get CBVs
	Util::DescriptorTable utilCbvTable;
	utilCbvTable.SetDescriptors(0, 1, &m_cbMatrices->GetCBV());
	const auto cbvTable = utilCbvTable.GetCbvSrvUavTable(m_descriptorTablePool);

	// Get pipeline layout
	Util::PipelineLayout utilPipelineLayout;
	utilPipelineLayout.SetRange(0, DescriptorType::SRV, 1, 0);
	utilPipelineLayout.SetRange(1, DescriptorType::CBV, 1, 0);
	utilPipelineLayout.SetShaderStage(0, Shader::Stage::VS);
	utilPipelineLayout.SetShaderStage(1, Shader::Stage::VS);
	const auto pipelineLayout = utilPipelineLayout.GetPipelineLayout(m_pipelinePool, D3D12_ROOT_SIGNATURE_FLAG_NONE);

	// Get pipeline
	Graphics::State state;
	state.SetPipelineLayout(pipelineLayout);
	state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, VS_BOX_ARRAY));
	state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, PS_SIMPLE));
	state.IASetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
	state.OMSetNumRenderTargets(1);
	state.OMSetRTVFormat(0, DXGI_FORMAT_R8G8B8A8_UNORM);
	state.OMSetDSVFormat(DXGI_FORMAT_D24_UNORM_S8_UINT);
	const auto pipelineState = state.GetPipeline(m_pipelinePool);

	static auto warmUp = true;
	if (warmUp) warmUp = false;
	else
	{
		// Set pipeline state
		m_commandList->SetPipelineState(pipelineState.Get());
		m_commandList->SetGraphicsRootSignature(pipelineLayout.Get());

		// Set descriptor tables
		m_grid->Barrier(m_commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		m_commandList->SetDescriptorHeaps(1, m_descriptorTablePool.GetCbvSrvUavPool().GetAddressOf());
		m_commandList->SetGraphicsRootDescriptorTable(0, *srvTable);
		m_commandList->SetGraphicsRootDescriptorTable(1, *cbvTable);

		// Set viewport
		const auto gridSize = GRID_SIZE >> SHOW_MIP;
		CD3DX12_VIEWPORT viewport(0.0f, 0.0f, m_viewport.x, m_viewport.y);
		CD3DX12_RECT scissorRect(0, 0, static_cast<long>(m_viewport.x), static_cast<long>(m_viewport.y));
		m_commandList->RSSetViewports(1, &viewport);
		m_commandList->RSSetScissorRects(1, &scissorRect);

		m_commandList->OMSetRenderTargets(1, rtvs.get(), FALSE, &dsv);

		// Record commands.
		m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		m_commandList->DrawInstanced(4, 6 * gridSize * gridSize * gridSize, 0, 0);
	}
}