#pragma once

#include "app_window.hpp"
#include "app_ui.hpp"

#include <daxa/utils/pipeline_manager.hpp>
#include <daxa/utils/imgui.hpp>
#include <daxa/utils/task_graph.hpp>
#include <daxa/utils/math_operators.hpp>
using namespace daxa::math_operators;

#include <shared/shared.inl>

#include <chrono>
#include <future>

using BDA = daxa::BufferDeviceAddress;

struct RenderImages {
    u32vec2 size{};
    u32vec2 rounded_size{};
    daxa::ImageId depth_prepass_image;

    daxa::ImageId raster_color_image;
    daxa::ImageId raster_depth_image;

    daxa::ImageId g_buffer_image;

    daxa::ImageId depth32_image;
    daxa::ImageId scaled_depth32_image;
    daxa::ImageId ssao_image;

    daxa::ImageId indirect_diffuse_image;
    std::array<daxa::ImageId, 2> reconstructed_shading_images;

    void create(daxa::Device &device);
    void destroy(daxa::Device &device) const;
};

struct VoxelChunks {
    daxa::BufferId buffer;

    daxa::BufferId leaves_buffer;
    daxa::BufferId available_leaves_stack_buffer;
    daxa::BufferId released_leaves_stack_buffer;
    u32 current_leaf_count = 0;

    daxa::BufferId parents_buffer;
    daxa::BufferId available_parents_stack_buffer;
    daxa::BufferId released_parents_stack_buffer;
    u32 current_parent_count = 0;

    void create(daxa::Device &device, u32 log2_chunks_per_axis);
    void destroy(daxa::Device &device) const;
};

struct GpuResources {
    RenderImages render_images;
    daxa::ImageId value_noise_image;
    daxa::ImageId blue_noise_vec1_image;
    daxa::ImageId blue_noise_vec2_image;
    daxa::ImageId blue_noise_unit_vec3_image;
    daxa::ImageId blue_noise_cosine_vec3_image;
    daxa::BufferId settings_buffer;
    daxa::BufferId input_buffer;
    daxa::BufferId output_buffer;
    daxa::BufferId staging_output_buffer;
    daxa::BufferId globals_buffer;
    daxa::BufferId temp_voxel_chunks_buffer;
    VoxelChunks voxel_chunks;
    AllocatorBufferState<VoxelMallocPageAllocator> voxel_malloc;
    daxa::BufferId gvox_model_buffer;
    AllocatorBufferState<VoxelLeafChunkAllocator> voxel_leaf_chunk_malloc;
    AllocatorBufferState<VoxelParentChunkAllocator> voxel_parent_chunk_malloc;

    daxa::BufferId simulated_voxel_particles_buffer;
    daxa::BufferId rendered_voxel_particles_buffer;
    daxa::BufferId placed_voxel_particles_buffer;
    daxa::SamplerId final_image_sampler;
    daxa::SamplerId value_noise_sampler;

    void create(daxa::Device &device);
    void destroy(daxa::Device &device) const;
};

struct GvoxModelData {
    size_t size = 0;
    uint8_t *ptr = nullptr;
};

struct VoxelApp : AppWindow<VoxelApp> {
    using Clock = std::chrono::high_resolution_clock;

    daxa::Instance daxa_instance;
    daxa::Device device;

    daxa::Swapchain swapchain;
    daxa::ImageId swapchain_image{};
    daxa::TaskImage task_swapchain_image{daxa::TaskImageInfo{.swapchain_image = true}};

    daxa::PipelineManager main_pipeline_manager;

    AppUi ui;
    daxa::ImGuiRenderer imgui_renderer;

    GpuResources gpu_resources;
    daxa::BufferId prev_gvox_model_buffer{};

    daxa::TaskImage task_render_depth_prepass_image{{.name = "task_render_depth_prepass_image"}};
    daxa::TaskImage task_render_raster_color_image{{.name = "task_render_raster_color_image"}};
    daxa::TaskImage task_render_raster_depth_image{{.name = "task_render_raster_depth_image"}};

    daxa::TaskImage task_render_g_buffer_image{{.name = "task_render_g_buffer_image"}};

    daxa::TaskImage task_render_depth32_image{{.name = "task_render_depth32_image"}};
    daxa::TaskImage task_render_scaled_depth32_image{{.name = "task_render_scaled_depth32_image"}};
    daxa::TaskImage task_render_ssao_image{{.name = "task_render_ssao_image"}};

    daxa::TaskImage task_render_indirect_diffuse_image{{.name = "task_render_indirect_diffuse_image"}};
    daxa::TaskImage task_render_prev_reconstructed_shading_image{{.name = "task_render_prev_reconstructed_shading_image"}};
    daxa::TaskImage task_render_reconstructed_shading_image{{.name = "task_render_reconstructed_shading_image"}};

    daxa::TaskImage task_value_noise_image{{.name = "task_value_noise_image"}};
    daxa::TaskImage task_blue_noise_vec1_image{{.name = "task_blue_noise_vec1_image"}};
    daxa::TaskImage task_blue_noise_vec2_image{{.name = "task_blue_noise_vec2_image"}};
    daxa::TaskImage task_blue_noise_unit_vec3_image{{.name = "task_blue_noise_unit_vec3_image"}};
    daxa::TaskImage task_blue_noise_cosine_vec3_image{{.name = "task_blue_noise_cosine_vec3_image"}};

    daxa::TaskBuffer task_settings_buffer{{.name = "task_settings_buffer"}};
    daxa::TaskBuffer task_input_buffer{{.name = "task_input_buffer"}};
    daxa::TaskBuffer task_output_buffer{{.name = "task_output_buffer"}};
    daxa::TaskBuffer task_staging_output_buffer{{.name = "task_staging_output_buffer"}};
    daxa::TaskBuffer task_globals_buffer{{.name = "task_globals_buffer"}};
    daxa::TaskBuffer task_temp_voxel_chunks_buffer{{.name = "task_temp_voxel_chunks_buffer"}};
    daxa::TaskBuffer task_voxel_chunks_buffer{{.name = "task_voxel_chunks_buffer"}};
    daxa::TaskBuffer task_gvox_model_buffer{{.name = "task_gvox_model_buffer"}};
    daxa::TaskBuffer task_simulated_voxel_particles_buffer{{.name = "task_simulated_voxel_particles_buffer"}};
    daxa::TaskBuffer task_rendered_voxel_particles_buffer{{.name = "task_rendered_voxel_particles_buffer"}};
    daxa::TaskBuffer task_placed_voxel_particles_buffer{{.name = "task_placed_voxel_particles_buffer"}};

    StartupComputeTaskState startup_task_state;
    PerframeComputeTaskState perframe_task_state;
    ChunkEditComputeTaskState chunk_edit_task_state;
    ChunkOpt_x2x4_ComputeTaskState chunk_opt_x2x4_task_state;
    ChunkOpt_x8up_ComputeTaskState chunk_opt_x8up_task_state;
    ChunkAllocComputeTaskState chunk_alloc_task_state;
    TraceDepthPrepassComputeTaskState trace_depth_prepass_task_state;
    TracePrimaryComputeTaskState trace_primary_task_state;
    DownscaleComputeTaskState downscale_task_state;
    SsaoComputeTaskState ssao_task_state;
    TraceSecondaryComputeTaskState trace_secondary_task_state;
    UpscaleReconstructComputeTaskState upscale_reconstruct_task_state;
    PostprocessingRasterTaskState postprocessing_task_state;

#if ENABLE_HIERARCHICAL_QUEUE
    ChunkHierarchyComputeTaskState chunk_hierarchy_task_state;
#else
    PerChunkComputeTaskState per_chunk_task_state;
#endif

    VoxelParticleSimComputeTaskState voxel_particle_sim_task_state;
    VoxelParticleRasterTaskState voxel_particle_raster_task_state;

    enum class Conditions {
        STARTUP,
        UPLOAD_SETTINGS,
        UPLOAD_GVOX_MODEL,
        DYNAMIC_BUFFERS_REALLOC,
        LAST,
    };
    daxa::TaskGraph main_task_graph;
    std::array<bool, static_cast<usize>(Conditions::LAST)> condition_values{};

    GpuInput gpu_input{};
    GpuOutput gpu_output{};
    Clock::time_point start;
    Clock::time_point prev_time;
    f32 render_res_scl{1.0f};
    GvoxContext *gvox_ctx;
    bool needs_vram_calc = true;

    bool has_model = false;
    std::future<GvoxModelData> gvox_model_data_future;
    GvoxModelData gvox_model_data;
    bool model_is_loading = false;
    bool model_is_ready = false;

    VoxelApp();
    VoxelApp(VoxelApp const &) = delete;
    VoxelApp(VoxelApp &&) = delete;
    auto operator=(VoxelApp const &) -> VoxelApp & = delete;
    auto operator=(VoxelApp &&) -> VoxelApp & = delete;
    ~VoxelApp();

    void run();

    void calc_vram_usage();
    auto load_gvox_data() -> GvoxModelData;

    void on_update();
    void on_mouse_move(f32 x, f32 y);
    void on_mouse_scroll(f32 dx, f32 dy);
    void on_mouse_button(i32 button_id, i32 action);
    void on_key(i32 key_id, i32 action);
    void on_resize(u32 sx, u32 sy);
    void on_drop(std::span<char const *> filepaths);

    void set_task_render_images();
    void recreate_render_images();
    void recreate_voxel_chunks();

    void run_startup(daxa::TaskGraph &temp_task_graph);
    void upload_settings(daxa::TaskGraph &temp_task_graph);
    void upload_model(daxa::TaskGraph &temp_task_graph);
    void dynamic_buffers_realloc(daxa::TaskGraph &temp_task_graph);

    auto record_main_task_graph() -> daxa::TaskGraph;
};
