// Copyright (c) 2018,  Zhirnov Andrey. For more information see 'LICENSE'

#include "VTaskProcessor.h"
#include "VFrameGraphThread.h"
#include "VDrawTask.h"
//#include "VFramebuffer.h"
#include "VFrameGraphDebugger.h"
#include "Shared/EnumUtils.h"
#include "stl/Algorithms/StringUtils.h"

namespace FG
{

	template <typename ID>
	forceinline auto const*  VTaskProcessor::_GetState (ID id) const
	{
		return _frameGraph.GetResourceManager()->GetState( id );
	}
	
	template <typename ID>
	forceinline auto const*  VTaskProcessor::_GetResource (ID id) const
	{
		return _frameGraph.GetResourceManager()->GetResource( id );
	}
//-----------------------------------------------------------------------------



	//
	// Pipeline Resource Barriers
	//
	class VTaskProcessor::PipelineResourceBarriers
	{
	// types
	private:
		using ResourceSet_t	= PipelineResources::ResourceSet_t;


	// variables
	private:
		VTaskProcessor &	_tp;
		const Task			_currTask;


	// methods
	public:
		PipelineResourceBarriers (VTaskProcessor &tp, Task task) : _tp{tp}, _currTask{task} {}

		// ResourceGraph //
		void operator () (const PipelineResources::Buffer &buf);
		void operator () (const PipelineResources::Image &img);
		//void operator () (const PipelineResources::Texture &tex);
		//void operator () (const PipelineResources::SubpassInput &sp);
		void operator () (const PipelineResources::Sampler &) {}
		void operator () (const std::monostate &) {}
	};

	
	
	//
	// Draw Task Barriers
	//
	class VTaskProcessor::DrawTaskBarriers final
	{
	// types
	private:
		using FragmentOutput	= GraphicsPipelineDesc::FragmentOutput;
		using FragOutputs_t		= StaticArray< FragmentOutput, FG_MaxColorBuffers >;
		using UniqFragOutputs_t	= HashSet< const VGraphicsPipeline::FragmentOutputInstance *>;


	// variables
	private:
		VTaskProcessor &	_tp;
		Task				_currTask;

		FragOutputs_t		_fragOutput;
		UniqFragOutputs_t	_uniqueOutputs;
		uint				_maxFragCount;

		bool				_earlyFragmentTests		: 1;
		bool				_lateFragmentTests		: 1;
		bool				_depthRead				: 1;
		bool				_depthWrite				: 1;
		bool				_stencilReadWrite		: 1;
		bool				_rasterizerDiscard		: 1;
		bool				_compatibleFragOutput	: 1;


	// methods
	public:
		explicit DrawTaskBarriers (VTaskProcessor &tp, Task task);

		void Visit (const VFgDrawTask<DrawTask> &task);
		void Visit (const VFgDrawTask<DrawIndexedTask> &task);

		void _MergePipeline (RawGPipelineID pplnId, const RenderState &rs);

		ND_ bool						IsEarlyFragmentTests () const	{ return _earlyFragmentTests; }
		ND_ bool						IsLateFragmentTests ()	const	{ return _lateFragmentTests; }

        ND_ ArrayView<FragmentOutput>	GetFragOutputs ()		const	{ return ArrayView<FragmentOutput>{ _fragOutput.data(), _maxFragCount }; }
	};


	
	//
	// Draw Task Commands
	//
	class VTaskProcessor::DrawTaskCommands final
	{
	// variables
	private:
		VTaskProcessor &					_tp;
		VDevice const&						_dev;
		VFgTask<SubmitRenderPass> const*	_currTask;
		VkCommandBuffer						_cmdBuffer;


	// methods
	public:
		explicit DrawTaskCommands (VTaskProcessor &tp, VFgTask<SubmitRenderPass> const* task, VkCommandBuffer cmd);

		void Visit (const VFgDrawTask<DrawTask> &task);
		void Visit (const VFgDrawTask<DrawIndexedTask> &task);

		void _BindVertexBuffers (ArrayView<VLocalBuffer const*> vertexBuffers, ArrayView<VkDeviceSize> vertexOffsets) const;
		void _BindPipeline (const VLogicalRenderPass *logicalRP, RawGPipelineID pipelineId, const RenderState &renderState,
							EPipelineDynamicState dynamicStates, const VertexInputState &vertexInput) const;
		void _BindPipelineResources (RawGPipelineID pipelineId, const VkDescriptorSets_t &descriptorSets, ArrayView<uint> dynamicOffsets) const;
		void _SetScissor (const DrawTask::Scissors_t &sc) const;
	};
//-----------------------------------------------------------------------------
	

	
/*
=================================================
	operator (Buffer)
=================================================
*/
	void VTaskProcessor::PipelineResourceBarriers::operator () (const PipelineResources::Buffer &buf)
	{
		VLocalBuffer const*  buffer = _tp._GetState( buf.bufferId );

		// validation
		DEBUG_ONLY(
			auto&	limits	= _tp._frameGraph.GetDevice().GetDeviceProperties().limits;

			if ( (buf.state & EResourceState::_StateMask) == EResourceState::UniformRead )
			{
				ASSERT( (buf.offset % limits.minUniformBufferOffsetAlignment) == 0 );
				ASSERT( buf.size <= limits.maxUniformBufferRange );
			}else{
				ASSERT( (buf.offset % limits.minStorageBufferOffsetAlignment) == 0 );
				ASSERT( buf.size <= limits.maxStorageBufferRange );
			}
		)

		_tp._AddBuffer( buffer, buf.state, VkDeviceSize(buf.offset), VkDeviceSize(buf.size) );
	}
		
/*
=================================================
	operator (Image / Texture / SubpassInput)
=================================================
*/
	void VTaskProcessor::PipelineResourceBarriers::operator () (const PipelineResources::Image &img)
	{
		VLocalImage const*  image = _tp._GetState( img.imageId );

		_tp._AddImage( image, img.state, EResourceState_ToImageLayout( img.state ), *img.desc );
	}
//-----------------------------------------------------------------------------



/*
=================================================
	constructor
=================================================
*/
	VTaskProcessor::DrawTaskBarriers::DrawTaskBarriers (VTaskProcessor &tp, Task task) :
		_tp{ tp },						_currTask{ task },
		_maxFragCount{ 0 },
		_earlyFragmentTests{false},		_lateFragmentTests{false},
		_depthRead{false},				_depthWrite{false},
		_stencilReadWrite{false},		_rasterizerDiscard{false},
		_compatibleFragOutput{true}
	{
		// invalidate fragment output
		for (auto& frag : _fragOutput)
		{
			frag.index	= ~0u;
			frag.type	= EFragOutput::Unknown;
		}
	}

/*
=================================================
	Visit (DrawTask)
=================================================
*/
	void VTaskProcessor::DrawTaskBarriers::Visit (const VFgDrawTask<DrawTask> &task)
	{
		// update descriptor sets and add pipeline barriers
		_tp._ExtractDescriptorSets( task.GetResources(), OUT task.descriptorSets );

		// add vertex buffers
		for (size_t i = 0; i < task.GetVertexBuffers().size(); ++i)
		{
			VkDeviceSize	offset	= task.GetVBOffsets()[i];
			VkDeviceSize	size	= VK_WHOLE_SIZE;
			Bytes<uint>		stride	= task.GetVBStrides()[i];

			offset += VkDeviceSize(stride) * task.drawCmd.firstVertex;
			size    = VkDeviceSize(stride) * task.drawCmd.vertexCount;	// TODO: instance

			_tp._AddBuffer( task.GetVertexBuffers()[i], EResourceState::VertexBuffer, offset, size );
		}
		
		_MergePipeline( task.pipeline, task.renderState );
	}

/*
=================================================
	Visit (DrawIndexedTask)
=================================================
*/
	void VTaskProcessor::DrawTaskBarriers::Visit (const VFgDrawTask<DrawIndexedTask> &task)
	{
		// update descriptor sets and add pipeline barriers
		_tp._ExtractDescriptorSets( task.GetResources(), OUT task.descriptorSets );
		
		// add vertex buffers
		for (size_t i = 0; i < task.GetVertexBuffers().size(); ++i)
		{
			VkDeviceSize	offset	= task.GetVBOffsets()[i];
			VkDeviceSize	size	= VK_WHOLE_SIZE;
			Bytes<uint>		stride	= task.GetVBStrides()[i];
			
			offset += VkDeviceSize(stride) * task.drawCmd.vertexOffset;
			//size    = VkDeviceSize(stride) * EIndex::MaxValue( task.indexBuffer.indexType );	// TODO: instance

			_tp._AddBuffer( task.GetVertexBuffers()[i], EResourceState::VertexBuffer, offset, size );
		}
		
		// add index buffer
		{
			const VkDeviceSize	index_size	= VkDeviceSize(EIndex_SizeOf( task.indexType ));
			VkDeviceSize		offset		= VkDeviceSize(task.indexBufferOffset);
			VkDeviceSize		size		= index_size * task.drawCmd.indexCount;

			_tp._AddBuffer( task.indexBuffer, EResourceState::IndexBuffer, offset, size );
		}
		
		_MergePipeline( task.pipeline, task.renderState );
	}
	
/*
=================================================
	_MergePipeline
=================================================
*/
	void VTaskProcessor::DrawTaskBarriers::_MergePipeline (RawGPipelineID pplnId, const RenderState &renderState)
	{
		VGraphicsPipeline const*	pipeline = _tp._GetResource( pplnId );

		// merge fragment output
		auto	inst = pipeline->GetFragmentOutput();

		if ( _uniqueOutputs.insert( inst ).second )
		{
			for (const auto& src : inst->Get())
			{
				ASSERT( src.index < _fragOutput.size() );

				auto&	dst = _fragOutput[ src.index ];

				_maxFragCount = Max( _maxFragCount, src.index+1 );

				if ( dst.type == EFragOutput::Unknown and dst.index == ~0u )
				{
					dst = src;
					continue;
				}

				if ( src.id   != dst.id	  or
					 src.type != dst.type )
				{
					ASSERT( src.id == dst.id );
					ASSERT( src.type == dst.type );

					_compatibleFragOutput = false;
				}
			}
		}
		

		if ( pipeline->IsEarlyFragmentTests() )
			_earlyFragmentTests = true;
		else
			_lateFragmentTests = true;

		if ( renderState.depth.test )
			_depthRead = true;

		if ( renderState.depth.write )
			_depthWrite = true;

		if ( renderState.stencil.enabled )
			_stencilReadWrite = true;

		if ( renderState.rasterization.rasterizerDiscard )
			_rasterizerDiscard = true;
	}
//-----------------------------------------------------------------------------


	
/*
=================================================
	constructor
=================================================
*/
	VTaskProcessor::DrawTaskCommands::DrawTaskCommands (VTaskProcessor &tp, VFgTask<SubmitRenderPass> const* task, VkCommandBuffer cmd) :
		_tp{ tp },	_dev{ tp._dev },	_currTask{ task },	_cmdBuffer{ cmd }
	{
	}

/*
=================================================
	_BindVertexBuffers
=================================================
*/
	void VTaskProcessor::DrawTaskCommands::_BindVertexBuffers (ArrayView<VLocalBuffer const*> vertexBuffers, ArrayView<VkDeviceSize> vertexOffsets) const
	{
		if ( vertexBuffers.empty() )
			return;

		FixedArray<VkBuffer, FG_MaxVertexBuffers>	buffers;	buffers.resize( vertexBuffers.size() );

		for (size_t i = 0; i < vertexBuffers.size(); ++i)
		{
			buffers[i] = vertexBuffers[i]->Handle();
		}

		_dev.vkCmdBindVertexBuffers( _cmdBuffer, 0, uint(buffers.size()), buffers.data(), vertexOffsets.data() );
	}
	
/*
=================================================
	_BindPipeline
=================================================
*/
	void VTaskProcessor::DrawTaskCommands::_BindPipeline (const VLogicalRenderPass *logicalRP, RawGPipelineID pipelineId,
														  const RenderState &renderState, EPipelineDynamicState dynamicStates,
														  const VertexInputState &vertexInput) const
	{
		VkPipeline	ppln_id = _tp._frameGraph.GetPipelineCache()->CreatePipelineInstance(
									*_tp._frameGraph.GetResourceManager(), pipelineId,
									logicalRP->GetRenderPassID(), logicalRP->GetSubpassIndex(),
									renderState, vertexInput, dynamicStates, 0, uint(logicalRP->GetViewports().size()) );
		
		_dev.vkCmdBindPipeline( _cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ppln_id );
	}
	
/*
=================================================
	_BindPipelineResources
=================================================
*/
	void VTaskProcessor::DrawTaskCommands::_BindPipelineResources (RawGPipelineID pipelineId, const VkDescriptorSets_t &descriptorSets, ArrayView<uint> dynamicOffsets) const
	{
		if ( descriptorSets.empty() )
			return;

		VGraphicsPipeline const*	gppln	= _tp._GetResource( pipelineId );
		VPipelineLayout const*		layout	= _tp._GetResource( gppln->GetLayoutID() );

		_dev.vkCmdBindDescriptorSets( _cmdBuffer,
									  VK_PIPELINE_BIND_POINT_GRAPHICS,
									  layout->Handle(),
									  0,
									  uint(descriptorSets.size()), descriptorSets.data(),
									  uint(dynamicOffsets.size()), dynamicOffsets.data() );
	}
	
/*
=================================================
	_SetScissor
=================================================
*/
	void VTaskProcessor::DrawTaskCommands::_SetScissor (const DrawTask::Scissors_t &srcScissors) const
	{
		if ( not srcScissors.empty() )
		{
			FixedArray< VkRect2D, FG_MaxViewports >		vk_scissors;

			for (auto& src : srcScissors)
			{
				VkRect2D		dst = {};
				dst.offset.x		= src.left;
				dst.offset.y		= src.top;
                dst.extent.width	= uint(src.Width());
                dst.extent.height	= uint(src.Height());
				vk_scissors.push_back( dst );
			}

			_dev.vkCmdSetScissor( _cmdBuffer, 0, uint(vk_scissors.size()), vk_scissors.data() );
		}
		else
		{
			const auto&	vk_scissors = _currTask->GetLogicalPass()->GetScissors();

			_dev.vkCmdSetScissor( _cmdBuffer, 0, uint(vk_scissors.size()), vk_scissors.data() );
		}
	}

/*
=================================================
	Visit (DrawTask)
=================================================
*/
	void VTaskProcessor::DrawTaskCommands::Visit (const VFgDrawTask<DrawTask> &task)
	{
		_tp._CmdDebugMarker( task.GetName() );

		_BindPipeline( _currTask->GetLogicalPass(), task.pipeline, task.renderState,
					   task.dynamicStates, task.vertexInput );

		_BindPipelineResources( task.pipeline, task.descriptorSets, task.GetResources().dynamicOffsets );
		_BindVertexBuffers( task.GetVertexBuffers(), task.GetVBOffsets() );
		
		_SetScissor( task.scissors );

		_dev.vkCmdDraw( _cmdBuffer,
						task.drawCmd.vertexCount,
						task.drawCmd.instanceCount,
						task.drawCmd.firstVertex,
						task.drawCmd.firstInstance );
	}
	
/*
=================================================
	Visit (DrawIndexedTask)
=================================================
*/
	void VTaskProcessor::DrawTaskCommands::Visit (const VFgDrawTask<DrawIndexedTask> &task)
	{
		_tp._CmdDebugMarker( task.GetName() );
		
		_BindPipeline( _currTask->GetLogicalPass(), task.pipeline, task.renderState,
					   task.dynamicStates, task.vertexInput );
		
		_BindPipelineResources( task.pipeline, task.descriptorSets, task.GetResources().dynamicOffsets );
		_BindVertexBuffers( task.GetVertexBuffers(), task.GetVBOffsets() );
		
		_SetScissor( task.scissors );

		_dev.vkCmdBindIndexBuffer( _cmdBuffer,
									task.indexBuffer->Handle(),
									VkDeviceSize( task.indexBufferOffset ),
									VEnumCast( task.indexType ) );

		_dev.vkCmdDrawIndexed( _cmdBuffer,
								task.drawCmd.indexCount,
								task.drawCmd.instanceCount,
								task.drawCmd.firstIndex,
								task.drawCmd.vertexOffset,
								task.drawCmd.firstInstance );
	}
//-----------------------------------------------------------------------------



/*
=================================================
	constructor
=================================================
*/
	VTaskProcessor::VTaskProcessor (VFrameGraphThread &fg, VBarrierManager &barrierMngr, VkCommandBuffer cmdbuf, const CommandBatchID &batchId, uint indexInBatch) :
		_frameGraph{ fg },		_dev{ fg.GetDevice() },
		_cmdBuffer{ cmdbuf },	_isDebugMarkerSupported{ _dev.EnableDebugMarkers() },
		_pendingBufferBarriers{ fg.GetAllocator() },
		_pendingImageBarriers{ fg.GetAllocator() },
		_barrierMngr{ barrierMngr }
	{
		ASSERT( _cmdBuffer );

		_CmdPushDebugGroup( "SubBatch: "s << batchId.GetName() << ", index: " << ToString(indexInBatch) );
	}
	
/*
=================================================
	destructor
=================================================
*/
	VTaskProcessor::~VTaskProcessor ()
	{
		_CmdPopDebugGroup();
	}
	
/*
=================================================
	Visit*_DrawTask
=================================================
*/
	void VTaskProcessor::Visit1_DrawTask (void *visitor, void *taskData)
	{
		static_cast<DrawTaskBarriers *>(visitor)->Visit( *static_cast<VFgDrawTask<DrawTask>*>( taskData ) );
	}
	
	void VTaskProcessor::Visit2_DrawTask (void *visitor, void *taskData)
	{
		static_cast<DrawTaskCommands *>(visitor)->Visit( *static_cast<VFgDrawTask<DrawTask>*>( taskData ) );
	}
	
/*
=================================================
	Visit*_DrawIndexedTask
=================================================
*/
	void VTaskProcessor::Visit1_DrawIndexedTask (void *visitor, void *taskData)
	{
		static_cast<DrawTaskBarriers *>(visitor)->Visit( *static_cast<VFgDrawTask<DrawIndexedTask>*>( taskData ) );
	}

	void VTaskProcessor::Visit2_DrawIndexedTask (void *visitor, void *taskData)
	{
		static_cast<DrawTaskCommands *>(visitor)->Visit( *static_cast<VFgDrawTask<DrawIndexedTask>*>( taskData ) );
	}

/*
=================================================
	_CmdDebugMarker
=================================================
*/
	void VTaskProcessor::_CmdDebugMarker (StringView text) const
	{
		if ( not _isDebugMarkerSupported )
			return;

		VkDebugMarkerMarkerInfoEXT	info = {};
		info.sType			= VK_STRUCTURE_TYPE_DEBUG_MARKER_MARKER_INFO_EXT;
		info.pMarkerName	= text.data();

		_dev.vkCmdDebugMarkerInsertEXT( _cmdBuffer, &info );
	}

/*
=================================================
	_CmdPushDebugGroup
=================================================
*/
	void VTaskProcessor::_CmdPushDebugGroup (StringView text) const
	{
		if ( not _isDebugMarkerSupported )
			return;

		VkDebugMarkerMarkerInfoEXT	info = {};
		info.sType			= VK_STRUCTURE_TYPE_DEBUG_MARKER_MARKER_INFO_EXT;
		info.pMarkerName	= text.data();

		_dev.vkCmdDebugMarkerBeginEXT( _cmdBuffer, &info );
	}

/*
=================================================
	_CmdPopDebugGroup
=================================================
*/
	void VTaskProcessor::_CmdPopDebugGroup () const
	{
		if ( not _isDebugMarkerSupported )
			return;

		_dev.vkCmdDebugMarkerEndEXT( _cmdBuffer );
	}
	
/*
=================================================
	_OnRunTask
=================================================
*/
	forceinline void  VTaskProcessor::_OnRunTask (const IFrameGraphTask *task) const
	{
		_CmdDebugMarker( task->Name() );
		
		if ( _frameGraph.GetDebugger() )
			_frameGraph.GetDebugger()->RunTask( task );
	}

/*
=================================================
	_AddRenderTargetBarriers
=================================================
*/
	void VTaskProcessor::_AddRenderTargetBarriers (const VLogicalRenderPass *logicalRP, const DrawTaskBarriers &info)
	{
		if ( logicalRP->GetDepthStencilTarget().IsDefined() )
		{
			const auto &	rt		= logicalRP->GetDepthStencilTarget();
			EResourceState	state	= rt.state;
			VkImageLayout	layout	= EResourceState_ToImageLayout( rt.state );

			if ( info.IsEarlyFragmentTests() )
				state |= EResourceState::EarlyFragmentTests;

			if ( info.IsLateFragmentTests() )
				state |= EResourceState::LateFragmentTests;
			
			_AddImage( rt.imagePtr, state, layout, rt.desc );
		}

		for (const auto& rt : logicalRP->GetColorTargets())
		{
			VkImageLayout	layout = EResourceState_ToImageLayout( rt.second.state );

			_AddImage( rt.second.imagePtr, rt.second.state, layout, rt.second.desc );
		}
	}
	
/*
=================================================
	_ExtractClearValues
=================================================
*/
	void VTaskProcessor::_ExtractClearValues (const VLogicalRenderPass *logicalRP, const VRenderPass *rp, OUT VkClearValues_t &clearValues) const
	{
		clearValues.resize( logicalRP->GetColorTargets().size() + uint(logicalRP->GetDepthStencilTarget().IsDefined()) );

		if ( logicalRP->GetDepthStencilTarget().IsDefined() )
		{
			clearValues[0] = logicalRP->GetDepthStencilTarget().clearValue;
		}

		for (const auto& ct : logicalRP->GetColorTargets())
		{
			uint	index;
			CHECK( rp->GetColorAttachmentIndex( ct.first, OUT index ));

			clearValues[index] = ct.second.clearValue;
		}
	}

/*
=================================================
	_BeginRenderPass
=================================================
*/
	void VTaskProcessor::_BeginRenderPass (const VFgTask<SubmitRenderPass> &task)
	{
		ASSERT( not task.IsSubpass() );

		FixedArray< VLogicalRenderPass*, 32 >		logical_passes;

		for (auto* iter = &task; iter != null; iter = iter->GetNextSubpass())
		{
			logical_passes.push_back( iter->GetLogicalPass() );
		}

		
		// add barriers
		DrawTaskBarriers	barrier_visitor{ *this, _currTask };

		for (auto& pass : logical_passes)
		{
			for (auto& draw : pass->GetDrawTasks())
			{
				draw->Process1( &barrier_visitor );
			}
		}


		// create render pass and framebuffer
		CHECK( _frameGraph.GetResourceManager()->GetRenderPassCache()->
					CreateRenderPasses( *_frameGraph.GetResourceManager(), logical_passes, barrier_visitor.GetFragOutputs() ));
		
		VFramebuffer const*	framebuffer = _GetResource( task.GetLogicalPass()->GetFramebufferID() );

		_AddRenderTargetBarriers( task.GetLogicalPass(), barrier_visitor );
		_CommitBarriers();


		// begin render pass
		VRenderPass const*	render_pass = _GetResource( task.GetLogicalPass()->GetRenderPassID() );
		RectI const&		area		= task.GetLogicalPass()->GetArea();

		VkClearValues_t		clear_values;
		_ExtractClearValues( task.GetLogicalPass(), render_pass, OUT clear_values );

		VkRenderPassBeginInfo	pass_info = {};
		pass_info.sType						= VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		pass_info.renderPass				= render_pass->Handle();
		pass_info.renderArea.offset.x		= area.left;
		pass_info.renderArea.offset.y		= area.top;
        pass_info.renderArea.extent.width	= uint(area.Width());
        pass_info.renderArea.extent.height	= uint(area.Height());
		pass_info.clearValueCount			= uint(clear_values.size());
		pass_info.pClearValues				= clear_values.data();
		pass_info.framebuffer				= framebuffer->Handle();
		
		_dev.vkCmdBeginRenderPass( _cmdBuffer, &pass_info, VK_SUBPASS_CONTENTS_INLINE );
	}
	
/*
=================================================
	_BeginSubpass
=================================================
*/
	void VTaskProcessor::_BeginSubpass (const VFgTask<SubmitRenderPass> &task)
	{
		ASSERT( task.IsSubpass() );

		// TODO: barriers for attachments

		_dev.vkCmdNextSubpass( _cmdBuffer, VK_SUBPASS_CONTENTS_INLINE );
		/*
		// TODO
		vkCmdClearAttachments( _cmdBuffer,
							   uint( attachments.Count() ),
							   attachments.ptr(),
							   uint( clear_rects.Count() ),
							   clear_rects.ptr() );*/
	}

/*
=================================================
	Visit (SubmitRenderPass)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<SubmitRenderPass> &task)
	{
		_OnRunTask( &task );
		
		if ( task.IsSubpass() )
			_BeginSubpass( task );
		else
			_BeginRenderPass( task );

		
		// set viewports
		const auto&	viewports = task.GetLogicalPass()->GetViewports();
		if ( not viewports.empty() ) {
			_dev.vkCmdSetViewport( _cmdBuffer, 0, uint(viewports.size()), viewports.data() );
		}


		// draw
		DrawTaskCommands	command_builder{ *this, &task, _cmdBuffer };
		
		for (auto& draw : task.GetLogicalPass()->GetDrawTasks())
		{
			draw->Process2( &command_builder );
		}

		// end render pass
		if ( task.IsLastSubpass() ) {
			_dev.vkCmdEndRenderPass( _cmdBuffer );
		}
	}
	
/*
=================================================
	_ExtractDescriptorSets
=================================================
*/
	void VTaskProcessor::_ExtractDescriptorSets (const VPipelineResourceSet &resourceSet, OUT VkDescriptorSets_t &descriptorSets)
	{
		descriptorSets.resize( resourceSet.resources.size() );

		for (size_t i = 0; i < resourceSet.resources.size(); ++i)
		{
			const auto &				res		 = resourceSet.resources[i];
			VPipelineResources const*	ppln_res = _GetResource( res );
			PipelineResourceBarriers	visitor	 { *this, _currTask };

			for (auto& un : ppln_res->GetData()) {
				std::visit( visitor, un.res );
			}
			descriptorSets[i] = ppln_res->Handle();
		}
	}
	
/*
=================================================
	_BindPipelineResources
=================================================
*/
	void VTaskProcessor::_BindPipelineResources (RawCPipelineID pipelineId, const VPipelineResourceSet &resourceSet)
	{
		VComputePipeline const*		cppln	= _GetResource( pipelineId );
		VPipelineLayout const*		layout	= _GetResource( cppln->GetLayoutID() );

		// update descriptor sets and add pipeline barriers
		VkDescriptorSets_t	descriptor_sets;
		_ExtractDescriptorSets( resourceSet, OUT descriptor_sets );

		_dev.vkCmdBindDescriptorSets( _cmdBuffer,
									  VK_PIPELINE_BIND_POINT_COMPUTE,
									  layout->Handle(),
									  0,
									  uint(descriptor_sets.size()),
									  descriptor_sets.data(),
									  uint(resourceSet.dynamicOffsets.size()),
									  resourceSet.dynamicOffsets.data() );
	}
	
/*
=================================================
	_BindPipeline
=================================================
*/
	void VTaskProcessor::_BindPipeline (RawCPipelineID pipelineId, const Optional<uint3> &localSize) const
	{
		VkPipeline	ppln_id = _frameGraph.GetPipelineCache()->CreatePipelineInstance( *_frameGraph.GetResourceManager(), pipelineId, localSize, 0 );
		
		_dev.vkCmdBindPipeline( _cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ppln_id );
	}

/*
=================================================
	Visit (DispatchCompute)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<DispatchCompute> &task)
	{
		_OnRunTask( &task );

		_isCompute = true;

		_BindPipelineResources( task.pipeline, task.GetResources() );
		_BindPipeline( task.pipeline, task.localGroupSize );
		_CommitBarriers();

		_dev.vkCmdDispatch( _cmdBuffer, task.groupCount.x, task.groupCount.y, task.groupCount.z );
	}
	
/*
=================================================
	Visit (DispatchIndirectCompute)
=================================================
*
	void VTaskProcessor::Visit (const VFgTask<DispatchIndirectCompute> &task)
	{
		_OnRunTask( _currQueue, &task );

		_isCompute = true;
		
		// update descriptor sets and add pipeline barriers
		for (const auto& res : task.GetResources())
		{
			PipelineResourceBarriers	visitor{ *this, res.second, _currTask };

			for (auto& un : res.second->GetUniforms())
			{
				std::visit( visitor, un );
			}
		}
		
		VBufferPtr	indirect_buffer = Cast<VBuffer>( task.IndirectBuffer()->GetReal( _currTask, EResourceState::IndirectBuffer ));

		_AddBuffer( indirect_buffer, EResourceState::IndirectBuffer, task.IndirectBufferOffset(), VK_WHOLE_SIZE );		
		_CommitBarriers();

		
		// prepare & dispatch
		_BindPipeline( task.GetPipeline(), task.LocalSize() );
		_BindPipelineResources( task.GetPipeline(), task.GetResources() );
		
		_dev.vkCmdDispatchIndirect( _cmdBuffer,
									indirect_buffer->GetBufferID(),
									task.IndirectBufferOffset() );
	}

/*
=================================================
	Visit (CopyBuffer)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<CopyBuffer> &task)
	{
		_OnRunTask( &task );

		VLocalBuffer const *	src_buffer	= task.srcBuffer;
		VLocalBuffer const *	dst_buffer	= task.dstBuffer;
		BufferCopyRegions_t		regions;	regions.resize( task.regions.size() );

		for (size_t i = 0, count = regions.size(); i < count; ++i)
		{
			const auto&	src = task.regions[i];
			auto&		dst = regions[i];

			dst.srcOffset	= VkDeviceSize( src.srcOffset );
			dst.dstOffset	= VkDeviceSize( src.dstOffset );
			dst.size		= VkDeviceSize( src.size );

			// TODO: check buffer intersection
			ASSERT( src.size + src.srcOffset <= src_buffer->Size() );
			ASSERT( src.size + src.dstOffset <= dst_buffer->Size() );

			//if ( task.SrcBuffer()->IsMutable() or task.DstBuffer()->IsMutable() )
			{
				_AddBuffer( src_buffer, EResourceState::TransferSrc, dst.srcOffset, dst.size );
				_AddBuffer( dst_buffer, EResourceState::TransferDst, dst.dstOffset, dst.size );
			}
		}
		
		_CommitBarriers();
		
		_dev.vkCmdCopyBuffer( _cmdBuffer,
							  src_buffer->Handle(),
							  dst_buffer->Handle(),
							  uint(regions.size()),
							  regions.data() );
	}
	
/*
=================================================
	Visit (CopyImage)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<CopyImage> &task)
	{
		_OnRunTask( &task );

		VLocalImage const *		src_image	= task.srcImage;
		VLocalImage const *		dst_image	= task.dstImage;
		ImageCopyRegions_t		regions;	regions.resize( task.regions.size() );

		for (size_t i = 0, count = regions.size(); i < count; ++i)
		{
			const auto&		src					= task.regions[i];
			auto&			dst					= regions[i];
			const uint3		image_size			= Max( src.size, 1u );

			dst.srcSubresource.aspectMask		= VEnumCast( src.srcSubresource.aspectMask, src_image->PixelFormat() );
			dst.srcSubresource.mipLevel			= src.srcSubresource.mipLevel.Get();
			dst.srcSubresource.baseArrayLayer	= src.srcSubresource.baseLayer.Get();
			dst.srcSubresource.layerCount		= src.srcSubresource.layerCount;
			dst.srcOffset						= VkOffset3D{ src.srcOffset.x, src.srcOffset.y, src.srcOffset.z };

			dst.dstSubresource.aspectMask		= VEnumCast( src.dstSubresource.aspectMask, dst_image->PixelFormat() );
			dst.dstSubresource.mipLevel			= src.dstSubresource.mipLevel.Get();
			dst.dstSubresource.baseArrayLayer	= src.dstSubresource.baseLayer.Get();
			dst.dstSubresource.layerCount		= src.dstSubresource.layerCount;
			dst.dstOffset						= VkOffset3D{ src.dstOffset.x, src.dstOffset.y, src.dstOffset.z };

			dst.extent							= VkExtent3D{ image_size.x, image_size.y, image_size.z };

			ASSERT( src.srcSubresource.mipLevel < src_image->Description().maxLevel );
			ASSERT( src.dstSubresource.mipLevel < dst_image->Description().maxLevel );
			ASSERT( src.srcSubresource.baseLayer.Get() + src.srcSubresource.layerCount <= src_image->ArrayLayers() );
			ASSERT( src.srcSubresource.baseLayer.Get() + src.dstSubresource.layerCount <= dst_image->ArrayLayers() );
			//ASSERT(All( src.srcOffset + src.size <= Max(1u, src_image.Dimension().xyz() >> src.srcSubresource.mipLevel.Get()) ));
			//ASSERT(All( src.dstOffset + src.size <= Max(1u, dst_image.Dimension().xyz() >> src.dstSubresource.mipLevel.Get()) ));

			_AddImage( src_image, EResourceState::TransferSrc, task.srcLayout, dst.srcSubresource );
			_AddImage( dst_image, EResourceState::TransferDst, task.dstLayout, dst.dstSubresource );
		}
		
		_CommitBarriers();
		
		_dev.vkCmdCopyImage( _cmdBuffer,
							 src_image->Handle(),
							 task.srcLayout,
							 dst_image->Handle(),
							 task.dstLayout,
							 uint(regions.size()),
							 regions.data() );
	}
	
/*
=================================================
	Visit (CopyBufferToImage)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<CopyBufferToImage> &task)
	{
		_OnRunTask( &task );

		VLocalBuffer const *		src_buffer	= task.srcBuffer;
		VLocalImage const *			dst_image	= task.dstImage;
		BufferImageCopyRegions_t	regions;	regions.resize( task.regions.size() );
		
		for (size_t i = 0, count = regions.size(); i < count; ++i)
		{
			const auto&			src			= task.regions[i];
			auto&				dst			= regions[i];
			const int3			img_offset	= int3(src.imageOffset);
			const uint3			img_size	= Max( src.imageSize, 1u );

			dst.bufferOffset					= VkDeviceSize( src.bufferOffset );
			dst.bufferRowLength					= src.bufferRowLength;
			dst.bufferImageHeight				= src.bufferImageHeight;

			dst.imageSubresource.aspectMask		= VEnumCast( src.imageLayers.aspectMask, dst_image->PixelFormat() );
			dst.imageSubresource.mipLevel		= src.imageLayers.mipLevel.Get();
			dst.imageSubresource.baseArrayLayer	= src.imageLayers.baseLayer.Get();
			dst.imageSubresource.layerCount		= src.imageLayers.layerCount;
			dst.imageOffset						= VkOffset3D{ img_offset.x, img_offset.y, img_offset.z };
			dst.imageExtent						= VkExtent3D{ img_size.x, img_size.y, img_size.z };

			_AddBuffer( src_buffer, EResourceState::TransferSrc, dst, dst_image );
			_AddImage(  dst_image,  EResourceState::TransferDst, task.dstLayout, dst.imageSubresource );
		}
		
		_CommitBarriers();
		
		_dev.vkCmdCopyBufferToImage( _cmdBuffer,
									 src_buffer->Handle(),
									 dst_image->Handle(),
									 task.dstLayout,
									 uint(regions.size()),
									 regions.data() );
	}
	
/*
=================================================
	Visit (CopyImageToBuffer)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<CopyImageToBuffer> &task)
	{
		_OnRunTask( &task );

		VLocalImage const *			src_image	= task.srcImage;
		VLocalBuffer const *		dst_buffer	= task.dstBuffer;
		BufferImageCopyRegions_t	regions;	regions.resize( task.regions.size() );
		
		for (size_t i = 0, count = regions.size(); i < count; ++i)
		{
			const auto&			src			= task.regions[i];
			auto&				dst			= regions[i];
			const uint3			image_size	= Max( src.imageSize, 1u );

			dst.bufferOffset					= VkDeviceSize( src.bufferOffset );
			dst.bufferRowLength					= src.bufferRowLength;
			dst.bufferImageHeight				= src.bufferImageHeight;

			dst.imageSubresource.aspectMask		= VEnumCast( src.imageLayers.aspectMask, src_image->PixelFormat() );
			dst.imageSubresource.mipLevel		= src.imageLayers.mipLevel.Get();
			dst.imageSubresource.baseArrayLayer	= src.imageLayers.baseLayer.Get();
			dst.imageSubresource.layerCount		= src.imageLayers.layerCount;
			dst.imageOffset						= VkOffset3D{ src.imageOffset.x, src.imageOffset.y, src.imageOffset.z };
			dst.imageExtent						= VkExtent3D{ image_size.x, image_size.y, image_size.z };

			_AddImage(  src_image,  EResourceState::TransferSrc, task.srcLayout, dst.imageSubresource );
			_AddBuffer( dst_buffer, EResourceState::TransferDst, dst, src_image );
		}
		
		_CommitBarriers();
		
		_dev.vkCmdCopyImageToBuffer( _cmdBuffer,
									 src_image->Handle(),
									 task.srcLayout,
									 dst_buffer->Handle(),
									 uint(regions.size()),
									 regions.data() );
	}
	
/*
=================================================
	Visit (BlitImage)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<BlitImage> &task)
	{
		_OnRunTask( &task );

		VLocalImage const *		src_image	= task.srcImage;
		VLocalImage const *		dst_image	= task.dstImage;
		BlitRegions_t			regions;	regions.resize( task.regions.size() );
		
		for (size_t i = 0, count = regions.size(); i < count; ++i)
		{
			const auto&		src	= task.regions[i];
			auto&			dst	= regions[i];
				
			dst.srcSubresource.aspectMask		= VEnumCast( src.srcSubresource.aspectMask, src_image->PixelFormat() );
			dst.srcSubresource.mipLevel			= src.srcSubresource.mipLevel.Get();
			dst.srcSubresource.baseArrayLayer	= src.srcSubresource.baseLayer.Get();
			dst.srcSubresource.layerCount		= src.srcSubresource.layerCount;
			dst.srcOffsets[0]					= VkOffset3D{ src.srcOffset0.x, src.srcOffset0.y, src.srcOffset0.z };
			dst.srcOffsets[1]					= VkOffset3D{ src.srcOffset1.x, src.srcOffset1.y, src.srcOffset1.z };
			
			dst.dstSubresource.aspectMask		= VEnumCast( src.dstSubresource.aspectMask, dst_image->PixelFormat() );
			dst.dstSubresource.mipLevel			= src.dstSubresource.mipLevel.Get();
			dst.dstSubresource.baseArrayLayer	= src.dstSubresource.baseLayer.Get();
			dst.dstSubresource.layerCount		= src.dstSubresource.layerCount;
			dst.dstOffsets[0]					= VkOffset3D{ src.dstOffset0.x, src.dstOffset0.y, src.dstOffset0.z };
			dst.dstOffsets[1]					= VkOffset3D{ src.dstOffset1.x, src.dstOffset1.y, src.dstOffset1.z };

			_AddImage( src_image, EResourceState::TransferSrc, task.srcLayout, dst.srcSubresource );
			_AddImage( dst_image, EResourceState::TransferDst, task.dstLayout, dst.dstSubresource );
		}
		
		_CommitBarriers();
		
		_dev.vkCmdBlitImage( _cmdBuffer,
							 src_image->Handle(),
							 task.srcLayout,
							 dst_image->Handle(),
							 task.dstLayout,
							 uint(regions.size()),
							 regions.data(),
							 task.filter );
	}
	
/*
=================================================
	Visit (ResolveImage)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<ResolveImage> &task)
	{
		_OnRunTask( &task );
		
		VLocalImage	const *		src_image	= task.srcImage;
		VLocalImage const *		dst_image	= task.dstImage;
		ResolveRegions_t		regions;	regions.resize( task.regions.size() );
		
		for (size_t i = 0, count = regions.size(); i < count; ++i)
		{
			const auto&			src				= task.regions[i];
			auto&				dst				= regions[i];
			const uint3			image_size		= Max( src.extent, 1u );

			dst.srcSubresource.aspectMask		= VEnumCast( src.srcSubresource.aspectMask, src_image->PixelFormat() );
			dst.srcSubresource.mipLevel			= src.srcSubresource.mipLevel.Get();
			dst.srcSubresource.baseArrayLayer	= src.srcSubresource.baseLayer.Get();
			dst.srcSubresource.layerCount		= src.srcSubresource.layerCount;
			dst.srcOffset						= VkOffset3D{ src.srcOffset.x, src.srcOffset.y, src.srcOffset.z };
			
			dst.dstSubresource.aspectMask		= VEnumCast( src.dstSubresource.aspectMask, dst_image->PixelFormat() );
			dst.dstSubresource.mipLevel			= src.dstSubresource.mipLevel.Get();
			dst.dstSubresource.baseArrayLayer	= src.dstSubresource.baseLayer.Get();
			dst.dstSubresource.layerCount		= src.dstSubresource.layerCount;
			dst.dstOffset						= VkOffset3D{ src.dstOffset.x, src.dstOffset.y, src.dstOffset.z };
			
			dst.extent							= VkExtent3D{ image_size.x, image_size.y, image_size.z };

			_AddImage( src_image, EResourceState::TransferSrc, task.srcLayout, dst.srcSubresource );
			_AddImage( dst_image, EResourceState::TransferDst, task.dstLayout, dst.dstSubresource );
		}
		
		_CommitBarriers();
		
		_dev.vkCmdResolveImage(	_cmdBuffer,
								src_image->Handle(),
								task.srcLayout,
								dst_image->Handle(),
								task.dstLayout,
								uint(regions.size()),
								regions.data() );
	}
	
/*
=================================================
	Visit (FillBuffer)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<FillBuffer> &task)
	{
		_OnRunTask( &task );

		VLocalBuffer const *	dst_buffer = task.dstBuffer;

		_AddBuffer( dst_buffer, EResourceState::TransferDst, task.dstOffset, task.size );
		
		_CommitBarriers();
		
		_dev.vkCmdFillBuffer( _cmdBuffer,
							  dst_buffer->Handle(),
							  task.dstOffset,
							  task.size,
							  task.pattern );
	}
	
/*
=================================================
	Visit (ClearColorImage)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<ClearColorImage> &task)
	{
		_OnRunTask( &task );

		VLocalImage const *		dst_image	= task.dstImage;
		ImageClearRanges_t		ranges;		ranges.resize( task.ranges.size() );
		
		for (size_t i = 0, count = ranges.size(); i < count; ++i)
		{
			const auto&		src	= task.ranges[i];
			auto&			dst	= ranges[i];
			
			dst.aspectMask		= VK_IMAGE_ASPECT_COLOR_BIT;
			dst.baseMipLevel	= src.baseMipLevel.Get();
			dst.levelCount		= src.levelCount;
			dst.baseArrayLayer	= src.baseLayer.Get();
			dst.layerCount		= src.layerCount;

			_AddImage( dst_image, EResourceState::TransferDst, task.dstLayout, dst );
		}
		
		_CommitBarriers();
		
		_dev.vkCmdClearColorImage( _cmdBuffer,
								   dst_image->Handle(),
								   task.dstLayout,
								   &task.ClearValue(),
								   uint(ranges.size()),
								   ranges.data() );
	}
	
/*
=================================================
	Visit (ClearDepthStencilImage)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<ClearDepthStencilImage> &task)
	{
		_OnRunTask( &task );
		
		VLocalImage const *		dst_image	= task.dstImage;
		ImageClearRanges_t		ranges;		ranges.resize( task.ranges.size() );
		
		for (size_t i = 0, count = ranges.size(); i < count; ++i)
		{
			const auto&		src	= task.ranges[i];
			auto&			dst	= ranges[i];
			
			dst.aspectMask		= VEnumCast( src.aspectMask, dst_image->PixelFormat() );
			dst.baseMipLevel	= src.baseMipLevel.Get();
			dst.levelCount		= src.levelCount;
			dst.baseArrayLayer	= src.baseLayer.Get();
			dst.layerCount		= src.layerCount;

			_AddImage( dst_image, EResourceState::TransferDst, task.dstLayout, dst );
		}
		
		_CommitBarriers();
		
		_dev.vkCmdClearDepthStencilImage( _cmdBuffer,
										  dst_image->Handle(),
										  task.dstLayout,
										  &task.clearValue,
										  uint(ranges.size()),
										  ranges.data() );
	}

/*
=================================================
	Visit (UpdateBuffer)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<UpdateBuffer> &task)
	{
		_OnRunTask( &task );
		
		VLocalBuffer const *	dst_buffer = task.dstBuffer;

		_AddBuffer( dst_buffer, EResourceState::TransferDst, task.dstOffset, task.DataSize() );
		
		_CommitBarriers();
		
		_dev.vkCmdUpdateBuffer( _cmdBuffer,
								dst_buffer->Handle(),
								task.dstOffset,
								task.DataSize(),
								task.DataPtr() );
	}

/*
=================================================
	Visit (Present)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<Present> &task)
	{
		_OnRunTask( &task );

		/*_AddImage( task.GetImage(), EResourceState::TransferSrc | EResourceState::_InvalidateAfter, EImageLayout::TransferSrcOptimal,
				   RangeU(0, 1) + task.GetLayer().Get(), RangeU(0, 1), EImageAspect::Color );

		GpuMsg::ThreadBeginFrame	begin_frame;
		_gpuThread->Send( begin_frame );

		_barrierMngr.AddBarrier( GpuMsg::CmdPipelineBarrier{}.AddImage({}) );

		_cmdBuilder->Send( GpuMsg::CmdBlitImage{ task.GetImage(), EImageLayout::TransferSrcOptimal,
												 begin_frame.result->image, EImageLayout::TransferDstOptimal, true }
								.AddRegion(	{}, uint3(), uint3(),
											{}, uint3(), uint3() ));
											
		_CommitBarriers();
		
		_gpuThread->Send( GpuMsg::ThreadEndFrame{} );

		// TODO*/
	}

/*
=================================================
	Visit (PresentVR)
=================================================
*/
	void VTaskProcessor::Visit (const VFgTask<PresentVR> &task)
	{
		_OnRunTask( &task );

		/*_AddImage( task.GetLeftEyeImage(), EResourceState::PresentImage | EResourceState::_InvalidateAfter, EImageLayout::PresentSrc,
				   ImageRange{ task.GetLeftEyeLayer(), 1, 0_mipmap, 1 }, EImageAspect::Color );
		
		_AddImage( task.GetRightEyeImage(), EResourceState::PresentImage | EResourceState::_InvalidateAfter, EImageLayout::PresentSrc,
				   ImageRange{ task.GetRightEyeLayer(), 1, 0_mipmap, 1 }, EImageAspect::Color );

		_CommitBarriers();*/
		
		// TODO
	}
	
/*
=================================================
	_AddImageState
=================================================
*/
	inline void VTaskProcessor::_AddImageState (const VLocalImage *img, const ImageState &state)
	{
		ASSERT( not state.range.IsEmpty() );

		_pendingImageBarriers.insert( img );

		img->AddPendingState( state );

		if ( _frameGraph.GetDebugger() )
			_frameGraph.GetDebugger()->AddImageUsage( img->ToGlobal(), state );
	}
	
/*
=================================================
	_AddImage
=================================================
*/
	inline void VTaskProcessor::_AddImage (const VLocalImage *img, EResourceState state, VkImageLayout layout, const ImageViewDesc &desc)
	{
		ASSERT( desc.layerCount > 0 and desc.levelCount > 0 );

		_AddImageState( img,
						ImageState{
							state, layout,
							ImageRange{ desc.baseLayer, desc.layerCount, desc.baseLevel, desc.levelCount },
							(EPixelFormat_HasDepth( desc.format )   ? VK_IMAGE_ASPECT_DEPTH_BIT   :
							 EPixelFormat_HasStencil( desc.format ) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT) | 0u,
							_currTask
						});
	}

/*
=================================================
	_AddImage
=================================================
*/
	inline void VTaskProcessor::_AddImage (const VLocalImage *img, EResourceState state, VkImageLayout layout, const VkImageSubresourceLayers &subres)
	{
		_AddImageState( img,
						ImageState{
							state, layout,
							ImageRange{ ImageLayer(subres.baseArrayLayer), subres.layerCount, MipmapLevel(subres.mipLevel), 1 },
							subres.aspectMask,
							_currTask
						});
	}
	
/*
=================================================
	_AddImage
=================================================
*/
	inline void VTaskProcessor::_AddImage (const VLocalImage *img, EResourceState state, VkImageLayout layout, const VkImageSubresourceRange &subres)
	{
		_AddImageState( img,
						ImageState{
							state, layout,
							ImageRange{ ImageLayer(subres.baseArrayLayer), subres.layerCount, MipmapLevel(subres.baseMipLevel), subres.levelCount },
							subres.aspectMask,
							_currTask
						});
	}

/*
=================================================
	_AddBufferState
=================================================
*/
	inline void VTaskProcessor::_AddBufferState (const VLocalBuffer *buf, const BufferState &state)
	{
		_pendingBufferBarriers.insert( buf );

		buf->AddPendingState( state );
		
		if ( _frameGraph.GetDebugger() )
			_frameGraph.GetDebugger()->AddBufferUsage( buf->ToGlobal(), state );
	}

/*
=================================================
	_AddBuffer
=================================================
*/
	inline void VTaskProcessor::_AddBuffer (const VLocalBuffer *buf, EResourceState state, VkDeviceSize offset, VkDeviceSize size)
	{
		ASSERT( size > 0 );

		//if ( buf->IsImmutable() )
		//	return;

		const VkDeviceSize	buf_size = VkDeviceSize(buf->Size());
		
		size = Min( buf_size, (size == VK_WHOLE_SIZE ? buf_size - offset : offset + size) );

		_AddBufferState( buf, BufferState{ state, offset, size, _currTask });
	}
	
/*
=================================================
	_AddBuffer
=================================================
*/
	inline void VTaskProcessor::_AddBuffer (const VLocalBuffer *buf, EResourceState state,
											const VkBufferImageCopy &reg, const VLocalImage *img)
	{
		//if ( buf->IsImmutable() )
		//	return;

		const uint			bpp			= EPixelFormat_BitPerPixel( img->PixelFormat(), VEnumRevert(reg.imageSubresource.aspectMask) );
		const VkDeviceSize	row_size	= (reg.imageExtent.width * bpp) / 8;
		const VkDeviceSize	row_pitch	= (reg.bufferRowLength * bpp) / 8;
		const VkDeviceSize	slice_pitch	= reg.bufferImageHeight * row_pitch;
		const uint			dim_z		= Max( reg.imageSubresource.layerCount, reg.imageExtent.depth );
		BufferState			buf_state	{ state, 0, row_size, _currTask };

#if 1
		// one big barrier
		buf_state.range = BufferRange{ 0, slice_pitch * dim_z } + reg.bufferOffset;
		
		_AddBufferState( buf, buf_state );
#else
		for (uint z = 0; z < dim_z; ++z)
		{
			for (uint y = 0; y < reg.imageExtent.height; ++y)
			{
				buf_state.range = BufferRange{0, row_size} + (reg.bufferOffset + z * slice_pitch + y * row_pitch);
				
				_AddBufferState( buf, buf_state );
			}
		}
#endif
	}
	
/*
=================================================
	_CommitBarriers
=================================================
*/
	inline void VTaskProcessor::_CommitBarriers ()
	{
		for (auto& buf : _pendingBufferBarriers)
		{
			buf->CommitBarrier( _barrierMngr, _frameGraph.GetDebugger() );
		}

		for (auto& img : _pendingImageBarriers)
		{
			img->CommitBarrier( _barrierMngr, _frameGraph.GetDebugger() );
		}

		_pendingBufferBarriers.clear();
		_pendingImageBarriers.clear();

		_barrierMngr.Commit( _dev, _cmdBuffer );
	}

	
}	// FG
