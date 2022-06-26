#include "graphics/command_buffer.hpp"
#include "graphics/gpu_device.hpp"

#include "foundation/memory.hpp"

namespace raptor {


void CommandBuffer::reset() {

    is_recording = false;
    current_render_pass = nullptr;
    current_pipeline = nullptr;
    current_command = 0;

    vkResetDescriptorPool( device->vulkan_device, vk_descriptor_pool, 0 );

    u32 resource_count = descriptor_sets.free_indices_head;
    for ( u32 i = 0; i < resource_count; ++i) {
        DesciptorSet* v_descriptor_set = ( DesciptorSet* )descriptor_sets.access_resource( i );

        if ( v_descriptor_set ) {
            // Contains the allocation for all the resources, binding and samplers arrays.
            rfree( v_descriptor_set->resources, device->allocator );
        }
        descriptor_sets.release_resource( i );
    }
}


void CommandBuffer::init( QueueType::Enum type_, u32 buffer_size_, u32 submit_size, bool baked_ ) {
    // TODO(marco): are these still needed?
    type = type_;
    buffer_size = buffer_size_;
    baked = baked_;

     ////////  Create Descriptor Pools
    static const u32 k_global_pool_elements = 128;
    VkDescriptorPoolSize pool_sizes[] =
    {
        { VK_DESCRIPTOR_TYPE_SAMPLER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, k_global_pool_elements}
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = k_global_pool_elements * ArraySize( pool_sizes );
    pool_info.poolSizeCount = ( u32 )ArraySize( pool_sizes );
    pool_info.pPoolSizes = pool_sizes;
    VkResult result = vkCreateDescriptorPool( device->vulkan_device, &pool_info, device->vulkan_allocation_callbacks, &vk_descriptor_pool );
    RASSERT( result == VK_SUCCESS );

    descriptor_sets.init( device->allocator, 256, sizeof( DesciptorSet ) );

    reset();
}

void CommandBuffer::terminate() {

    is_recording = false;

    reset();

    descriptor_sets.shutdown();

    vkDestroyDescriptorPool( device->vulkan_device, vk_descriptor_pool, device->vulkan_allocation_callbacks );
}

DescriptorSetHandle CommandBuffer::create_descriptor_set( const DescriptorSetCreation& creation ) {
    DescriptorSetHandle handle = { descriptor_sets.obtain_resource() };
    if ( handle.index == k_invalid_index ) {
        return handle;
    }

    DesciptorSet* descriptor_set = ( DesciptorSet* )descriptor_sets.access_resource( handle.index );
    const DesciptorSetLayout* descriptor_set_layout = device->access_descriptor_set_layout( creation.layout );

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    alloc_info.descriptorPool = vk_descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_set_layout->vk_descriptor_set_layout;

    VkResult result = vkAllocateDescriptorSets( device->vulkan_device, &alloc_info, &descriptor_set->vk_descriptor_set );
    RASSERT( result == VK_SUCCESS );

    // Cache data
    u8* memory = rallocam( ( sizeof( ResourceHandle ) + sizeof( SamplerHandle ) + sizeof( u16 ) ) * creation.num_resources, device->allocator );
    descriptor_set->resources = ( ResourceHandle* )memory;
    descriptor_set->samplers = ( SamplerHandle* )( memory + sizeof( ResourceHandle ) * creation.num_resources );
    descriptor_set->bindings = ( u16* )( memory + ( sizeof( ResourceHandle ) + sizeof( SamplerHandle ) ) * creation.num_resources );
    descriptor_set->num_resources = creation.num_resources;
    descriptor_set->layout = descriptor_set_layout;

    // Update descriptor set
    VkWriteDescriptorSet descriptor_write[ 8 ];
    VkDescriptorBufferInfo buffer_info[ 8 ];
    VkDescriptorImageInfo image_info[ 8 ];

    Sampler* vk_default_sampler = device->access_sampler( device->default_sampler );

    u32 num_resources = creation.num_resources;
    GpuDevice::fill_write_descriptor_sets( *device, descriptor_set_layout, descriptor_set->vk_descriptor_set, descriptor_write, buffer_info, image_info, vk_default_sampler->vk_sampler,
                                           num_resources, creation.resources, creation.samplers, creation.bindings );

    // Cache resources
    for ( u32 r = 0; r < creation.num_resources; r++ ) {
        descriptor_set->resources[ r ] = creation.resources[ r ];
        descriptor_set->samplers[ r ] = creation.samplers[ r ];
        descriptor_set->bindings[ r ] = creation.bindings[ r ];
    }

    vkUpdateDescriptorSets( device->vulkan_device, num_resources, descriptor_write, 0, nullptr );

    return handle;
}

void CommandBuffer::bind_pass( RenderPassHandle handle_ ) {

    //if ( !is_recording )
    {
        is_recording = true;

        RenderPass* render_pass = device->access_render_pass( handle_ );

        // Begin/End render pass are valid only for graphics render passes.
        if ( current_render_pass && ( current_render_pass->type != RenderPassType::Compute ) && ( render_pass != current_render_pass ) ) {
            vkCmdEndRenderPass( vk_command_buffer );
        }

        if ( render_pass != current_render_pass && ( render_pass->type != RenderPassType::Compute ) ) {
            VkRenderPassBeginInfo render_pass_begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            render_pass_begin.framebuffer = render_pass->type == RenderPassType::Swapchain ? device->vulkan_swapchain_framebuffers[device->vulkan_image_index] : render_pass->vk_frame_buffer;
            render_pass_begin.renderPass = render_pass->vk_render_pass;

            render_pass_begin.renderArea.offset = { 0, 0 };
            render_pass_begin.renderArea.extent = { render_pass->width, render_pass->height };

            // TODO: this breaks.
            render_pass_begin.clearValueCount = 2;// render_pass->output.color_operation ? 2 : 0;
            render_pass_begin.pClearValues = clears;

            vkCmdBeginRenderPass( vk_command_buffer, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE );
        }

        // Cache render pass
        current_render_pass = render_pass;
    }
}

void CommandBuffer::bind_pipeline( PipelineHandle handle_ ) {

    Pipeline* pipeline = device->access_pipeline( handle_ );
    vkCmdBindPipeline( vk_command_buffer, pipeline->vk_bind_point, pipeline->vk_pipeline );

    // Cache pipeline
    current_pipeline = pipeline;
}

void CommandBuffer::bind_vertex_buffer( BufferHandle handle_, u32 binding, u32 offset ) {

    Buffer* buffer = device->access_buffer( handle_ );
    VkDeviceSize offsets[] = { offset };

    VkBuffer vk_buffer = buffer->vk_buffer;
    // TODO: add global vertex buffer ?
    if ( buffer->parent_buffer.index != k_invalid_index ) {
        Buffer* parent_buffer = device->access_buffer( buffer->parent_buffer );
        vk_buffer = parent_buffer->vk_buffer;
        offsets[ 0 ] = buffer->global_offset;
    }

    vkCmdBindVertexBuffers( vk_command_buffer, binding, 1, &vk_buffer, offsets );
}

void CommandBuffer::bind_index_buffer( BufferHandle handle_, u32 offset_ ) {

    Buffer* buffer = device->access_buffer( handle_ );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize offset = offset_;
    if ( buffer->parent_buffer.index != k_invalid_index ) {
        Buffer* parent_buffer = device->access_buffer( buffer->parent_buffer );
        vk_buffer = parent_buffer->vk_buffer;
        offset = buffer->global_offset;
    }
    vkCmdBindIndexBuffer( vk_command_buffer, vk_buffer, offset, VkIndexType::VK_INDEX_TYPE_UINT16 );
}

void CommandBuffer::bind_descriptor_set( DescriptorSetHandle* handles, u32 num_lists, u32* offsets, u32 num_offsets ) {

    // TODO:
    u32 offsets_cache[ 8 ];
    num_offsets = 0;

    for ( u32 l = 0; l < num_lists; ++l ) {
        DesciptorSet* descriptor_set = device->access_descriptor_set( handles[l] );
        vk_descriptor_sets[l] = descriptor_set->vk_descriptor_set;

        // Search for dynamic buffers
        const DesciptorSetLayout* descriptor_set_layout = descriptor_set->layout;
        for ( u32 i = 0; i < descriptor_set_layout->num_bindings; ++i ) {
            const DescriptorBinding& rb = descriptor_set_layout->bindings[ i ];

            if ( rb.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) {
                // Search for the actual buffer offset
                const u32 resource_index = descriptor_set->bindings[ i ];
                ResourceHandle buffer_handle = descriptor_set->resources[ resource_index ];
                Buffer* buffer = device->access_buffer( { buffer_handle } );

                offsets_cache[ num_offsets++ ] = buffer->global_offset;
            }
        }
    }

    const u32 k_first_set = 0;
    vkCmdBindDescriptorSets( vk_command_buffer, current_pipeline->vk_bind_point, current_pipeline->vk_pipeline_layout, k_first_set,
                             num_lists, vk_descriptor_sets, num_offsets, offsets_cache );

    if ( device->bindless_supported ) {
        vkCmdBindDescriptorSets( vk_command_buffer, current_pipeline->vk_bind_point, current_pipeline->vk_pipeline_layout, 1,
                                 1, &device->vulkan_bindless_descriptor_set, 0, nullptr );
    }
}

void CommandBuffer::bind_local_descriptor_set( DescriptorSetHandle* handles, u32 num_lists, u32* offsets, u32 num_offsets ) {

    // TODO:
    u32 offsets_cache[ 8 ];
    num_offsets = 0;

    for ( u32 l = 0; l < num_lists; ++l ) {
        DesciptorSet* descriptor_set = ( DesciptorSet* )descriptor_sets.access_resource( handles[ l ].index );
        vk_descriptor_sets[l] = descriptor_set->vk_descriptor_set;

        // Search for dynamic buffers
        const DesciptorSetLayout* descriptor_set_layout = descriptor_set->layout;
        for ( u32 i = 0; i < descriptor_set_layout->num_bindings; ++i ) {
            const DescriptorBinding& rb = descriptor_set_layout->bindings[ i ];

            if ( rb.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) {
                // Search for the actual buffer offset
                const u32 resource_index = descriptor_set->bindings[ i ];
                ResourceHandle buffer_handle = descriptor_set->resources[ resource_index ];
                Buffer* buffer = device->access_buffer( { buffer_handle } );

                offsets_cache[ num_offsets++ ] = buffer->global_offset;
            }
        }
    }

    const u32 k_first_set = 0;
    vkCmdBindDescriptorSets( vk_command_buffer, current_pipeline->vk_bind_point, current_pipeline->vk_pipeline_layout, k_first_set,
                             num_lists, vk_descriptor_sets, num_offsets, offsets_cache );

    if ( device->bindless_supported ) {
        vkCmdBindDescriptorSets( vk_command_buffer, current_pipeline->vk_bind_point, current_pipeline->vk_pipeline_layout, 1,
                                 1, &device->vulkan_bindless_descriptor_set, 0, nullptr );
    }
}

void CommandBuffer::set_viewport( const Viewport* viewport ) {

    VkViewport vk_viewport;

    if ( viewport ) {
        vk_viewport.x = viewport->rect.x * 1.f;
        vk_viewport.width = viewport->rect.width * 1.f;
        // Invert Y with negative height and proper offset - Vulkan has unique Clipping Y.
        vk_viewport.y = viewport->rect.height * 1.f - viewport->rect.y;
        vk_viewport.height = -viewport->rect.height * 1.f;
        vk_viewport.minDepth = viewport->min_depth;
        vk_viewport.maxDepth = viewport->max_depth;
    }
    else {
        vk_viewport.x = 0.f;

        if ( current_render_pass ) {
            vk_viewport.width = current_render_pass->width * 1.f;
            // Invert Y with negative height and proper offset - Vulkan has unique Clipping Y.
            vk_viewport.y = current_render_pass->height * 1.f;
            vk_viewport.height = -current_render_pass->height * 1.f;
        }
        else {
            vk_viewport.width = device->swapchain_width * 1.f;
            // Invert Y with negative height and proper offset - Vulkan has unique Clipping Y.
            vk_viewport.y = device->swapchain_height * 1.f;
            vk_viewport.height = -device->swapchain_height * 1.f;
        }
        vk_viewport.minDepth = 0.0f;
        vk_viewport.maxDepth = 1.0f;
    }

    vkCmdSetViewport( vk_command_buffer, 0, 1, &vk_viewport);
}

void CommandBuffer::set_scissor( const Rect2DInt* rect ) {

    VkRect2D vk_scissor;

    if ( rect ) {
        vk_scissor.offset.x = rect->x;
        vk_scissor.offset.y = rect->y;
        vk_scissor.extent.width = rect->width;
        vk_scissor.extent.height = rect->height;
    }
    else {
        vk_scissor.offset.x = 0;
        vk_scissor.offset.y = 0;
        vk_scissor.extent.width = device->swapchain_width;
        vk_scissor.extent.height = device->swapchain_height;
    }

    vkCmdSetScissor( vk_command_buffer, 0, 1, &vk_scissor );
}

void CommandBuffer::clear( f32 red, f32 green, f32 blue, f32 alpha ) {
    clears[0].color = { red, green, blue, alpha };
}

void CommandBuffer::clear_depth_stencil( f32 depth, u8 value ) {
    clears[1].depthStencil.depth = depth;
    clears[1].depthStencil.stencil = value;
}

void CommandBuffer::draw( TopologyType::Enum topology, u32 first_vertex, u32 vertex_count, u32 first_instance, u32 instance_count ) {
    vkCmdDraw( vk_command_buffer, vertex_count, instance_count, first_vertex, first_instance );
}

void CommandBuffer::draw_indexed( TopologyType::Enum topology, u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset, u32 first_instance ) {
    vkCmdDrawIndexed( vk_command_buffer, index_count, instance_count, first_index, vertex_offset, first_instance );
}

void CommandBuffer::dispatch( u32 group_x, u32 group_y, u32 group_z ) {
    vkCmdDispatch( vk_command_buffer, group_x, group_y, group_z );
}

void CommandBuffer::draw_indirect( BufferHandle buffer_handle, u32 offset, u32 stride ) {

    Buffer* buffer = device->access_buffer( buffer_handle );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize vk_offset = offset;

    vkCmdDrawIndirect( vk_command_buffer, vk_buffer, vk_offset, 1, sizeof( VkDrawIndirectCommand ) );
}

void CommandBuffer::draw_indexed_indirect( BufferHandle buffer_handle, u32 offset, u32 stride ) {
    Buffer* buffer = device->access_buffer( buffer_handle );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize vk_offset = offset;

    vkCmdDrawIndexedIndirect( vk_command_buffer, vk_buffer, vk_offset, 1, sizeof( VkDrawIndirectCommand ) );
}

void CommandBuffer::dispatch_indirect( BufferHandle buffer_handle, u32 offset ) {
    Buffer* buffer = device->access_buffer( buffer_handle );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize vk_offset = offset;

    vkCmdDispatchIndirect( vk_command_buffer, vk_buffer, vk_offset );
}

// DrawIndirect = 0, VertexInput = 1, VertexShader = 2, FragmentShader = 3, RenderTarget = 4, ComputeShader = 5, Transfer = 6
static ResourceState to_resource_state( PipelineStage::Enum stage ) {
    static ResourceState s_states[] = { RESOURCE_STATE_INDIRECT_ARGUMENT, RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_COPY_DEST };
    return s_states[ stage ];
}

void CommandBuffer::barrier( const ExecutionBarrier& barrier ) {

    if ( current_render_pass && ( current_render_pass->type != RenderPassType::Compute ) ) {
        vkCmdEndRenderPass( vk_command_buffer );

        current_render_pass = nullptr;
    }

    static VkImageMemoryBarrier image_barriers[ 8 ];
    // TODO: subpass
    if ( barrier.new_barrier_experimental != u32_max ) {

        VkPipelineStageFlags source_stage_mask = 0;
        VkPipelineStageFlags destination_stage_mask = 0;
        VkAccessFlags source_access_flags = VK_ACCESS_NONE_KHR, destination_access_flags = VK_ACCESS_NONE_KHR;

        for ( u32 i = 0; i < barrier.num_image_barriers; ++i ) {

            Texture* texture_vulkan = device->access_texture( barrier.image_barriers[ i ].texture );

            VkImageMemoryBarrier& vk_barrier = image_barriers[ i ];
            const bool is_color = !TextureFormat::has_depth_or_stencil( texture_vulkan->vk_format );

            {
                VkImageMemoryBarrier* pImageBarrier = &vk_barrier;
                pImageBarrier->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                pImageBarrier->pNext = NULL;

                ResourceState current_state = barrier.source_pipeline_stage == PipelineStage::RenderTarget ? RESOURCE_STATE_RENDER_TARGET : RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                ResourceState next_state = barrier.destination_pipeline_stage == PipelineStage::RenderTarget ? RESOURCE_STATE_RENDER_TARGET : RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                if ( !is_color ) {
                    current_state = barrier.source_pipeline_stage == PipelineStage::RenderTarget ? RESOURCE_STATE_DEPTH_WRITE : RESOURCE_STATE_DEPTH_READ;
                    next_state = barrier.destination_pipeline_stage == PipelineStage::RenderTarget ? RESOURCE_STATE_DEPTH_WRITE : RESOURCE_STATE_DEPTH_READ;
                }

                pImageBarrier->srcAccessMask = util_to_vk_access_flags( current_state );
                pImageBarrier->dstAccessMask = util_to_vk_access_flags( next_state );
                pImageBarrier->oldLayout = util_to_vk_image_layout( current_state );
                pImageBarrier->newLayout = util_to_vk_image_layout( next_state );

                pImageBarrier->image = texture_vulkan->vk_image;
                pImageBarrier->subresourceRange.aspectMask = is_color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                pImageBarrier->subresourceRange.baseMipLevel = 0;
                pImageBarrier->subresourceRange.levelCount = 1;
                pImageBarrier->subresourceRange.baseArrayLayer = 0;
                pImageBarrier->subresourceRange.layerCount = 1;

                {
                    pImageBarrier->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    pImageBarrier->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                }

                source_access_flags |= pImageBarrier->srcAccessMask;
                destination_access_flags |= pImageBarrier->dstAccessMask;
            }

            vk_barrier.oldLayout = texture_vulkan->vk_image_layout;
            texture_vulkan->vk_image_layout = vk_barrier.newLayout;
        }

        static VkBufferMemoryBarrier buffer_memory_barriers[ 8 ];
        for ( u32 i = 0; i < barrier.num_memory_barriers; ++i ) {
            VkBufferMemoryBarrier& vk_barrier = buffer_memory_barriers[ i ];
            vk_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;

            Buffer* buffer = device->access_buffer( barrier.memory_barriers[ i ].buffer );

            vk_barrier.buffer = buffer->vk_buffer;
            vk_barrier.offset = 0;
            vk_barrier.size = buffer->size;

            ResourceState current_state = to_resource_state( barrier.source_pipeline_stage );
            ResourceState next_state = to_resource_state( barrier.destination_pipeline_stage );
            vk_barrier.srcAccessMask = util_to_vk_access_flags( current_state );
            vk_barrier.dstAccessMask = util_to_vk_access_flags( next_state );

            source_access_flags |= vk_barrier.srcAccessMask;
            destination_access_flags |= vk_barrier.dstAccessMask;

            vk_barrier.srcQueueFamilyIndex = 0;
            vk_barrier.dstQueueFamilyIndex = 0;
        }

        source_stage_mask = util_determine_pipeline_stage_flags( source_access_flags, barrier.source_pipeline_stage == PipelineStage::ComputeShader ? QueueType::Compute : QueueType::Graphics );
        destination_stage_mask = util_determine_pipeline_stage_flags( destination_access_flags, barrier.destination_pipeline_stage == PipelineStage::ComputeShader ? QueueType::Compute : QueueType::Graphics );

        vkCmdPipelineBarrier( vk_command_buffer, source_stage_mask, destination_stage_mask, 0, 0, nullptr, barrier.num_memory_barriers, buffer_memory_barriers, barrier.num_image_barriers, image_barriers );
        return;
    }

    VkImageLayout new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageLayout new_depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAccessFlags source_access_mask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkAccessFlags source_buffer_access_mask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkAccessFlags source_depth_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkAccessFlags destination_access_mask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkAccessFlags destination_buffer_access_mask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkAccessFlags destination_depth_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    switch ( barrier.destination_pipeline_stage ) {

        case PipelineStage::FragmentShader:
        {
            //new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            break;
        }

        case PipelineStage::ComputeShader:
        {
            new_layout = VK_IMAGE_LAYOUT_GENERAL;


            break;
        }

        case PipelineStage::RenderTarget:
        {
            new_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            new_depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            destination_access_mask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            destination_depth_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

            break;
        }

        case PipelineStage::DrawIndirect:
        {
            destination_buffer_access_mask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            break;
        }
    }

    switch ( barrier.source_pipeline_stage ) {

        case PipelineStage::FragmentShader:
        {
            //source_access_mask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            break;
        }

        case PipelineStage::ComputeShader:
        {

            break;
        }

        case PipelineStage::RenderTarget:
        {
            source_access_mask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            source_depth_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

            break;
        }

        case PipelineStage::DrawIndirect:
        {
            source_buffer_access_mask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            break;
        }
    }

    bool has_depth = false;

    for ( u32 i = 0; i < barrier.num_image_barriers; ++i ) {

        Texture* texture_vulkan = device->access_texture( barrier.image_barriers[ i ].texture );

        VkImageMemoryBarrier& vk_barrier = image_barriers[ i ];
        vk_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

        vk_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        const bool is_color = !TextureFormat::has_depth_or_stencil( texture_vulkan->vk_format );
        has_depth = has_depth || !is_color;

        vk_barrier.image = texture_vulkan->vk_image;
        vk_barrier.subresourceRange.aspectMask = is_color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        vk_barrier.subresourceRange.baseMipLevel = 0;
        vk_barrier.subresourceRange.levelCount = 1;
        vk_barrier.subresourceRange.baseArrayLayer = 0;
        vk_barrier.subresourceRange.layerCount = 1;

        vk_barrier.oldLayout = texture_vulkan->vk_image_layout;

        // Transition to...
        vk_barrier.newLayout = is_color ? new_layout : new_depth_layout;

        vk_barrier.srcAccessMask = is_color ? source_access_mask : source_depth_access_mask;
        vk_barrier.dstAccessMask = is_color ? destination_access_mask : destination_depth_access_mask;

        texture_vulkan->vk_image_layout = vk_barrier.newLayout;
    }

    VkPipelineStageFlags source_stage_mask = to_vk_pipeline_stage( ( PipelineStage::Enum )barrier.source_pipeline_stage );
    VkPipelineStageFlags destination_stage_mask = to_vk_pipeline_stage( ( PipelineStage::Enum )barrier.destination_pipeline_stage );

    if ( has_depth ) {

        source_stage_mask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        destination_stage_mask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    }

    static VkBufferMemoryBarrier buffer_memory_barriers[ 8 ];
    for ( u32 i = 0; i < barrier.num_memory_barriers; ++i ) {
        VkBufferMemoryBarrier& vk_barrier = buffer_memory_barriers[ i ];
        vk_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;

        Buffer* buffer = device->access_buffer( barrier.memory_barriers[ i ].buffer );

        vk_barrier.buffer = buffer->vk_buffer;
        vk_barrier.offset = 0;
        vk_barrier.size = buffer->size;
        vk_barrier.srcAccessMask = source_buffer_access_mask;
        vk_barrier.dstAccessMask = destination_buffer_access_mask;

        vk_barrier.srcQueueFamilyIndex = 0;
        vk_barrier.dstQueueFamilyIndex = 0;
    }

    vkCmdPipelineBarrier( vk_command_buffer, source_stage_mask, destination_stage_mask, 0, 0, nullptr, barrier.num_memory_barriers, buffer_memory_barriers, barrier.num_image_barriers, image_barriers );
}

void CommandBuffer::fill_buffer( BufferHandle buffer, u32 offset, u32 size, u32 data ) {
    Buffer* vk_buffer = device->access_buffer( buffer );

    vkCmdFillBuffer( vk_command_buffer, vk_buffer->vk_buffer, VkDeviceSize( offset ), size ? VkDeviceSize( size ) : VkDeviceSize( vk_buffer->size ), data);
}

void CommandBuffer::push_marker( const char* name ) {

    device->push_gpu_timestamp( this, name );

    if ( !device->debug_utils_extension_present )
        return;

    device->push_marker( vk_command_buffer, name );
}

void CommandBuffer::pop_marker() {

    device->pop_gpu_timestamp( this );

    if ( !device->debug_utils_extension_present )
        return;

    device->pop_marker( vk_command_buffer );
}


} // namespace raptor
