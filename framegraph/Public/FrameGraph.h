// Copyright (c) 2018-2020,  Zhirnov Andrey. For more information see 'LICENSE'

#pragma once

#include "framegraph/Public/CommandBuffer.h"
#include "framegraph/Public/PipelineCompiler.h"
#include "framegraph/Public/VulkanTypes.h"
#include "framegraph/Public/Pipeline.h"
#include "framegraph/Public/BufferDesc.h"
#include "framegraph/Public/ImageDesc.h"
#include "framegraph/Public/SamplerDesc.h"
#include "framegraph/Public/MemoryDesc.h"
#include "framegraph/Public/RenderPassDesc.h"
#include "framegraph/Public/PipelineResources.h"
#include "framegraph/Public/FGEnums.h"

namespace FG
{

	//
	// Frame Graph interface
	//

	class IFrameGraph : public std::enable_shared_from_this<IFrameGraph>
	{
	// types
	public:
		using DeviceInfo_t			= Union< NullUnion, VulkanDeviceInfo >;
		using SwapchainCreateInfo_t	= Union< NullUnion, VulkanSwapchainCreateInfo/*, VulkanVREmulatorSwapchainCreateInfo*/ >;
		using ExternalImageDesc_t	= Union< NullUnion, VulkanImageDesc >;
		using ExternalBufferDesc_t	= Union< NullUnion, VulkanBufferDesc >;
		using ExternalImage_t		= Union< NullUnion, ImageVk_t >;
		using ExternalBuffer_t		= Union< NullUnion, BufferVk_t >;

		using OnExternalImageReleased_t		= std::function< void (const ExternalImage_t &) >;
		using OnExternalBufferReleased_t	= std::function< void (const ExternalBuffer_t &) >;
		using ShaderDebugCallback_t			= std::function< void (StringView taskName, StringView shaderName, EShaderStages, ArrayView<String> output) >;

		struct RenderingStatistics
		{
			uint		descriptorBinds				= 0;
			uint		pushConstants				= 0;
			uint		pipelineBarriers			= 0;
			uint		transferOps					= 0;

			uint		indexBufferBindings			= 0;
			uint		vertexBufferBindings		= 0;
			uint		drawCalls					= 0;
			uint64_t	vertexCount					= 0;	// sum of vertices / indices, indirect draw isn't measured
			uint64_t	primitiveCount				= 0;	// sum of primitives
			uint		graphicsPipelineBindings	= 0;
			uint		dynamicStateChanges			= 0;

			uint		dispatchCalls				= 0;
			uint		computePipelineBindings		= 0;

			uint		rayTracingPipelineBindings	= 0;
			uint		traceRaysCalls				= 0;
			uint		buildASCalls				= 0;

			// for command buffers
			Nanoseconds	gpuTime						{0};	// for (currentFrame - ringBufferSize)
			Nanoseconds	cpuTime						{0};	// for (currentFrame - ringBufferSize)

			Nanoseconds submitingTime				{0};
			Nanoseconds waitingTime					{0};
		};

		struct ResourceStatistics
		{
			uint		newGraphicsPipelineCount	= 0;
			uint		newComputePipelineCount		= 0;
			uint		newRayTracingPipelineCount	= 0;
		};

		struct Statistics
		{
			RenderingStatistics		renderer;
			ResourceStatistics		resources;

			void Merge (const Statistics &);
		};
		

	// interface
	public:
		
			// Creates the framegraph.
		ND_ static FrameGraph		CreateFrameGraph (const DeviceInfo_t &);

			// Returns name and version number.
		ND_ static const char*		GetVersion ();


		// initialization //

			// Deinitialize instance systems.
			virtual void			Deinitialize () = 0;
			
			// Add pipeline compiler.
			virtual bool			AddPipelineCompiler (const PipelineCompiler &comp) = 0;
			
			// Callback will be called at end of the frame if debugging enabled by
			// calling 'Task::EnableDebugTrace' and shader compiled with 'EShaderLangFormat::EnableDebugTrace' flag.
			virtual bool			SetShaderDebugCallback (ShaderDebugCallback_t &&) = 0;

			// Returns device info with which framegraph has been crated.
		ND_ virtual DeviceInfo_t	GetDeviceInfo () const = 0;

			// Returns bitmask for all available queues.
		ND_ virtual EQueueUsage		GetAvilableQueues () const = 0;


		// resource manager //

			// Create resources: pipeline, image, buffer, etc.
			// See synchronization requirements on top of this file.
		ND_ virtual MPipelineID		CreatePipeline (INOUT MeshPipelineDesc &desc, StringView dbgName = Default) = 0;
		ND_ virtual RTPipelineID	CreatePipeline (INOUT RayTracingPipelineDesc &desc) = 0;
		ND_ virtual GPipelineID		CreatePipeline (INOUT GraphicsPipelineDesc &desc, StringView dbgName = Default) = 0;
		ND_ virtual CPipelineID		CreatePipeline (INOUT ComputePipelineDesc &desc, StringView dbgName = Default) = 0;
		ND_ virtual ImageID			CreateImage (const ImageDesc &desc, const MemoryDesc &mem = Default, StringView dbgName = Default) = 0;
		ND_ virtual ImageID			CreateImage (const ImageDesc &desc, const MemoryDesc &mem, EResourceState defaultState, StringView dbgName = Default) = 0;
		ND_ virtual BufferID		CreateBuffer (const BufferDesc &desc, const MemoryDesc &mem = Default, StringView dbgName = Default) = 0;
		ND_ virtual ImageID			CreateImage (const ExternalImageDesc_t &desc, OnExternalImageReleased_t &&, StringView dbgName = Default) = 0;
		ND_ virtual BufferID		CreateBuffer (const ExternalBufferDesc_t &desc, OnExternalBufferReleased_t &&, StringView dbgName = Default) = 0;
		ND_ virtual SamplerID		CreateSampler (const SamplerDesc &desc, StringView dbgName = Default) = 0;
		ND_ virtual SwapchainID		CreateSwapchain (const SwapchainCreateInfo_t &, RawSwapchainID oldSwapchain = Default, StringView dbgName = Default) = 0;
		ND_ virtual RTGeometryID	CreateRayTracingGeometry (const RayTracingGeometryDesc &desc, const MemoryDesc &mem = Default, StringView dbgName = Default) = 0;
		ND_ virtual RTSceneID		CreateRayTracingScene (const RayTracingSceneDesc &desc, const MemoryDesc &mem = Default, StringView dbgName = Default) = 0;
		ND_ virtual RTShaderTableID	CreateRayTracingShaderTable (StringView dbgName = Default) = 0;
			virtual bool			InitPipelineResources (RawGPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const = 0;
			virtual bool			InitPipelineResources (RawCPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const = 0;
			virtual bool			InitPipelineResources (RawMPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const = 0;
			virtual bool			InitPipelineResources (RawRTPipelineID pplnId, const DescriptorSetID &id, OUT PipelineResources &resources) const = 0;

		ND_ virtual bool			IsSupported (RawImageID image, const ImageViewDesc &desc) const = 0;
		ND_ virtual bool			IsSupported (RawBufferID buffer, const BufferViewDesc &desc) const = 0;
		ND_ virtual bool			IsSupported (const ImageDesc &desc, EMemoryType memType = EMemoryType::Default) const = 0;
		ND_ virtual bool			IsSupported (const BufferDesc &desc, EMemoryType memType = EMemoryType::Default) const = 0;

			// Creates internal descriptor set and release dynamically allocated memory in the 'resources'.
			// After that your can not modify the 'resources', but you still can use it in the tasks.
			virtual bool			CachePipelineResources (INOUT PipelineResources &resources) = 0;
			virtual void			ReleaseResource (INOUT PipelineResources &resources) = 0;

			// Release reference to resource, Returns 'true' if resource has been deleted.
			// See synchronization requirements on top of this file.
			virtual bool			ReleaseResource (INOUT GPipelineID &id) = 0;
			virtual bool			ReleaseResource (INOUT CPipelineID &id) = 0;
			virtual bool			ReleaseResource (INOUT MPipelineID &id) = 0;
			virtual bool			ReleaseResource (INOUT RTPipelineID &id) = 0;
			virtual bool			ReleaseResource (INOUT ImageID &id) = 0;
			virtual bool			ReleaseResource (INOUT BufferID &id) = 0;
			virtual bool			ReleaseResource (INOUT SamplerID &id) = 0;
			virtual bool			ReleaseResource (INOUT SwapchainID &id) = 0;
			virtual bool			ReleaseResource (INOUT RTGeometryID &id) = 0;
			virtual bool			ReleaseResource (INOUT RTSceneID &id) = 0;
			virtual bool			ReleaseResource (INOUT RTShaderTableID &id) = 0;

			// Returns resource description.
		ND_ virtual BufferDesc const&	GetDescription (RawBufferID id) const = 0;
		ND_ virtual ImageDesc const&	GetDescription (RawImageID id) const = 0;
		//ND_ virtual SamplerDesc const&	GetDescription (RawSamplerID &id) const = 0;
		ND_ virtual ExternalBufferDesc_t GetApiSpecificDescription (RawBufferID id) const = 0;
		ND_ virtual ExternalImageDesc_t  GetApiSpecificDescription (RawImageID id) const = 0;
		
			// Returns 'true' if resource is not deleted.
		ND_	virtual bool			IsResourceAlive (RawGPipelineID id) const = 0;
		ND_	virtual bool			IsResourceAlive (RawCPipelineID id) const = 0;
		ND_	virtual bool			IsResourceAlive (RawMPipelineID id) const = 0;
		ND_	virtual bool			IsResourceAlive (RawRTPipelineID id) const = 0;
		ND_	virtual bool			IsResourceAlive (RawImageID id) const = 0;
		ND_	virtual bool			IsResourceAlive (RawBufferID id) const = 0;
		ND_	virtual bool			IsResourceAlive (RawSwapchainID id) const = 0;
		ND_	virtual bool			IsResourceAlive (RawRTGeometryID id) const = 0;
		ND_	virtual bool			IsResourceAlive (RawRTSceneID id) const = 0;
		ND_	virtual bool			IsResourceAlive (RawRTShaderTableID id) const = 0;

			// 
		ND_ virtual GPipelineID		AcquireResource (RawGPipelineID id) = 0;
		ND_ virtual CPipelineID		AcquireResource (RawCPipelineID id) = 0;
		ND_ virtual MPipelineID		AcquireResource (RawMPipelineID id) = 0;
		ND_ virtual RTPipelineID	AcquireResource (RawRTPipelineID id) = 0;
		ND_ virtual ImageID			AcquireResource (RawImageID id) = 0;
		ND_ virtual BufferID		AcquireResource (RawBufferID id) = 0;
		ND_ virtual SwapchainID		AcquireResource (RawSwapchainID id) = 0;
		ND_ virtual RTGeometryID	AcquireResource (RawRTGeometryID id) = 0;
		ND_ virtual RTSceneID		AcquireResource (RawRTSceneID id) = 0;
		ND_ virtual RTShaderTableID	AcquireResource (RawRTShaderTableID id) = 0;

			// Copy data into host-visible memory.
			virtual bool			UpdateHostBuffer (RawBufferID id, BytesU offset, BytesU size, const void *data) = 0;
			
			// Returns pointer to host-visible memory.
			template <typename T>
					bool			MapBufferRange (RawBufferID id, BytesU offset, INOUT BytesU &size, OUT T* &data);
			virtual bool			MapBufferRange (RawBufferID id, BytesU offset, INOUT BytesU &size, OUT void* &data) = 0;


		// frame execution //
		
			// Begin command buffer recording.
		ND_ virtual CommandBuffer	Begin (const CommandBufferDesc &, ArrayView<CommandBuffer> dependsOn = {}) = 0;

			// Compile framegraph for current command buffer and append it to the pending command buffer queue (waiting to submit).
			virtual bool			Execute (INOUT CommandBuffer &) = 0;

			// Wait until all commands complete execution on the GPU or until time runs out.
			virtual bool			Wait (ArrayView<CommandBuffer> commands, Nanoseconds timeout = Nanoseconds{3600'000'000'000}) = 0;

			// Submit all pending command buffers and present all pending swapchain images.
			virtual bool			Flush (EQueueUsage queues = EQueueUsage::All) = 0;

			// Wait until all commands will complete their work on GPU, trigger events for 'ReadImage' and 'ReadBuffer' tasks.
			virtual bool			WaitIdle () = 0;


		// debugging //

			// Returns framegraph statistics.
			virtual bool			GetStatistics (OUT Statistics &result) const = 0;

			// Returns serialized tasks, resource usage and barriers, can be used for regression testing.
			virtual bool			DumpToString (OUT String &result) const = 0;

			// Returns graph written on dot language, can be used for graph visualization with graphviz.
			virtual bool			DumpToGraphViz (OUT String &result) const = 0;
	};

	

	template <typename T>
	inline bool  IFrameGraph::MapBufferRange (RawBufferID id, BytesU offset, INOUT BytesU &size, OUT T* &data)
	{
		void*	ptr = null;
		bool	res = MapBufferRange( id, offset, OUT size, OUT ptr );
		data = Cast<T>( ptr );
		return res;
	}

}	// FG
