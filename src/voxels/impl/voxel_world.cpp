#include "voxel_world.inl"
#include <fmt/format.h>

#ifndef defer
struct defer_dummy {};
template <class F>
struct deferrer {
    F f;
    ~deferrer() { f(); }
};
template <class F>
deferrer<F> operator*(defer_dummy, F f) { return {f}; }
#define DEFER_(LINE) zz_defer##LINE
#define DEFER(LINE) DEFER_(LINE)
#define defer auto DEFER(__LINE__) = defer_dummy{} *[&]()
#endif // defer

static uint32_t calc_chunk_index(glm::uvec3 chunk_i, glm::ivec3 offset) {
    // Modulate the chunk index to be wrapped around relative to the chunk offset provided.
    auto temp_chunk_i = (glm::ivec3(chunk_i) + (offset >> glm::ivec3(6 + LOG2_VOXEL_SIZE))) % glm::ivec3(CHUNKS_PER_AXIS);
    if (temp_chunk_i.x < 0) {
        temp_chunk_i.x += CHUNKS_PER_AXIS;
    }
    if (temp_chunk_i.y < 0) {
        temp_chunk_i.y += CHUNKS_PER_AXIS;
    }
    if (temp_chunk_i.z < 0) {
        temp_chunk_i.z += CHUNKS_PER_AXIS;
    }
    chunk_i = glm::uvec3(temp_chunk_i);
    uint32_t chunk_index = chunk_i.x + chunk_i.y * CHUNKS_PER_AXIS + chunk_i.z * CHUNKS_PER_AXIS * CHUNKS_PER_AXIS;
    assert(chunk_index < 50000);
    return chunk_index;
}

static uint32_t calc_palette_region_index(glm::uvec3 inchunk_voxel_i) {
    glm::uvec3 palette_region_i = inchunk_voxel_i / uint32_t(PALETTE_REGION_SIZE);
    return palette_region_i.x + palette_region_i.y * PALETTES_PER_CHUNK_AXIS + palette_region_i.z * PALETTES_PER_CHUNK_AXIS * PALETTES_PER_CHUNK_AXIS;
}

static uint32_t calc_palette_voxel_index(glm::uvec3 inchunk_voxel_i) {
    glm::uvec3 palette_voxel_i = inchunk_voxel_i & uint32_t(PALETTE_REGION_SIZE - 1);
    return palette_voxel_i.x + palette_voxel_i.y * PALETTE_REGION_SIZE + palette_voxel_i.z * PALETTE_REGION_SIZE * PALETTE_REGION_SIZE;
}

PackedVoxel sample_palette(CpuPaletteChunk palette_header, uint32_t palette_voxel_index) {
    // daxa_RWBufferPtr(uint) blob_u32s;
    // voxel_malloc_address_to_u32_ptr(allocator, palette_header.blob_ptr, blob_u32s);
    // blob_u32s = advance(blob_u32s, PALETTE_ACCELERATION_STRUCTURE_SIZE_U32S);
    auto const *blob_u32s = palette_header.blob_ptr;
    if (palette_header.variant_n > PALETTE_MAX_COMPRESSED_VARIANT_N) {
        return PackedVoxel(blob_u32s[palette_voxel_index]);
    }
    auto bits_per_variant = ceil_log2(palette_header.variant_n);
    auto mask = (~0u) >> (32 - bits_per_variant);
    auto bit_index = palette_voxel_index * bits_per_variant;
    auto data_index = bit_index / 32;
    auto data_offset = bit_index - data_index * 32;
    auto my_palette_index = (blob_u32s[palette_header.variant_n + data_index + 0] >> data_offset) & mask;
    if (data_offset + bits_per_variant > 32) {
        auto shift = bits_per_variant - ((data_offset + bits_per_variant) & 0x1f);
        my_palette_index |= (blob_u32s[palette_header.variant_n + data_index + 1] << shift) & mask;
    }
    auto voxel_data = blob_u32s[my_palette_index];

    // debug_utils::Console::add_log(fmt::format("\nCPU {} ", bit_index));

    // debug_utils::Console::add_log("\nCPU ");
    // for (uint32_t i = 0; i < palette_header.variant_n; ++i) {
    //     debug_utils::Console::add_log(fmt::format("{} ", blob_u32s[i]));
    // }
    // debug_utils::Console::add_log("\n");

    return PackedVoxel(voxel_data);
}

PackedVoxel sample_voxel_chunk(CpuVoxelChunk const &voxel_chunk, glm::uvec3 inchunk_voxel_i) {
    auto palette_region_index = calc_palette_region_index(inchunk_voxel_i);
    auto palette_voxel_index = calc_palette_voxel_index(inchunk_voxel_i);
    CpuPaletteChunk palette_header = voxel_chunk.palette_chunks[palette_region_index];
    if (palette_header.variant_n < 2) {
        return PackedVoxel(static_cast<uint32_t>(std::bit_cast<uint64_t>(palette_header.blob_ptr)));
    }
    return sample_palette(palette_header, palette_voxel_index);
}

// PackedVoxel sample_voxel_chunk(VoxelBufferPtrs ptrs, glm::uvec3 chunk_n, glm::vec3 voxel_p, glm::vec3 bias) {
//     vec3 offset = glm::vec3((deref(ptrs.globals).offset) & ((1 << (6 + LOG2_VOXEL_SIZE)) - 1)) + glm::vec3(chunk_n) * CHUNK_WORLDSPACE_SIZE * 0.5;
//     uvec3 voxel_i = glm::uvec3(floor((voxel_p + offset) * VOXEL_SCL + bias));
//     uvec3 chunk_i = voxel_i / CHUNK_SIZE;
//     uint chunk_index = calc_chunk_index(ptrs.globals, chunk_i, chunk_n);
//     return sample_voxel_chunk(ptrs.allocator, advance(ptrs.voxel_chunks_ptr, chunk_index), voxel_i - chunk_i * CHUNK_SIZE);
// }

bool VoxelWorld::sample(daxa_f32vec3 pos, daxa_i32vec3 player_unit_offset) {
    glm::vec3 offset = glm::vec3(std::bit_cast<glm::ivec3>(player_unit_offset) & ((1 << (6 + LOG2_VOXEL_SIZE)) - 1)) + glm::vec3(CHUNKS_PER_AXIS) * CHUNK_WORLDSPACE_SIZE * 0.5f;
    glm::uvec3 voxel_i = glm::uvec3(floor((std::bit_cast<glm::vec3>(pos) + glm::vec3(offset)) * float(VOXEL_SCL)));
    glm::uvec3 chunk_i = voxel_i / uint32_t(CHUNK_SIZE);
    uint32_t chunk_index = calc_chunk_index(chunk_i, std::bit_cast<glm::ivec3>(player_unit_offset));

    auto packed_voxel = sample_voxel_chunk(voxel_chunks[chunk_index], voxel_i - chunk_i * uint32_t(CHUNK_SIZE));
    auto material_type = (packed_voxel.data >> 0) & 3;

    return material_type != 0;
}

void VoxelWorld::init_gpu_malloc(GpuContext &gpu_context) {
    if (!gpu_malloc_initialized) {
        gpu_malloc_initialized = true;
        buffers.voxel_malloc.create(gpu_context);
        // buffers.voxel_leaf_chunk_malloc.create(device);
        // buffers.voxel_parent_chunk_malloc.create(device);
    }
}

void VoxelWorld::record_startup(GpuContext &gpu_context) {
    buffers.chunk_updates = gpu_context.find_or_add_temporal_buffer({
        .size = sizeof(ChunkUpdate) * MAX_CHUNK_UPDATES_PER_FRAME * (FRAMES_IN_FLIGHT + 1),
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
        .name = "chunk_updates",
    });
    buffers.chunk_update_heap = gpu_context.find_or_add_temporal_buffer({
        .size = sizeof(uint32_t) * MAX_CHUNK_UPDATES_PER_FRAME_VOXEL_COUNT * (FRAMES_IN_FLIGHT + 1),
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
        .name = "chunk_update_heap",
    });

    buffers.voxel_globals = gpu_context.find_or_add_temporal_buffer({
        .size = sizeof(VoxelWorldGlobals),
        .name = "voxel_globals",
    });

    auto chunk_n = (CHUNKS_PER_AXIS);
    chunk_n = chunk_n * chunk_n * chunk_n;
    buffers.voxel_chunks = gpu_context.find_or_add_temporal_buffer({
        .size = sizeof(VoxelLeafChunk) * chunk_n,
        .name = "voxel_chunks",
    });
    voxel_chunks.resize(chunk_n);

    init_gpu_malloc(gpu_context);

    gpu_context.frame_task_graph.use_persistent_buffer(buffers.chunk_updates.task_resource);
    gpu_context.frame_task_graph.use_persistent_buffer(buffers.chunk_update_heap.task_resource);
    gpu_context.frame_task_graph.use_persistent_buffer(buffers.voxel_globals.task_resource);
    gpu_context.frame_task_graph.use_persistent_buffer(buffers.voxel_chunks.task_resource);
    buffers.voxel_malloc.for_each_task_buffer([&gpu_context](auto &task_buffer) { gpu_context.frame_task_graph.use_persistent_buffer(task_buffer); });

    gpu_context.startup_task_graph.use_persistent_buffer(buffers.voxel_globals.task_resource);
    gpu_context.startup_task_graph.use_persistent_buffer(buffers.voxel_chunks.task_resource);
    buffers.voxel_malloc.for_each_task_buffer([&gpu_context](auto &task_buffer) { gpu_context.startup_task_graph.use_persistent_buffer(task_buffer); });

    // buffers.voxel_leaf_chunk_malloc.for_each_task_buffer([&gpu_context](auto &task_buffer) { gpu_context.frame_task_graph.use_persistent_buffer(task_buffer); });
    // buffers.voxel_parent_chunk_malloc.for_each_task_buffer([&gpu_context](auto &task_buffer) { gpu_context.frame_task_graph.use_persistent_buffer(task_buffer); });

    gpu_context.startup_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_globals.task_resource),
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_chunks.task_resource),
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_malloc.task_element_buffer),
            // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_leaf_chunk_malloc.task_element_buffer),
            // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_parent_chunk_malloc.task_element_buffer),
        },
        .task = [this](daxa::TaskInterface const &ti) {
            ti.recorder.clear_buffer({
                .buffer = buffers.voxel_globals.task_resource.get_state().buffers[0],
                .offset = 0,
                .size = sizeof(VoxelWorldGlobals),
                .clear_value = 0,
            });

            auto chunk_n = (CHUNKS_PER_AXIS);
            chunk_n = chunk_n * chunk_n * chunk_n;
            ti.recorder.clear_buffer({
                .buffer = buffers.voxel_chunks.task_resource.get_state().buffers[0],
                .offset = 0,
                .size = sizeof(VoxelLeafChunk) * chunk_n,
                .clear_value = 0,
            });

            buffers.voxel_malloc.clear_buffers(ti.recorder);
            // buffers.voxel_leaf_chunk_malloc.clear_buffers(ti.recorder);
            // buffers.voxel_parent_chunk_malloc.clear_buffers(ti.recorder);
        },
        .name = "clear chunk editor",
    });

    gpu_context.startup_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_malloc.task_allocator_buffer),
            // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_leaf_chunk_malloc.task_allocator_buffer),
            // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_parent_chunk_malloc.task_allocator_buffer),
        },
        .task = [this](daxa::TaskInterface const &ti) {
            buffers.voxel_malloc.init(ti.device, ti.recorder);
            // buffers.voxel_leaf_chunk_malloc.init(ti.device, ti.recorder);
            // buffers.voxel_parent_chunk_malloc.init(ti.device, ti.recorder);
        },
        .name = "Initialize",
    });

    gpu_context.add(ComputeTask<VoxelWorldStartupCompute::Task, VoxelWorldStartupComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/startup.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{VoxelWorldStartupCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            VOXELS_BUFFER_USES_ASSIGN(VoxelWorldStartupCompute, buffers),
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, VoxelWorldStartupComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch({1, 1, 1});
        },
        .task_graph_ptr = &gpu_context.startup_task_graph,
    });

    // test

    daxa_u64 blas_scratch_buffer_size = 1024 * 1024 * 10;
    daxa_u64 blas_scratch_buffer_offset = 0;
    daxa_u32 acceleration_structure_scratch_offset_alignment = 0;
    daxa_u64 blas_buffer_size = 1024 * 1024 * 10;
    daxa_u64 blas_buffer_offset = 0;
    const daxa_u32 ACCELERATION_STRUCTURE_BUILD_OFFSET_ALIGMENT = 256; // NOTE: Requested by the spec

    acceleration_structure_scratch_offset_alignment = gpu_context.device.properties().acceleration_structure_properties.value().min_acceleration_structure_scratch_offset_alignment;

    blas_scratch_buffer = gpu_context.find_or_add_temporal_buffer({
        .size = blas_scratch_buffer_size,
        .name = "blas_scratch_buffer",
    });
    blas_buffer = gpu_context.find_or_add_temporal_buffer({
        .size = blas_buffer_size,
        .name = "blas_buffer",
    });

    /// Alignments:
    auto get_aligned = [&](daxa_u64 operand, daxa_u64 granularity) -> daxa_u64 {
        return ((operand + (granularity - 1)) & ~(granularity - 1));
    };

    /// aabb data:
    auto min_max = std::array{
        std::array{0.0f, 0.0f, 0.0f},
        std::array{1.0f, 1.0f, 1.0f},
        std::array{0.5f, 0.5f, 0.5f},
        std::array{1.5f, 1.5f, 1.5f},
    };
    buffers.aabb_buffer = gpu_context.find_or_add_temporal_buffer({
        .size = sizeof(decltype(min_max)),
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM, // TODO
        .name = "aabb_buffer",
    });
    *gpu_context.device.get_host_address_as<decltype(min_max)>(buffers.aabb_buffer.resource_id).value() = min_max;
    /// Procedural Geometry Info:
    auto proc_geometries = std::array{
        daxa::BlasAabbGeometryInfo{
            .data = gpu_context.device.get_device_address(buffers.aabb_buffer.resource_id).value(),
            .stride = sizeof(daxa_f32mat3x2),
            .count = 1,
            .flags = daxa::GeometryFlagBits::OPAQUE, // Is also default
        },
        daxa::BlasAabbGeometryInfo{
            .data = gpu_context.device.get_device_address(buffers.aabb_buffer.resource_id).value() + sizeof(daxa_f32mat3x2),
            .stride = sizeof(daxa_f32mat3x2),
            .count = 1,
            .flags = daxa::GeometryFlagBits::OPAQUE, // Is also default
        }};
    /// Create Procedural Blas:
    auto proc_blas_build_info = daxa::BlasBuildInfo{
        .flags = daxa::AccelerationStructureBuildFlagBits::PREFER_FAST_TRACE, // Is also default
        .dst_blas = {},                                                       // Ignored in get_acceleration_structure_build_sizes.       // Is also default
        .geometries = proc_geometries,
        .scratch_data = {}, // Ignored in get_acceleration_structure_build_sizes.   // Is also default
    };
    daxa::AccelerationStructureBuildSizesInfo proc_build_size_info = gpu_context.device.get_blas_build_sizes(proc_blas_build_info);

    auto scratch_alignment_size = get_aligned(proc_build_size_info.build_scratch_size, acceleration_structure_scratch_offset_alignment);
    if ((blas_scratch_buffer_offset + scratch_alignment_size) > blas_scratch_buffer_size) {
        std::cout << "blas_scratch_buffer_offset > blas_scratch_buffer_size" << std::endl;
        abort();
    }
    proc_blas_build_info.scratch_data = gpu_context.device.get_device_address(blas_scratch_buffer.resource_id).value() + blas_scratch_buffer_offset;
    blas_scratch_buffer_offset += scratch_alignment_size;

    auto build_aligment_size = get_aligned(proc_build_size_info.acceleration_structure_size, ACCELERATION_STRUCTURE_BUILD_OFFSET_ALIGMENT);

    if ((blas_buffer_offset + build_aligment_size) > blas_buffer_size) {
        // TODO: Try to resize buffer
        std::cout << "blas_buffer_offset > blas_buffer_size" << std::endl;
        abort();
    }

    this->proc_blas = gpu_context.device.create_blas_from_buffer({
        .blas_info = {
            .size = proc_build_size_info.acceleration_structure_size,
            .name = "test procedural blas",
        },
        .buffer_id = blas_buffer.resource_id,
        .offset = blas_buffer_offset,
    });
    proc_blas_build_info.dst_blas = proc_blas;
    blas_buffer_offset += build_aligment_size;

    /// create blas instances for tlas:
    auto blas_instances_buffer = gpu_context.device.create_buffer({
        .size = sizeof(daxa_BlasInstanceData) * 2,
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
        .name = "blas instances array buffer",
    });
    defer { gpu_context.device.destroy_buffer(blas_instances_buffer); };

    auto blas_instance_array = std::array{daxa_BlasInstanceData{
        .transform = {
            {1, 0, 0, +0.0f},
            {0, 1, 0, +0.0f},
            {0, 0, 1, -0.5f},
        },
        .instance_custom_index = 0, // Is also default
        .mask = 0xFF,
        .instance_shader_binding_table_record_offset = 0,
        .flags = {}, // Is also default
        .blas_device_address = gpu_context.device.get_device_address(proc_blas).value(),
    }};

    std::memcpy(gpu_context.device.get_host_address_as<daxa_BlasInstanceData>(blas_instances_buffer).value(),
                blas_instance_array.data(),
                blas_instance_array.size() * sizeof(daxa_BlasInstanceData));

    auto blas_instances = std::array{
        daxa::TlasInstanceInfo{
            .data = {}, // Ignored in get_acceleration_structure_build_sizes.   // Is also default
            .count = 2,
            .is_data_array_of_pointers = false,      // Buffer contains flat array of instances, not an array of pointers to instances.
            .flags = daxa::GeometryFlagBits::OPAQUE, // Is also default
        },
    };
    auto tlas_build_info = daxa::TlasBuildInfo{
        .flags = daxa::AccelerationStructureBuildFlagBits::PREFER_FAST_TRACE,
        .dst_tlas = {}, // Ignored in get_acceleration_structure_build_sizes.
        .instances = blas_instances,
        .scratch_data = {}, // Ignored in get_acceleration_structure_build_sizes.
    };
    daxa::AccelerationStructureBuildSizesInfo tlas_build_sizes = gpu_context.device.get_tlas_build_sizes(tlas_build_info);
    /// Create Tlas:
    buffers.tlas = gpu_context.device.create_tlas({
        .size = tlas_build_sizes.acceleration_structure_size,
        .name = "tlas",
    });
    /// Create Build Scratch buffer
    auto tlas_scratch_buffer = gpu_context.device.create_buffer({
        .size = tlas_build_sizes.build_scratch_size,
        .name = "tlas build scratch buffer",
    });
    defer { gpu_context.device.destroy_buffer(tlas_scratch_buffer); };
    /// Update build info:
    tlas_build_info.dst_tlas = buffers.tlas;
    tlas_build_info.scratch_data = gpu_context.device.get_device_address(tlas_scratch_buffer).value();
    blas_instances[0].data = gpu_context.device.get_device_address(blas_instances_buffer).value();

    task_blas = daxa::TaskBlas({.initial_blas = {.blas = std::array{proc_blas}}});
    buffers.task_tlas = daxa::TaskTlas({.initial_tlas = {.tlas = std::array{buffers.tlas}}});

    /// Record build commands:
    daxa::TaskGraph temp_task_graph = daxa::TaskGraph({
        .device = gpu_context.device,
        .name = "temp_task_graph",
    });

    temp_task_graph.use_persistent_blas(task_blas);
    temp_task_graph.use_persistent_tlas(buffers.task_tlas);

    temp_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskBlasAccess::BUILD_WRITE, task_blas),
        },
        .task = [&](daxa::TaskInterface const &ti) {
            ti.recorder.build_acceleration_structures({
                .blas_build_infos = std::array{proc_blas_build_info},
            });
        },
        .name = "blas build",
    });
    temp_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskBlasAccess::BUILD_READ, task_blas),
            daxa::inl_attachment(daxa::TaskTlasAccess::BUILD_WRITE, buffers.task_tlas),
        },
        .task = [&](daxa::TaskInterface const &ti) {
            ti.recorder.build_acceleration_structures({
                .tlas_build_infos = std::array{tlas_build_info},
            });
        },
        .name = "tlas build",
    });

    temp_task_graph.submit({});
    temp_task_graph.complete({});
    temp_task_graph.execute({});

    gpu_context.frame_task_graph.use_persistent_tlas(buffers.task_tlas);
    gpu_context.frame_task_graph.use_persistent_buffer(buffers.aabb_buffer.task_resource);
}

void VoxelWorld::begin_frame(daxa::Device &device, GpuInput const &gpu_input, VoxelWorldOutput const &gpu_output) {
    buffers.voxel_malloc.check_for_realloc(device, gpu_output.voxel_malloc_output.current_element_count);
    // buffers.voxel_leaf_chunk_malloc.check_for_realloc(device, gpu_output.voxel_leaf_chunk_output.current_element_count);
    // buffers.voxel_parent_chunk_malloc.check_for_realloc(device, gpu_output.voxel_parent_chunk_output.current_element_count);

    bool needs_realloc = false;
    needs_realloc = needs_realloc || buffers.voxel_malloc.needs_realloc();
    // needs_realloc = needs_realloc || buffers.voxel_leaf_chunk_malloc.needs_realloc();
    // needs_realloc = needs_realloc || buffers.voxel_parent_chunk_malloc.needs_realloc();

    debug_utils::DebugDisplay::set_debug_string("GPU Heap", fmt::format("{} pages ({:.2f} MB)", buffers.voxel_malloc.current_element_count, static_cast<double>(buffers.voxel_malloc.current_element_count * VOXEL_MALLOC_PAGE_SIZE_BYTES) / 1'000'000.0));
    debug_utils::DebugDisplay::set_debug_string("GPU Heap Usage", fmt::format("{:.2f} MB", static_cast<double>(gpu_output.voxel_malloc_output.current_element_count) * VOXEL_MALLOC_PAGE_SIZE_BYTES / 1'000'000));

    if (needs_realloc) {
        auto temp_task_graph = daxa::TaskGraph({
            .device = device,
            .name = "temp_task_graph",
        });

        buffers.voxel_malloc.for_each_task_buffer([&temp_task_graph](auto &task_buffer) { temp_task_graph.use_persistent_buffer(task_buffer); });
        // buffers.voxel_leaf_chunk_malloc.for_each_task_buffer([&temp_task_graph](auto &task_buffer) { temp_task_graph.use_persistent_buffer(task_buffer); });
        // buffers.voxel_parent_chunk_malloc.for_each_task_buffer([&temp_task_graph](auto &task_buffer) { temp_task_graph.use_persistent_buffer(task_buffer); });
        temp_task_graph.add_task({
            .attachments = {
                daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_READ, buffers.voxel_malloc.task_old_element_buffer),
                daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_malloc.task_element_buffer),
                // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_READ, buffers.voxel_leaf_chunk_malloc.task_old_element_buffer),
                // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_leaf_chunk_malloc.task_element_buffer),
                // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_READ, buffers.voxel_parent_chunk_malloc.task_old_element_buffer),
                // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_parent_chunk_malloc.task_element_buffer),
            },
            .task = [this](daxa::TaskInterface const &ti) {
                if (buffers.voxel_malloc.needs_realloc()) {
                    buffers.voxel_malloc.realloc(ti.device, ti.recorder);
                }
                // if (buffers.voxel_leaf_chunk_malloc.needs_realloc()) {
                //     buffers.voxel_leaf_chunk_malloc.realloc(ti.device, ti.recorder);
                // }
                // if (buffers.voxel_parent_chunk_malloc.needs_realloc()) {
                //     buffers.voxel_parent_chunk_malloc.realloc(ti.device, ti.recorder);
                // }
            },
            .name = "Transfer Task",
        });

        temp_task_graph.submit({});
        temp_task_graph.complete({});
        temp_task_graph.execute({});
    }

    {
        auto const offset = (gpu_input.frame_index + 0) % (FRAMES_IN_FLIGHT + 1);
        auto const *output_heap = device.get_host_address_as<uint32_t>(buffers.chunk_update_heap.resource_id).value() + offset * MAX_CHUNK_UPDATES_PER_FRAME_VOXEL_COUNT;
        auto const *chunk_updates = device.get_host_address_as<ChunkUpdate>(buffers.chunk_updates.resource_id).value() + offset * MAX_CHUNK_UPDATES_PER_FRAME;
        auto copied_bytes = 0u;

        for (uint32_t chunk_update_i = 0; chunk_update_i < MAX_CHUNK_UPDATES_PER_FRAME; ++chunk_update_i) {
            if (chunk_updates[chunk_update_i].info.flags != 1) {
                // copied_bytes += sizeof(uint32_t);
                continue;
            }
            auto chunk_update = chunk_updates[chunk_update_i];
            copied_bytes += sizeof(chunk_update);
            auto &chunk = voxel_chunks[chunk_update.info.chunk_index];
            for (uint32_t palette_region_i = 0; palette_region_i < PALETTES_PER_CHUNK; ++palette_region_i) {
                auto const &palette_header = chunk_update.palette_headers[palette_region_i];
                auto &palette_chunk = chunk.palette_chunks[palette_region_i];
                auto palette_size = palette_header.variant_n;
                auto compressed_size = 0u;
                if (palette_chunk.variant_n > 1) {
                    delete[] palette_chunk.blob_ptr;
                }
                palette_chunk.variant_n = palette_size;
                auto bits_per_variant = ceil_log2(palette_size);
                if (palette_size > PALETTE_MAX_COMPRESSED_VARIANT_N) {
                    compressed_size = PALETTE_REGION_TOTAL_SIZE;
                } else if (palette_size > 1) {
                    compressed_size = palette_size + (bits_per_variant * PALETTE_REGION_TOTAL_SIZE + 31) / 32;
                } else {
                    // no blob
                }
                if (compressed_size != 0) {
                    palette_chunk.blob_ptr = new uint32_t[compressed_size];
                    memcpy(palette_chunk.blob_ptr, output_heap + palette_header.blob_ptr, compressed_size * sizeof(uint32_t));
                    copied_bytes += compressed_size * sizeof(uint32_t);
                } else {
                    palette_chunk.blob_ptr = std::bit_cast<uint32_t *>(size_t(palette_header.blob_ptr));
                }
            }
        }

        // if (copied_bytes > 0) {
        //     debug_utils::Console::add_log(fmt::format("{} MB copied", double(copied_bytes) / 1'000'000.0));
        // }
    }
}

void VoxelWorld::record_frame(GpuContext &gpu_context, daxa::TaskBufferView task_gvox_model_buffer, VoxelParticles &particles) {
    gpu_context.add(ComputeTask<VoxelWorldPerframeCompute::Task, VoxelWorldPerframeComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/perframe.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{VoxelWorldPerframeCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{VoxelWorldPerframeCompute::AT.gpu_output, gpu_context.task_output_buffer}},
            daxa::TaskViewVariant{std::pair{VoxelWorldPerframeCompute::AT.chunk_updates, buffers.chunk_updates.task_resource}},
            VOXELS_BUFFER_USES_ASSIGN(VoxelWorldPerframeCompute, buffers),
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, VoxelWorldPerframeComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch({1, 1, 1});
        },
    });

    gpu_context.add(ComputeTask<PerChunkCompute::Task, PerChunkComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.gvox_model, task_gvox_model_buffer}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.value_noise_texture, gpu_context.task_value_noise_image_view}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, PerChunkComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            auto const dispatch_size = CHUNKS_DISPATCH_SIZE;
            ti.recorder.dispatch({dispatch_size, dispatch_size, dispatch_size});
        },
    });

    auto task_temp_voxel_chunks_buffer = gpu_context.frame_task_graph.create_transient_buffer({
        .size = sizeof(TempVoxelChunk) * MAX_CHUNK_UPDATES_PER_FRAME,
        .name = "temp_voxel_chunks_buffer",
    });

    gpu_context.add(ComputeTask<ChunkEditCompute::Task, ChunkEditComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.gvox_model, task_gvox_model_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.voxel_malloc_page_allocator, buffers.voxel_malloc.task_allocator_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            SIMPLE_STATIC_ALLOCATOR_BUFFER_USES_ASSIGN(ChunkEditCompute, GrassStrandAllocator, particles.grass.grass_allocator),
            SIMPLE_STATIC_ALLOCATOR_BUFFER_USES_ASSIGN(ChunkEditCompute, FlowerAllocator, particles.flowers.flower_allocator),
            SIMPLE_STATIC_ALLOCATOR_BUFFER_USES_ASSIGN(ChunkEditCompute, TreeParticleAllocator, particles.tree_particles.tree_particle_allocator),
            SIMPLE_STATIC_ALLOCATOR_BUFFER_USES_ASSIGN(ChunkEditCompute, FireParticleAllocator, particles.fire_particles.fire_particle_allocator),
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.value_noise_texture, gpu_context.task_value_noise_image_view}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.test_texture, gpu_context.task_test_texture}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.test_texture2, gpu_context.task_test_texture2}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkEditComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkEditCompute::AT.voxel_globals).ids[0],
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, chunk_edit_dispatch),
            });
        },
    });

    gpu_context.add(ComputeTask<ChunkEditPostProcessCompute::Task, ChunkEditPostProcessComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.gvox_model, task_gvox_model_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.voxel_malloc_page_allocator, buffers.voxel_malloc.task_allocator_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.value_noise_texture, gpu_context.task_value_noise_image_view}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkEditPostProcessComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkEditPostProcessCompute::AT.voxel_globals).ids[0],
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, chunk_edit_dispatch),
            });
        },
    });

    gpu_context.add(ComputeTask<ChunkOptCompute::Task, ChunkOptComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .extra_defines = {{"CHUNK_OPT_STAGE", "0"}},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkOptComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkOptCompute::AT.voxel_globals).ids[0],
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, subchunk_x2x4_dispatch),
            });
        },
    });

    gpu_context.add(ComputeTask<ChunkOptCompute::Task, ChunkOptComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .extra_defines = {{"CHUNK_OPT_STAGE", "1"}},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkOptComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkOptCompute::AT.voxel_globals).ids[0],
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, subchunk_x8up_dispatch),
            });
        },
    });

    gpu_context.add(ComputeTask<ChunkAllocCompute::Task, ChunkAllocComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.voxel_malloc_page_allocator, buffers.voxel_malloc.task_allocator_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.chunk_updates, buffers.chunk_updates.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.chunk_update_heap, buffers.chunk_update_heap.task_resource}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkAllocComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkAllocCompute::AT.voxel_globals).ids[0],
                // NOTE: This should always have the same value as the chunk edit dispatch, so we're re-using it here
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, chunk_edit_dispatch),
            });
        },
    });
}
