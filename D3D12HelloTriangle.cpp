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

#include "stdafx.h"
#include "D3D12HelloTriangle.h"

#include <stdexcept>

#include <dxr/DXRHelper.h>
#include <dxr/nv_helpers_dx12/BottomLevelASGenerator.h>
#include <dxr/nv_helpers_dx12/RaytracingPipelineGenerator.h>
#include <dxr/nv_helpers_dx12/RootSignatureGenerator.h>
#include <dxr/nv_helpers_dx12/Manipulator.h>

#include <glm/gtc/type_ptr.hpp>

#include "Windowsx.h"

D3D12HelloTriangle::D3D12HelloTriangle(UINT width, UINT height, std::wstring name) :
	DXSample(width, height, name),
	m_frameIndex(0),
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
	m_rtvDescriptorSize(0)
{
}

void D3D12HelloTriangle::OnInit()
{
	// Camera
	nv_helpers_dx12::CameraManip.setWindowSize(GetWidth(), GetHeight());
	nv_helpers_dx12::CameraManip.setLookat(glm::vec3(1.5f, 1.5f, 1.5f), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));

	LoadPipeline();
	LoadAssets();
	CheckRaytracingSupport();

	// Setup the acceleration structures (AS) for raytracing. When setting up
	// geometry, each bottom-level AS has its own transform matrix.
	CreateAccelerationStructures();

	// Command lists are created in the recording state, but there is nothing
	// to record yet. The main loop expects it to be closed, so close it now.
	ThrowIfFailed(m_commandList->Close());

	// Create the raytracing pipeline, associating the shader code to symbol names
	// and to their root signatures, and defining the amount of memory carried by
	// rays (ray payload)
	CreateRaytracingPipeline();

	CreatePerInstanceConstantBuffers();

	// Allocate the buffer storing the raytracing output, with the same dimensions
	// as the target image
	CreateRaytracingOutputBuffer();

	// Create a buffer to store the modelview and perspective camera matrices
	CreateCameraBuffer();

	// Create the buffer containing the raytracing result (always output in a
	// UAV), and create the heap referencing the resources used by the raytracing,
	// such as the acceleration structure
	CreateShaderResourceHeap();

	// Create the shader binding table and indicating which shaders
	// are invoked for each instance in the AS
	CreateShaderBindingTable();
}

// Load the rendering pipeline dependencies.
void D3D12HelloTriangle::LoadPipeline()
{
	UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	ComPtr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	if (m_useWarpDevice)
	{
		ComPtr<IDXGIAdapter> warpAdapter;
		ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

		ThrowIfFailed(D3D12CreateDevice(
			warpAdapter.Get(),
			D3D_FEATURE_LEVEL_12_1,
			IID_PPV_ARGS(&m_device)
			));
	}
	else
	{
		ComPtr<IDXGIAdapter1> hardwareAdapter;
		GetHardwareAdapter(factory.Get(), &hardwareAdapter);

		ThrowIfFailed(D3D12CreateDevice(
			hardwareAdapter.Get(),
			D3D_FEATURE_LEVEL_12_1,
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
	swapChainDesc.BufferCount = FrameCount;
	swapChainDesc.Width = m_width;
	swapChainDesc.Height = m_height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	ComPtr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		m_commandQueue.Get(),		// Swap chain needs the queue so that it can force a flush on it.
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

	// Create descriptor heaps.
	{
		// Describe and create a render target view (RTV) descriptor heap.
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
		rtvHeapDesc.NumDescriptors = FrameCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

		m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}

	// Create frame resources.
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

		// Create a RTV for each frame.
		for (UINT n = 0; n < FrameCount; n++)
		{
			ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
			m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
			rtvHandle.Offset(1, m_rtvDescriptorSize);
		}
	}

	ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));

	// The original sample does not support depth buffering, so we need to allocate a depth buffer,
	// and later bind it before rasterization
	CreateDepthBuffer();
}

// Load the sample assets.
void D3D12HelloTriangle::LoadAssets()
{

	{
		CD3DX12_ROOT_PARAMETER constantParameter;
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
		constantParameter.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
		rootSignatureDesc.Init(1, &constantParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		ComPtr<ID3DBlob> signature;
		ComPtr<ID3DBlob> error;
		ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
		ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
	}

	// Create the pipeline state, which includes compiling and loading shaders.
	{
		ComPtr<ID3DBlob> vertexShader;
		ComPtr<ID3DBlob> pixelShader;

#if defined(_DEBUG)
		// Enable better shader debugging with the graphics debugging tools.
		UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
		UINT compileFlags = 0;
#endif

		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(), nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr));
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(), nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr));

		// Define the vertex input layout.
		D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		// Describe and create the graphics pipeline state object (PSO).
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
		psoDesc.pRootSignature = m_rootSignature.Get();
		psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
		psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
		psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		psoDesc.DepthStencilState.DepthEnable = FALSE;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		psoDesc.SampleDesc.Count = 1;

		// Add support for depth testing, using a 32-bit floating-point depth buffer
		psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

		ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
	}

	// Create the command list.
	ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), m_pipelineState.Get(), IID_PPV_ARGS(&m_commandList)));

	// Create the vertex buffer.
	{

		// Create Plane Buffer
		CreatePlaneVB();

		// Create Cube Buffer
		CreateCubeVB();
	}

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
		m_fenceValue = 1;

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}

		WaitForPreviousFrame();
	}
}

// Update frame-based values.
void D3D12HelloTriangle::OnUpdate()
{

	m_time++;
	m_instances[0].second = XMMatrixRotationAxis({ 0.f, 1.f, 0.f }, static_cast<float>(m_time) / 50.0f) * XMMatrixTranslation(0.f, 0.1f * cosf(m_time / 20.f), 0.f);

	UpdateCameraBuffer();
}

// Render the scene.
void D3D12HelloTriangle::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	// Present the frame.
	ThrowIfFailed(m_swapChain->Present(1, 0));

	WaitForPreviousFrame();
}

void D3D12HelloTriangle::OnDestroy()
{
	WaitForPreviousFrame();
	CloseHandle(m_fenceEvent);
}

void D3D12HelloTriangle::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	ThrowIfFailed(m_commandAllocator->Reset());

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));

	// Set necessary state.
	m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
	m_commandList->RSSetViewports(1, &m_viewport);
	m_commandList->RSSetScissorRects(1, &m_scissorRect);

	// Indicate that the back buffer will be used as a render target.
	m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
	// Bind the depth buffer as a render target
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
	m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

	if (m_raster) {
		// Clear depth buffer
		m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
		//Perspective Camera
		std::vector< ID3D12DescriptorHeap* > heaps = { m_constHeap.Get() };
		m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
		// set the root descriptor table 0 to the constant buffer descriptor heap 
		m_commandList->SetGraphicsRootDescriptorTable(0, m_constHeap->GetGPUDescriptorHandleForHeapStart());
		m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
		m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		//m_commandList->IASetVertexBuffers(0, 1, &m_tetrahoidBufferView);
		//m_commandList->IASetIndexBuffer(&m_indexBufferView);
		//m_commandList->DrawIndexedInstanced(12, 1, 0, 0, 0);

		m_commandList->IASetVertexBuffers(0, 1, &m_cubeBufferView);
		m_commandList->DrawInstanced(6 * 6, 1, 0, 0);

		m_commandList->IASetVertexBuffers(0, 1, &m_planeBufferView);
		m_commandList->DrawInstanced(6, 1, 0, 0);
	}
	else {
		CreateTopLevelAS(m_instances, true);
		/*const float clearColor[] = { 0.6f, 0.8f, 0.4f, 1.0f };
		m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);*/
		// Bind the descriptor heap giving access to the top-level acceleration
		// structure, as well as the raytracing output
		std::vector<ID3D12DescriptorHeap*> heaps = { m_srvUavHeap.Get() };
		m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());

		// On the last frame, the raytracing output was used as a copy source, to
		// copy its contents into the render target. Now we need to transition it to
		// a UAV so that the shaders can write in it.
		CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(m_outputResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		m_commandList->ResourceBarrier(1, &transition);

		// Setup the raytracing task
		D3D12_DISPATCH_RAYS_DESC desc = {};
		// The layout of the SBT is as follows: ray generation shader, miss
		// shaders, hit groups. As described in the CreateShaderBindingTable method,
		// all SBT entries of a given type have the same size to allow a fixed stride.
		// The ray generation shaders are always at the beginning of the SBT.
		uint32_t rayGenerationSectionSizeInBytes = m_sbtHelper.GetRayGenSectionSize();
		desc.RayGenerationShaderRecord.StartAddress = m_sbtStorage->GetGPUVirtualAddress();
		desc.RayGenerationShaderRecord.SizeInBytes = rayGenerationSectionSizeInBytes;

		// The miss shaders are in the second SBT section, right after the ray
		// generation shader. We have one miss shader for the camera rays and one
		// for the shadow rays, so this section has a size of 2*m_sbtEntrySize. We
		// also indicate the stride between the two miss shaders, which is the size
		// of a SBT entry
		uint32_t missSectionSizeInBytes = m_sbtHelper.GetMissSectionSize();
		desc.MissShaderTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes;
		desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
		desc.MissShaderTable.StrideInBytes = m_sbtHelper.GetMissEntrySize();

		// The hit groups section start after the miss shaders. In this sample we
		// have one 1 hit group for the triangle
		uint32_t hitGroupsSectionSize = m_sbtHelper.GetHitGroupSectionSize();
		desc.HitGroupTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes + missSectionSizeInBytes;
		desc.HitGroupTable.SizeInBytes = hitGroupsSectionSize;
		desc.HitGroupTable.StrideInBytes = m_sbtHelper.GetHitGroupEntrySize();

		// Dimensions of the image to render, identical to a kernel launch dimension
		desc.Width = GetWidth();
		desc.Height = GetHeight();
		desc.Depth = 1;

		// Bind the raytracing pipeline
		m_commandList->SetPipelineState1(m_rtStateObject.Get());
		// Dispatch the rays and write to the raytracing output
		m_commandList->DispatchRays(&desc);

		// The raytracing output needs to be copied to the actual render target used
		// for display. For this, we need to transition the raytracing output from a
		// UAV to a copy source, and the render target buffer to a copy destination.
		// We can then do the actual copy, before transitioning the render target
		// buffer into a render target, that will be then used to display the image
		transition = CD3DX12_RESOURCE_BARRIER::Transition(m_outputResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		m_commandList->ResourceBarrier(1, &transition);
		transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		m_commandList->ResourceBarrier(1, &transition);
		m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(), m_outputResource.Get());
		transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
		m_commandList->ResourceBarrier(1, &transition);
	}


	// Indicate that the back buffer will now be used to present.
	m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	ThrowIfFailed(m_commandList->Close());
}

void D3D12HelloTriangle::WaitForPreviousFrame()
{
	// WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
	// This is code implemented as such for simplicity. The D3D12HelloFrameBuffering
	// sample illustrates how to use fences for efficient resource usage and to
	// maximize GPU utilization.

	// Signal and increment the fence value.
	const UINT64 fence = m_fenceValue;
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
	m_fenceValue++;

	// Wait until the previous frame is finished.
	if (m_fence->GetCompletedValue() < fence)
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12HelloTriangle::CheckRaytracingSupport() {
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
	ThrowIfFailed(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
	if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
		throw std::runtime_error("Raytracing not supported on device");
}

void D3D12HelloTriangle::OnKeyUp(UINT8 key)
{ 
	// Alternate between rasterization and raytracing using the spacebar 
	if (key == VK_SPACE) { m_raster = !m_raster; }
}

AccelerationStructureBuffers D3D12HelloTriangle::CreateBottomLevelAS(std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vVertexBuffers, std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vIndexBuffers) {
	nv_helpers_dx12::BottomLevelASGenerator bottomLevelAS;
	// Adding all vertex buffers and not transforming their position. 
	for (size_t i = 0; i < vVertexBuffers.size(); i++) {
		if (i < vIndexBuffers.size() && vIndexBuffers[i].second > 0) {
			bottomLevelAS.AddVertexBuffer(vVertexBuffers[i].first.Get(), 0, vVertexBuffers[i].second, sizeof(Vertex), vIndexBuffers[i].first.Get(), 0, vIndexBuffers[i].second, nullptr, 0, true);
		}
		else {
			bottomLevelAS.AddVertexBuffer(vVertexBuffers[i].first.Get(), 0, vVertexBuffers[i].second, sizeof(Vertex), 0, 0);
		}
	}

	// The AS build requires some scratch space to store temporary information.
	// The amount of scratch memory is dependent on the scene complexity.
	UINT64 scratchSizeInBytes = 0;
	// The final AS also needs to be stored in addition to the existing vertex 
	// buffers. It size is also dependent on the scene complexity.
	UINT64 resultSizeInBytes = 0;
	bottomLevelAS.ComputeASBufferSizes(m_device.Get(), false, &scratchSizeInBytes, &resultSizeInBytes);
	// Once the sizes are obtained, the application is responsible for allocating
	// the necessary buffers. Since the entire generation will be done on the GPU,
	// we can directly allocate those on the default heap
	AccelerationStructureBuffers buffers;
	buffers.pScratch = nv_helpers_dx12::CreateBuffer(m_device.Get(), scratchSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, nv_helpers_dx12::kDefaultHeapProps);
	buffers.pResult = nv_helpers_dx12::CreateBuffer(m_device.Get(), resultSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nv_helpers_dx12::kDefaultHeapProps);
	// Build the acceleration structure. Note that this call integrates a barrier
	// on the generated AS, so that it can be used to compute a top-level AS right
	// after this method.
	bottomLevelAS.Generate(m_commandList.Get(), buffers.pScratch.Get(), buffers.pResult.Get(), false, nullptr);
	return buffers;
}

// Create the main acceleration structure that holds all instances of the scene.
// Similarly to the bottom-level AS generation, it is done in 3 steps: gathering
// the instances, computing the memory requirements for the AS, and building the
// AS itself
void D3D12HelloTriangle::CreateTopLevelAS(const std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>>& instances, bool updateOnly)
{ 
	if (!updateOnly) {
		for (size_t i = 0; i < instances.size(); i++)
		{
			m_topLevelASGenerator.AddInstance(instances[i].first.Get(), instances[i].second, static_cast<UINT>(i), static_cast<UINT>(2 * i) /*2 hit groups per instance*/);
		}

		UINT64 scratchSize, resultSize, instanceDescsSize;
		m_topLevelASGenerator.ComputeASBufferSizes(m_device.Get(), true, &scratchSize, &resultSize, &instanceDescsSize);
		m_topLevelASBuffers.pScratch = nv_helpers_dx12::CreateBuffer(m_device.Get(), scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nv_helpers_dx12::kDefaultHeapProps);
		m_topLevelASBuffers.pResult = nv_helpers_dx12::CreateBuffer(m_device.Get(), resultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nv_helpers_dx12::kDefaultHeapProps);
		m_topLevelASBuffers.pInstanceDesc = nv_helpers_dx12::CreateBuffer(m_device.Get(), instanceDescsSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
	}

	m_topLevelASGenerator.Generate(m_commandList.Get(), m_topLevelASBuffers.pScratch.Get(), m_topLevelASBuffers.pResult.Get(), m_topLevelASBuffers.pInstanceDesc.Get(), updateOnly, m_topLevelASBuffers.pResult.Get());
}

// Combine the BLAS and TLAS builds to construct the entire acceleration
// structure required to raytrace the scene
void D3D12HelloTriangle::CreateAccelerationStructures()
{
	AccelerationStructureBuffers cubeBottomLevelBuffers = CreateBottomLevelAS({ {m_CubeBuffer.Get(), 6 * 6} });
	AccelerationStructureBuffers planeBottomLevelBuffers = CreateBottomLevelAS({{m_planeBuffer.Get(), 6} });

	m_instances = {
		{cubeBottomLevelBuffers.pResult, XMMatrixTranslation(0, 0, 0)},
		{planeBottomLevelBuffers.pResult, XMMatrixTranslation(0, 0, 0)}
	};

	CreateTopLevelAS(m_instances);

	m_commandList->Close();
	ID3D12CommandList *ppCommandLists[] = {m_commandList.Get()}; m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
	m_fenceValue++;
	m_commandQueue->Signal(m_fence.Get(), m_fenceValue); m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
	WaitForSingleObject(m_fenceEvent, INFINITE);

	ThrowIfFailed( m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));
	m_bottomLevelAS = cubeBottomLevelBuffers.pResult;
}

// The ray generation shader needs to access 2 resources: the raytracing output
// and the top-level acceleration structure
ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateRayGenSignature() {
	nv_helpers_dx12::RootSignatureGenerator rsc;
	rsc.AddHeapRangesParameter({
		{0 /*u0*/, 1 /*1 descriptor */, 0 /*use the implicit register space 0*/, D3D12_DESCRIPTOR_RANGE_TYPE_UAV /* UAV representing the output buffer*/, 0 /*heap slot where the UAV is defined*/},
		{0 /*t0*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV /*Top-level acceleration structure*/, 1},
		{0 /*b0*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV /*Camera parameters*/, 2}
	});
	return rsc.Generate(m_device.Get(), true);
}

// The hit shader communicates only through the ray payload, and therefore does
// not require any resources
ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateHitSignature() {
	nv_helpers_dx12::RootSignatureGenerator rsc;
	// The vertex colors may differ for each instance, so it is not possible to
	// point to a single buffer in the heap. Instead we use the concept of root
	// parameters, which are defined directly by a pointer in memory. In the
	// shader binding table we will associate each hit shader instance with its
	// constant buffer. Here we bind the buffer to the first slot, accessible in
	// HLSL as register(b0)
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0 /*t0*/); // vertices and colors
	rsc.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 1 /*t1*/); // indices
	rsc.AddHeapRangesParameter({ { 2 /*t2*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1 /*2nd slot of the heap*/ } });
	return rsc.Generate(m_device.Get(), true);
}

// The miss shader communicates only through the ray payload, and therefore
// does not require any resources
ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateMissSignature() {
	nv_helpers_dx12::RootSignatureGenerator rsc;
	return rsc.Generate(m_device.Get(), true);
}

// The raytracing pipeline binds the shader code, root signatures and pipeline
// characteristics in a single structure used by DXR to invoke the shaders and
// manage temporary memory during raytracing
void D3D12HelloTriangle::CreateRaytracingPipeline()
{
	nv_helpers_dx12::RayTracingPipelineGenerator pipeline(m_device.Get());

	m_rayGenLibrary = nv_helpers_dx12::CompileShaderLibrary(L"res/shaders/RayGen.hlsl");
	m_missLibrary = nv_helpers_dx12::CompileShaderLibrary(L"res/shaders/Miss.hlsl");
	m_hitLibrary = nv_helpers_dx12::CompileShaderLibrary(L"res/shaders/Hit.hlsl");
	m_shadowLibrary = nv_helpers_dx12::CompileShaderLibrary(L"res/shaders/ShadowRay.hlsl");

	pipeline.AddLibrary(m_rayGenLibrary.Get(), {L"RayGen"});
	pipeline.AddLibrary(m_missLibrary.Get(), {L"Miss"});
	pipeline.AddLibrary(m_hitLibrary.Get(), { L"ClosestHit", L"CubeClosestHit", L"PlaneClosestHit" });
	pipeline.AddLibrary(m_shadowLibrary.Get(), { L"ShadowClosestHit", L"ShadowMiss" });

	// To be used, each DX12 shader needs a root signature defining which
	// parameters and buffers will be accessed.
	m_rayGenSignature = CreateRayGenSignature();
	m_missSignature = CreateMissSignature();
	m_hitSignature = CreateHitSignature();
	m_shadowSignature = CreateHitSignature();

	pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");
	pipeline.AddHitGroup(L"CubeHitGroup", L"CubeClosestHit");
	pipeline.AddHitGroup(L"PlaneHitGroup", L"PlaneClosestHit");
	pipeline.AddHitGroup(L"ShadowHitGroup", L"ShadowClosestHit");

	pipeline.AddRootSignatureAssociation(m_rayGenSignature.Get(), {L"RayGen"}); 
	pipeline.AddRootSignatureAssociation(m_shadowSignature.Get(), { L"ShadowHitGroup" });
	pipeline.AddRootSignatureAssociation(m_missSignature.Get(), { L"Miss", L"ShadowMiss" });
	pipeline.AddRootSignatureAssociation(m_hitSignature.Get(), { L"HitGroup", L"CubeHitGroup", L"PlaneHitGroup", });

	pipeline.SetMaxPayloadSize(4 * sizeof(float)); 
	pipeline.SetMaxAttributeSize(2 * sizeof(float)); 
	pipeline.SetMaxRecursionDepth(2);

	m_rtStateObject = pipeline.Generate(); 
	ThrowIfFailed( m_rtStateObject->QueryInterface(IID_PPV_ARGS(&m_rtStateObjectProps)));
}

// Allocate the buffer holding the raytracing output, with the same size as the
// output image
void D3D12HelloTriangle::CreateRaytracingOutputBuffer() 
{
	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.DepthOrArraySize = 1;
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; 
	resDesc.Width = GetWidth(); 
	resDesc.Height = GetHeight(); 
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; 
	resDesc.MipLevels = 1; 
	resDesc.SampleDesc.Count = 1; 
	ThrowIfFailed(m_device->CreateCommittedResource( &nv_helpers_dx12::kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&m_outputResource)));
}

// Create the main heap used by the shaders, which will give access to the
// raytracing output and the top-level acceleration structure
void D3D12HelloTriangle::CreateShaderResourceHeap() 
{ 
	// Create a SRV/UAV/CBV descriptor heap. We need 3 entries - 1 SRV for the TLAS, 1 UAV for the
	// raytracing output and 1 CBV for the camera matrices
	m_srvUavHeap = nv_helpers_dx12::CreateDescriptorHeap(m_device.Get(), 3, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);
	// Get a handle to the heap memory on the CPU side, to be able to write the #
	// descriptors directly 
	D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(); 
	// Create the UAV. Based on the root signature we created it is the first 
	// entry. The Create*View methods write the view information directly into 
	// srvHandle 
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {}; 
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; 
	m_device->CreateUnorderedAccessView(m_outputResource.Get(), nullptr, &uavDesc, srvHandle); 
	// Add the Top Level AS SRV right after the raytracing output buffer 
	srvHandle.ptr += m_device->GetDescriptorHandleIncrementSize( D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); 
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc; 
	srvDesc.Format = DXGI_FORMAT_UNKNOWN; 
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE; 
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; 
	srvDesc.RaytracingAccelerationStructure.Location = m_topLevelASBuffers.pResult->GetGPUVirtualAddress(); 
	// Write the acceleration structure view in the heap 
	m_device->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);

	// Perspective Camera
	// Add the constant buffer for the camera after the TLAS
	srvHandle.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	// Describe and create a constant buffer view for the camera
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = m_cameraBufferSize;
	m_device->CreateConstantBufferView(&cbvDesc, srvHandle);
}

// The Shader Binding Table (SBT) is the cornerstone of the raytracing setup:
// this is where the shader resources are bound to the shaders, in a way that
// can be interpreted by the raytracer on GPU. In terms of layout, the SBT
// contains a series of shader IDs with their resource pointers. The SBT
// contains the ray generation shader, the miss shaders, then the hit groups.
// Using the helper class, those can be specified in arbitrary order.
void D3D12HelloTriangle::CreateShaderBindingTable()
{
	m_sbtHelper.Reset(); 
	D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();

	auto heapPointer = reinterpret_cast<UINT64*>(srvUavHeapHandle.ptr);

	m_sbtHelper.AddRayGenerationProgram(L"RayGen", { heapPointer });

	m_sbtHelper.AddMissProgram(L"Miss", {}); 
	m_sbtHelper.AddMissProgram(L"ShadowMiss", {});
	
	m_sbtHelper.AddHitGroup(L"CubeHitGroup", {});
	m_sbtHelper.AddHitGroup(L"ShadowHitGroup", {});

	m_sbtHelper.AddHitGroup(L"PlaneHitGroup", { heapPointer });

	uint32_t sbtSize = m_sbtHelper.ComputeSBTSize();  
	m_sbtStorage = nv_helpers_dx12::CreateBuffer( m_device.Get(), sbtSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

	if (!m_sbtStorage) throw std::logic_error("Could not allocate the shader binding table.");

	m_sbtHelper.Generate(m_sbtStorage.Get(), m_rtStateObjectProps.Get());
}

// The camera buffer is a constant buffer that stores the transform matrices of
// the camera, for use by both the rasterization and raytracing. This method
// allocates the buffer where the matrices will be copied. For the sake of code
// clarity, it also creates a heap containing only this buffer, to use in the
// rasterization path.
void D3D12HelloTriangle::CreateCameraBuffer() {
	uint32_t nbMatrix = 4;
	// view, perspective, viewInv, perspectiveInv 
	m_cameraBufferSize = nbMatrix * sizeof(XMMATRIX);
	// Create the constant buffer for all matrices 
	m_cameraBuffer = nv_helpers_dx12::CreateBuffer(m_device.Get(), m_cameraBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
	// Create a descriptor heap that will be used by the rasterization shaders 
	m_constHeap = nv_helpers_dx12::CreateDescriptorHeap(m_device.Get(), 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);
	// Describe and create the constant buffer view. 
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = m_cameraBufferSize;
	// Get a handle to the heap memory on the CPU side, to be able to write the 
	// descriptors directly 
	D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_constHeap->GetCPUDescriptorHandleForHeapStart();
	m_device->CreateConstantBufferView(&cbvDesc, srvHandle);
}


// Create and copies the viewmodel and perspective matrices of the camera
void D3D12HelloTriangle::UpdateCameraBuffer() {
	std::vector<XMMATRIX> matrices(4);

	const glm::mat4& mat = nv_helpers_dx12::CameraManip.getMatrix();
	memcpy(&matrices[0].r->m128_f32[0], glm::value_ptr(mat), 16 * sizeof(float));

	float fovAngleY = 45.0f * XM_PI / 180.0f;
	matrices[1] = XMMatrixPerspectiveFovRH(fovAngleY, m_aspectRatio, 0.1f, 1000.0f);

	XMVECTOR det;
	matrices[2] = XMMatrixInverse(&det, matrices[0]);
	matrices[3] = XMMatrixInverse(&det, matrices[1]);

	uint8_t* pData;
	ThrowIfFailed(m_cameraBuffer->Map(0, nullptr, (void**)&pData));
	memcpy(pData, matrices.data(), m_cameraBufferSize);
	m_cameraBuffer->Unmap(0, nullptr);
}

void D3D12HelloTriangle::OnButtonDown(UINT32 lParam)
{
	nv_helpers_dx12::CameraManip.setMousePosition(-GET_X_LPARAM(lParam), -GET_Y_LPARAM(lParam));
}

void D3D12HelloTriangle::OnMouseMove(UINT8 wParam, UINT32 lParam)
{
	using nv_helpers_dx12::Manipulator;

	Manipulator::Inputs inputs;
	inputs.lmb = wParam & MK_LBUTTON;
	inputs.mmb = wParam & MK_MBUTTON;
	inputs.rmb = wParam & MK_RBUTTON;

	// no mouse button pressed
	if (!inputs.lmb && !inputs.rmb && !inputs.mmb) return;

	inputs.ctrl = GetAsyncKeyState(VK_CONTROL);
	inputs.shift = GetAsyncKeyState(VK_SHIFT);
	inputs.alt = GetAsyncKeyState(VK_MENU);
	CameraManip.mouseMove(-GET_X_LPARAM(lParam), -GET_Y_LPARAM(lParam), inputs);
}

void D3D12HelloTriangle::CreateTetrahoidVB() {
	// Define the geometry for a tetrahedron.
	Vertex tetrahoidVertices[] = {
		{{std::sqrtf(8.f / 9.f), 0.f, -1.f / 3.f}, {1.f, 0.f, 0.f, 1.f}},
		{{-std::sqrtf(2.f / 9.f), std::sqrtf(2.f / 3.f), -1.f / 3.f}, {0.f, 1.f, 0.f, 1.f}},
		{{-std::sqrtf(2.f / 9.f), -std::sqrtf(2.f / 3.f), -1.f / 3.f}, {0.f, 0.f, 1.f, 1.f}},
		{{0.f, 0.f, 1.f}, {1, 0, 1, 1}}
	};


	const UINT vertexBufferSize = sizeof(tetrahoidVertices);

	ThrowIfFailed(m_device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_tetrahoidBuffer)));

	// Copy the triangle data to the vertex buffer.
	UINT8* pVertexDataBegin;
	CD3DX12_RANGE readRange(0, 0);		// We do not intend to read from this resource on the CPU.
	ThrowIfFailed(m_tetrahoidBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
	memcpy(pVertexDataBegin, tetrahoidVertices, sizeof(tetrahoidVertices));
	m_tetrahoidBuffer->Unmap(0, nullptr);

	// Initialize the vertex buffer view.
	m_tetrahoidBufferView.BufferLocation = m_tetrahoidBuffer->GetGPUVirtualAddress();
	m_tetrahoidBufferView.StrideInBytes = sizeof(Vertex);
	m_tetrahoidBufferView.SizeInBytes = vertexBufferSize;

	// Indices
	std::vector<UINT> indices = { 0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2 };
	const UINT indexBufferSize = static_cast<UINT>(indices.size()) * sizeof(UINT);
	CD3DX12_HEAP_PROPERTIES heapProperty = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC bufferResource = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
	ThrowIfFailed(m_device->CreateCommittedResource(&heapProperty, D3D12_HEAP_FLAG_NONE, &bufferResource, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_indexBuffer)));
	// Copy the triangle data to the index buffer.
	UINT8* pIndexDataBegin;
	ThrowIfFailed(m_indexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pIndexDataBegin)));
	memcpy(pIndexDataBegin, indices.data(), indexBufferSize);
	m_indexBuffer->Unmap(0, nullptr);
	// Initialize the index buffer view.
	m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
	m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
	m_indexBufferView.SizeInBytes = indexBufferSize;
}

void D3D12HelloTriangle::CreatePlaneVB() {
	// Define the geometry for a plane. 
	Vertex planeVertices[] = {
		{{-1.5f, -.8f, 01.5f}, {0.0f, 0.8f, 0.9f, 1.0f}}, // 0 
		{{-1.5f, -.8f, -1.5f}, {0.0f, 0.8f, 0.9f, 1.0f}}, // 1 
		{{01.5f, -.8f, 01.5f}, {0.0f, 0.8f, 0.9f, 1.0f}}, // 2 
		{{01.5f, -.8f, 01.5f}, {0.0f, 0.8f, 0.9f, 1.0f}}, // 2 
		{{-1.5f, -.8f, -1.5f}, {0.0f, 0.8f, 0.9f, 1.0f}}, // 1 
		{{01.5f, -.8f, -1.5f}, {0.0f, 0.8f, 0.9f, 1.0f}} // 4 
	};
	const UINT planeBufferSize = sizeof(planeVertices);

	// Note: using upload heaps to transfer static data like vert buffers is not 
	// recommended. Every time the GPU needs it, the upload heap will be 
	// marshalled over. Please read up on Default Heap usage. An upload heap is 
	// used here for code simplicity and because there are very few verts to 
	// actually transfer. 
	CD3DX12_HEAP_PROPERTIES heapProperty = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC bufferResource = CD3DX12_RESOURCE_DESC::Buffer(planeBufferSize);
	ThrowIfFailed(m_device->CreateCommittedResource(&heapProperty, D3D12_HEAP_FLAG_NONE, &bufferResource, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_planeBuffer))); 

	// Copy the triangle data to the vertex buffer. 
	UINT8* pVertexDataBegin;
	CD3DX12_RANGE readRange(0, 0);

	// We do not intend to read from this resource on the CPU. 
	ThrowIfFailed(m_planeBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
	memcpy(pVertexDataBegin, planeVertices, sizeof(planeVertices));
	m_planeBuffer->Unmap(0, nullptr);

	// Initialize the vertex buffer view. 
	m_planeBufferView.BufferLocation = m_planeBuffer->GetGPUVirtualAddress();
	m_planeBufferView.StrideInBytes = sizeof(Vertex);
	m_planeBufferView.SizeInBytes = planeBufferSize;
}

void D3D12HelloTriangle::CreateCubeVB() {
	// Define the geometry for a cube. 
	Vertex cubeVertices[] = {
		{{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f, -0.5f, -0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f,  0.5f, -0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f,  0.5f, -0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},

		{{0.5f, -0.5f,  0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f,  0.5f,  0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f, -0.5f,  0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},

		{{-0.5f,  -0.5f,  0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f,  -0.5f, -0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f, 0.5f, -0.5f},   {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f, 0.5f, -0.5f},   {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f, 0.5f,  0.5f},   {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f,  -0.5f,  0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},

		{{0.5f,  0.5f,  0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f,  0.5f, -0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f, -0.5f, -0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f, -0.5f, -0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f, -0.5f,  0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f,  0.5f,  0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},

		{{0.5f, -0.5f, -0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f, -0.5f,  0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f, -0.5f, -0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},

		{{-0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f,  0.5f, -0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f,  0.5f,  0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{0.5f,  0.5f,  0.5f},  {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
		{{-0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.5f, 1.0f}},
	};
	const UINT cubeBufferSize = sizeof(cubeVertices);

	// Note: using upload heaps to transfer static data like vert buffers is not 
	// recommended. Every time the GPU needs it, the upload heap will be 
	// marshalled over. Please read up on Default Heap usage. An upload heap is 
	// used here for code simplicity and because there are very few verts to 
	// actually transfer. 
	CD3DX12_HEAP_PROPERTIES heapProperty = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC bufferResource = CD3DX12_RESOURCE_DESC::Buffer(cubeBufferSize);
	ThrowIfFailed(m_device->CreateCommittedResource(&heapProperty, D3D12_HEAP_FLAG_NONE, &bufferResource, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_CubeBuffer)));

	// Copy the triangle data to the vertex buffer. 
	UINT8* pVertexDataBegin;
	CD3DX12_RANGE readRange(0, 0);

	// We do not intend to read from this resource on the CPU. 
	ThrowIfFailed(m_CubeBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
	memcpy(pVertexDataBegin, cubeVertices, sizeof(cubeVertices));
	m_CubeBuffer->Unmap(0, nullptr);

	// Initialize the vertex buffer view. 
	m_cubeBufferView.BufferLocation = m_CubeBuffer->GetGPUVirtualAddress();
	m_cubeBufferView.StrideInBytes = sizeof(Vertex);
	m_cubeBufferView.SizeInBytes = cubeBufferSize;
}

void D3D12HelloTriangle::CreateGlobalConstantBuffer()
{
	// Due to HLSL packing rules, we create the CB with 9 float4 (each needs to start on a 16-byte boundary) 
	XMVECTOR bufferData[] = {
		// A 
		XMVECTOR{1.0f, 0.0f, 0.0f, 1.0f}, XMVECTOR{0.7f, 0.4f, 0.0f, 1.0f}, XMVECTOR{0.4f, 0.7f, 0.0f, 1.0f},
		// B 
		XMVECTOR{0.0f, 1.0f, 0.0f, 1.0f}, XMVECTOR{0.0f, 0.7f, 0.4f, 1.0f}, XMVECTOR{0.0f, 0.4f, 0.7f, 1.0f},
		// C 
		XMVECTOR{0.0f, 0.0f, 1.0f, 1.0f}, XMVECTOR{0.4f, 0.0f, 0.7f, 1.0f}, XMVECTOR{0.7f, 0.0f, 0.4f, 1.0f},
	};
	// Create our buffer 
	m_globalConstantBuffer = nv_helpers_dx12::CreateBuffer(m_device.Get(), sizeof(bufferData), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
	// Copy CPU memory to GPU 
	uint8_t* pData;
	ThrowIfFailed(m_globalConstantBuffer->Map(0, nullptr, (void**)&pData));
	memcpy(pData, bufferData, sizeof(bufferData));
	m_globalConstantBuffer->Unmap(0, nullptr);
}

void D3D12HelloTriangle::CreatePerInstanceConstantBuffers()
{
	// Due to HLSL packing rules, we create the CB with 9 float4 (each needs to start on a 16-byte boundary) 
	XMVECTOR bufferData[] = {
		// A 
		XMVECTOR{1.0f, 0.0f, 0.0f, 1.0f}, XMVECTOR{1.0f, 0.4f, 0.0f, 1.0f}, XMVECTOR{1.f, 0.7f, 0.0f, 1.0f},
		// B 
		XMVECTOR{0.0f, 1.0f, 0.0f, 1.0f}, XMVECTOR{0.0f, 1.0f, 0.4f, 1.0f}, XMVECTOR{0.0f, 1.0f, 0.7f, 1.0f},
		// C 
		XMVECTOR{0.0f, 0.0f, 1.0f, 1.0f}, XMVECTOR{0.4f, 0.0f, 1.0f, 1.0f}, XMVECTOR{0.7f, 0.0f, 1.0f, 1.0f},
	};

	m_perInstanceConstantBuffers.resize(3);

	int i(0);
	for (auto& cb : m_perInstanceConstantBuffers) {
		const uint32_t bufferSize = sizeof(XMVECTOR) * 3;

		cb = nv_helpers_dx12::CreateBuffer(m_device.Get(), bufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
		
		uint8_t* pData;
		ThrowIfFailed(cb->Map(0, nullptr, (void**)&pData));
		memcpy(pData, &bufferData[i * 3], bufferSize);

		cb->Unmap(0, nullptr); ++i;
	}
}

void D3D12HelloTriangle::CreateDepthBuffer()
{
	// The depth buffer heap type is specific for that usage, and the heap contents are not visible 
	// from the shaders 
	m_dsvHeap = nv_helpers_dx12::CreateDescriptorHeap(m_device.Get(), 1, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, false);

	// The depth and stencil can be packed into a single 32-bit texture buffer. Since we do not need 
	// stencil, we use the 32 bits to store depth information (DXGI_FORMAT_D32_FLOAT). 
	D3D12_HEAP_PROPERTIES depthHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC depthResourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, m_width, m_height, 1, 1);
	depthResourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	// The depth values will be initialized to 1 
	CD3DX12_CLEAR_VALUE depthOptimizedClearValue(DXGI_FORMAT_D32_FLOAT, 1.0f, 0);

	// Allocate the buffer itself, with a state allowing depth writes 
	ThrowIfFailed(m_device->CreateCommittedResource(&depthHeapProperties, D3D12_HEAP_FLAG_NONE, &depthResourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthOptimizedClearValue, IID_PPV_ARGS(&m_depthStencil)));

	// Write the depth buffer view into the depth buffer heap 
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	m_device->CreateDepthStencilView(m_depthStencil.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}