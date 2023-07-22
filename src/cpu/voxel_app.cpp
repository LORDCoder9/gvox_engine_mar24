#include "voxel_app.hpp"

#include <thread>
#include <numbers>
#include <fstream>
#include <random>
#include <unordered_map>

#include <gvox/adapters/input/byte_buffer.h>
#include <gvox/adapters/output/byte_buffer.h>
#include <gvox/adapters/parse/voxlap.h>

#define APPNAME "Voxel App"

using namespace std::chrono_literals;

#include <iostream>

#include <minizip/unzip.h>

constexpr auto round_frame_dim(u32vec2 size) {
    constexpr auto over_estimation = u32vec2{32, 32};
    auto result = (size + u32vec2{over_estimation.x - 1u, over_estimation.y - 1u}) / over_estimation * over_estimation;
    // not necessary, since it rounds up!
    // result = {std::max(result.x, over_estimation.x), std::max(result.y, over_estimation.y)};
    return result;
}

void RenderImages::create(daxa::Device &device) {
    rounded_size = round_frame_dim(size);
    depth_prepass_image = device.create_image({
        .format = daxa::Format::R32_SFLOAT,
        .size = {rounded_size.x / PREPASS_SCL, rounded_size.y / PREPASS_SCL, 1},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::SHADER_SAMPLED | daxa::ImageUsageFlagBits::TRANSFER_SRC,
        .name = "depth_prepass_image",
    });

    raster_color_image = device.create_image({
        .format = daxa::Format::R32G32B32A32_SFLOAT,
        .size = {size.x, size.y, 1},
        .usage = daxa::ImageUsageFlagBits::COLOR_ATTACHMENT | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "raster_color_image",
    });
    raster_depth_image = device.create_image({
        .format = daxa::Format::D32_SFLOAT,
        .size = {size.x, size.y, 1},
        .usage = daxa::ImageUsageFlagBits::DEPTH_STENCIL_ATTACHMENT | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "raster_depth_image",
    });

    g_buffer_image = device.create_image({
        .format = daxa::Format::R32G32B32A32_UINT,
        .size = {rounded_size.x, rounded_size.y, 1},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "g_buffer_image",
    });

    depth32_image = device.create_image({
        .format = daxa::Format::R32_SFLOAT,
        .size = {rounded_size.x, rounded_size.y, 1},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::SHADER_SAMPLED | daxa::ImageUsageFlagBits::TRANSFER_SRC,
        .name = "depth32_image",
    });
    scaled_depth32_image = device.create_image({
        .format = daxa::Format::R32_SFLOAT,
        .size = {rounded_size.x / SHADING_SCL, rounded_size.y / SHADING_SCL, 1},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::SHADER_SAMPLED | daxa::ImageUsageFlagBits::TRANSFER_DST,
        .name = "scaled_depth32_image",
    });
    ssao_image = device.create_image({
        .format = daxa::Format::R16_SFLOAT,
        .size = {rounded_size.x / SHADING_SCL, rounded_size.y / SHADING_SCL, 1},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::SHADER_SAMPLED | daxa::ImageUsageFlagBits::TRANSFER_DST,
        .name = "ssao_image",
    });

    indirect_diffuse_image = device.create_image({
        .format = daxa::Format::R32G32B32A32_SFLOAT,
        .size = {rounded_size.x / SHADING_SCL, rounded_size.y / SHADING_SCL, 1},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "indirect_diffuse_image",
    });
    for (auto &reconstructed_shading_image : reconstructed_shading_images) {
        reconstructed_shading_image = device.create_image({
            .format = daxa::Format::R32G32B32A32_SFLOAT,
            .size = {rounded_size.x, rounded_size.y, 1},
            .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
            .name = "reconstructed_shading_image",
        });
    }
}
void RenderImages::destroy(daxa::Device &device) const {
    device.destroy_image(depth_prepass_image);
    device.destroy_image(raster_color_image);
    device.destroy_image(raster_depth_image);
    device.destroy_image(g_buffer_image);
    device.destroy_image(depth32_image);
    device.destroy_image(scaled_depth32_image);
    device.destroy_image(ssao_image);
    device.destroy_image(indirect_diffuse_image);
    for (auto &reconstructed_shading_image : reconstructed_shading_images) {
        device.destroy_image(reconstructed_shading_image);
    }
}

void VoxelApp::set_task_render_images() {
    task_render_raster_color_image.set_images({.images = std::array{gpu_resources.render_images.raster_color_image}});
    task_render_raster_depth_image.set_images({.images = std::array{gpu_resources.render_images.raster_depth_image}});

    task_render_depth_prepass_image.set_images({.images = std::array{gpu_resources.render_images.depth_prepass_image}});

    task_render_g_buffer_image.set_images({.images = std::array{gpu_resources.render_images.g_buffer_image}});

    task_render_depth32_image.set_images({.images = std::array{gpu_resources.render_images.depth32_image}});
    task_render_scaled_depth32_image.set_images({.images = std::array{gpu_resources.render_images.scaled_depth32_image}});
    task_render_ssao_image.set_images({.images = std::array{gpu_resources.render_images.ssao_image}});

    task_render_indirect_diffuse_image.set_images({.images = std::array{gpu_resources.render_images.indirect_diffuse_image}});
    task_render_reconstructed_shading_image.set_images({.images = std::array{gpu_resources.render_images.reconstructed_shading_images[gpu_input.frame_index % 2]}});
    task_render_prev_reconstructed_shading_image.set_images({.images = std::array{gpu_resources.render_images.reconstructed_shading_images[1 - (gpu_input.frame_index % 2)]}});
}

void VoxelChunks::create(daxa::Device &device, u32 log2_chunks_per_axis) {
    auto chunk_n = (1u << log2_chunks_per_axis);
    chunk_n = chunk_n * chunk_n * chunk_n;
    buffer = device.create_buffer({
        .size = static_cast<u32>(sizeof(VoxelLeafChunk)) * chunk_n,
        .name = "voxel_chunks_buffer",
    });
}
void VoxelChunks::destroy(daxa::Device &device) const {
    if (!buffer.is_empty()) {
        device.destroy_buffer(buffer);
    }
}

void GpuResources::create(daxa::Device &device) {
    render_images.create(device);
    value_noise_image = device.create_image({
        .dimensions = 2,
        .format = daxa::Format::R8_UNORM,
        .size = {256, 256, 1},
        .array_layer_count = 256,
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "value_noise_image",
    });
    blue_noise_vec1_image = device.create_image({
        .dimensions = 3,
        .format = daxa::Format::R8G8B8A8_UNORM,
        .size = {128, 128, 64},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "blue_noise_vec1_image",
    });
    blue_noise_vec2_image = device.create_image({
        .dimensions = 3,
        .format = daxa::Format::R8G8B8A8_UNORM,
        .size = {128, 128, 64},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "blue_noise_vec2_image",
    });
    blue_noise_unit_vec3_image = device.create_image({
        .dimensions = 3,
        .format = daxa::Format::R8G8B8A8_UNORM,
        .size = {128, 128, 64},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "blue_noise_unit_vec3_image",
    });
    blue_noise_cosine_vec3_image = device.create_image({
        .dimensions = 3,
        .format = daxa::Format::R8G8B8A8_UNORM,
        .size = {128, 128, 64},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "blue_noise_cosine_vec3_image",
    });
    settings_buffer = device.create_buffer({
        .size = sizeof(GpuSettings),
        .name = "settings_buffer",
    });
    input_buffer = device.create_buffer({
        .size = sizeof(GpuInput),
        .name = "input_buffer",
    });
    output_buffer = device.create_buffer({
        .size = sizeof(GpuOutput) * (FRAMES_IN_FLIGHT + 1),
        .name = "output_buffer",
    });
    staging_output_buffer = device.create_buffer({
        .size = sizeof(GpuOutput) * (FRAMES_IN_FLIGHT + 1),
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
        .name = "staging_output_buffer",
    });
    globals_buffer = device.create_buffer({
        .size = sizeof(GpuGlobals),
        .name = "globals_buffer",
    });
    temp_voxel_chunks_buffer = device.create_buffer({
        .size = sizeof(TempVoxelChunk) * MAX_CHUNK_UPDATES_PER_FRAME,
        .name = "temp_voxel_chunks_buffer",
    });
    gvox_model_buffer = device.create_buffer({
        .size = static_cast<u32>(offsetof(GpuGvoxModel, data)),
        .name = "gvox_model_buffer",
    });
    simulated_voxel_particles_buffer = device.create_buffer({
        .size = sizeof(SimulatedVoxelParticle) * std::max<u32>(MAX_SIMULATED_VOXEL_PARTICLES, 1),
        .name = "simulated_voxel_particles_buffer",
    });
    rendered_voxel_particles_buffer = device.create_buffer({
        .size = sizeof(u32) * std::max<u32>(MAX_RENDERED_VOXEL_PARTICLES, 1),
        .name = "rendered_voxel_particles_buffer",
    });
    placed_voxel_particles_buffer = device.create_buffer({
        .size = sizeof(u32) * std::max<u32>(MAX_SIMULATED_VOXEL_PARTICLES, 1),
        .name = "placed_voxel_particles_buffer",
    });
    final_image_sampler = device.create_sampler({
        .magnification_filter = daxa::Filter::LINEAR,
        .minification_filter = daxa::Filter::LINEAR,
        .max_lod = 0.0f,
    });
    value_noise_sampler = device.create_sampler({
        .magnification_filter = daxa::Filter::LINEAR,
        .minification_filter = daxa::Filter::LINEAR,
        .address_mode_u = daxa::SamplerAddressMode::REPEAT,
        .address_mode_v = daxa::SamplerAddressMode::REPEAT,
        .address_mode_w = daxa::SamplerAddressMode::REPEAT,
        .max_lod = 0.0f,
    });
}
void GpuResources::destroy(daxa::Device &device) const {
    render_images.destroy(device);
    device.destroy_image(value_noise_image);
    device.destroy_image(blue_noise_vec1_image);
    device.destroy_image(blue_noise_vec2_image);
    device.destroy_image(blue_noise_unit_vec3_image);
    device.destroy_image(blue_noise_cosine_vec3_image);
    device.destroy_buffer(settings_buffer);
    device.destroy_buffer(input_buffer);
    device.destroy_buffer(output_buffer);
    device.destroy_buffer(staging_output_buffer);
    device.destroy_buffer(globals_buffer);
    device.destroy_buffer(temp_voxel_chunks_buffer);
    voxel_chunks.destroy(device);
    voxel_malloc.destroy(device);
    if (!gvox_model_buffer.is_empty()) {
        device.destroy_buffer(gvox_model_buffer);
    }
    voxel_leaf_chunk_malloc.destroy(device);
    voxel_parent_chunk_malloc.destroy(device);
    device.destroy_buffer(simulated_voxel_particles_buffer);
    device.destroy_buffer(rendered_voxel_particles_buffer);
    device.destroy_buffer(placed_voxel_particles_buffer);
    device.destroy_sampler(final_image_sampler);
    device.destroy_sampler(value_noise_sampler);
}

// Code flow
// VoxelApp::VoxelApp()
// GpuResources::create()
// VoxelApp::record_main_task_graph()
// VoxelApp::run()
// VoxelApp::on_update()

// [App initialization]
// Creates daxa instance, device, swapchain, pipeline manager
// Creates ui and imgui_renderer
// Creates task states
// Creates GPU Resources: GpuResources::create()
// Creates main task graph: VoxelApp::record_main_task_graph()
// Creates GVOX Context (gvox_ctx)
// Creates temp task graph
VoxelApp::VoxelApp()
    : AppWindow(APPNAME, {800, 600}),
      daxa_instance{daxa::create_instance({.enable_validation = false})},
      device{daxa_instance.create_device({
          // .enable_buffer_device_address_capture_replay = false,
          .name = "device",
      })},
      swapchain{device.create_swapchain({
          .native_window = AppWindow::get_native_handle(),
          .native_window_platform = AppWindow::get_native_platform(),
          .present_mode = daxa::PresentMode::IMMEDIATE,
          .image_usage = daxa::ImageUsageFlagBits::TRANSFER_DST,
          .max_allowed_frames_in_flight = FRAMES_IN_FLIGHT,
          .name = "swapchain",
      })},
      main_pipeline_manager{daxa::PipelineManager({
          .device = device,
          .shader_compile_options = {
              .root_paths = {
                  DAXA_SHADER_INCLUDE_DIR,
                  "assets",
                  "src",
                  "gpu",
                  "src/gpu",
              },
              .language = daxa::ShaderLanguage::GLSL,
              .enable_debug_info = true,
          },
          .register_null_pipelines_when_first_compile_fails = true,
          .name = "pipeline_manager",
      })},
      ui{[this]() {
          auto result = AppUi(AppWindow::glfw_window_ptr);
          auto const &device_props = device.properties();
          result.debug_gpu_name = reinterpret_cast<char const *>(device_props.device_name);
          return result;
      }()},
      imgui_renderer{[this]() {
          return daxa::ImGuiRenderer({
              .device = device,
              .format = swapchain.get_format(),
              .use_custom_config = false,
          });
      }()},
      gpu_resources{
          .render_images{.size{window_size}},
      },
      // clang-format off
      startup_task_state{main_pipeline_manager, ui},
      perframe_task_state{main_pipeline_manager, ui},
      chunk_edit_task_state{main_pipeline_manager, ui},
      chunk_opt_x2x4_task_state{main_pipeline_manager, ui},
      chunk_opt_x8up_task_state{main_pipeline_manager, ui},
      chunk_alloc_task_state{main_pipeline_manager, ui},
      trace_depth_prepass_task_state{main_pipeline_manager, ui, gpu_resources.render_images.rounded_size},
      trace_primary_task_state{main_pipeline_manager, ui, gpu_resources.render_images.rounded_size},
      downscale_task_state{main_pipeline_manager, ui, gpu_resources.render_images.rounded_size},
      ssao_task_state{main_pipeline_manager, ui, gpu_resources.render_images.rounded_size},
      trace_secondary_task_state{main_pipeline_manager, ui, gpu_resources.render_images.rounded_size},
      upscale_reconstruct_task_state{main_pipeline_manager, ui, gpu_resources.render_images.rounded_size},
      postprocessing_task_state{main_pipeline_manager, ui, gpu_resources.final_image_sampler, swapchain.get_format()},
#if ENABLE_HIERARCHICAL_QUEUE
      chunk_hierarchy_task_state{main_pipeline_manager, ui},
#else
      per_chunk_task_state{main_pipeline_manager, ui},
#endif
      voxel_particle_sim_task_state{main_pipeline_manager, ui},
      voxel_particle_raster_task_state{main_pipeline_manager, ui},
      // clang-format on
      main_task_graph{[this]() {
          gpu_resources.create(device);
          gpu_resources.voxel_chunks.create(device, ui.settings.log2_chunks_per_axis);
          gpu_resources.voxel_malloc.create(device);
          gpu_resources.voxel_leaf_chunk_malloc.create(device);
          gpu_resources.voxel_parent_chunk_malloc.create(device);
          return record_main_task_graph();
      }()},
      gvox_ctx(gvox_create_context()) {

    start = Clock::now();

    constexpr auto IMMEDIATE_LOAD_MODEL_FROM_GABES_DRIVE = false;
    if constexpr (IMMEDIATE_LOAD_MODEL_FROM_GABES_DRIVE) {
        ui.gvox_model_path = "C:/Users/gabe/AppData/Roaming/GabeVoxelGame/models/building.vox";
        gvox_model_data = load_gvox_data();
        model_is_ready = true;

        prev_gvox_model_buffer = gpu_resources.gvox_model_buffer;
        gpu_resources.gvox_model_buffer = device.create_buffer({
            .size = static_cast<u32>(gvox_model_data.size),
            .name = "gvox_model_buffer",
        });
        task_gvox_model_buffer.set_buffers({.buffers = std::array{gpu_resources.gvox_model_buffer}});
    }

    {
        daxa::TaskGraph temp_task_graph = daxa::TaskGraph({
            .device = device,
            .name = "temp_task_graph",
        });
        temp_task_graph.use_persistent_image(task_value_noise_image);
        temp_task_graph.use_persistent_image(task_blue_noise_vec1_image);
        temp_task_graph.use_persistent_image(task_blue_noise_vec2_image);
        temp_task_graph.use_persistent_image(task_blue_noise_unit_vec3_image);
        temp_task_graph.use_persistent_image(task_blue_noise_cosine_vec3_image);
        temp_task_graph.add_task({
            .uses = {
                daxa::TaskImageUse<daxa::TaskImageAccess::TRANSFER_WRITE>{task_value_noise_image.view().view({.layer_count = 256})},
            },
            .task = [this](daxa::TaskInterface task_runtime) {
                auto staging_buffer = device.create_buffer({
                    .size = static_cast<u32>(256 * 256 * 256 * 1),
                    .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                    .name = "staging_buffer",
                });
                auto *buffer_ptr = device.get_host_address_as<u8>(staging_buffer);
                auto seed_string = std::string{"gvox"};
                std::mt19937_64 rng(std::hash<std::string>{}(seed_string));
                std::uniform_int_distribution<std::mt19937::result_type> dist(0, 255);
                for (u32 i = 0; i < (256 * 256 * 256 * 1); ++i) {
                    buffer_ptr[i] = dist(rng) & 0xff;
                }
                auto cmd_list = task_runtime.get_command_list();
                cmd_list.pipeline_barrier({
                    .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
                });
                cmd_list.destroy_buffer_deferred(staging_buffer);
                for (u32 i = 0; i < 256; ++i) {
                    cmd_list.copy_buffer_to_image({
                        .buffer = staging_buffer,
                        .buffer_offset = 256 * 256 * i,
                        .image = task_value_noise_image.get_state().images[0],
                        .image_slice{
                            .base_array_layer = i,
                            .layer_count = 1,
                        },
                        .image_extent = {256, 256, 1},
                    });
                }
                needs_vram_calc = true;
            },
            .name = "upload_value_noise",
        });
        temp_task_graph.add_task({
            .uses = {
                daxa::TaskImageUse<daxa::TaskImageAccess::TRANSFER_WRITE>{task_blue_noise_vec1_image},
                daxa::TaskImageUse<daxa::TaskImageAccess::TRANSFER_WRITE>{task_blue_noise_vec2_image},
                daxa::TaskImageUse<daxa::TaskImageAccess::TRANSFER_WRITE>{task_blue_noise_unit_vec3_image},
                daxa::TaskImageUse<daxa::TaskImageAccess::TRANSFER_WRITE>{task_blue_noise_cosine_vec3_image},
            },
            .task = [this](daxa::TaskInterface task_runtime) {
                auto staging_buffer = device.create_buffer({
                    .size = static_cast<u32>(128 * 128 * 4 * 64 * 4),
                    .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                    .name = "staging_buffer",
                });
                auto *buffer_ptr = device.get_host_address_as<u8>(staging_buffer);
                auto *stbn_zip = unzOpen("assets/STBN.zip");
                for (auto i = 0; i < 64; ++i) {
                    [[maybe_unused]] int err = 0;
                    i32 size_x = 0;
                    i32 size_y = 0;
                    i32 channel_n = 0;
                    auto load_image = [&](char const *path, u8 *buffer_out_ptr) {
                        err = unzLocateFile(stbn_zip, path, 1);
                        assert(err == UNZ_OK);
                        auto file_info = unz_file_info{};
                        err = unzGetCurrentFileInfo(stbn_zip, &file_info, nullptr, 0, nullptr, 0, nullptr, 0);
                        assert(err == UNZ_OK);
                        auto file_data = std::vector<uint8_t>{};
                        file_data.resize(file_info.uncompressed_size);
                        err = unzOpenCurrentFile(stbn_zip);
                        assert(err == UNZ_OK);
                        err = unzReadCurrentFile(stbn_zip, file_data.data(), static_cast<uint32_t>(file_data.size()));
                        assert(err == file_data.size());
                        auto *temp_data = stbi_load_from_memory(file_data.data(), static_cast<int>(file_data.size()), &size_x, &size_y, &channel_n, 4);
                        if (temp_data != nullptr) {
                            assert(size_x == 128 && size_y == 128);
                            std::copy(temp_data + 0, temp_data + 128 * 128 * 4, buffer_out_ptr);
                        }
                    };
                    auto vec1_name = std::string{"STBN/stbn_vec1_2Dx1D_128x128x64_"} + std::to_string(i) + ".png";
                    auto vec2_name = std::string{"STBN/stbn_vec2_2Dx1D_128x128x64_"} + std::to_string(i) + ".png";
                    auto vec3_name = std::string{"STBN/stbn_unitvec3_2Dx1D_128x128x64_"} + std::to_string(i) + ".png";
                    auto cosine_vec3_name = std::string{"STBN/stbn_unitvec3_cosine_2Dx1D_128x128x64_"} + std::to_string(i) + ".png";
                    load_image(vec1_name.c_str(), buffer_ptr + (128 * 128 * 4) * i + (128 * 128 * 4 * 64) * 0);
                    load_image(vec2_name.c_str(), buffer_ptr + (128 * 128 * 4) * i + (128 * 128 * 4 * 64) * 1);
                    load_image(vec3_name.c_str(), buffer_ptr + (128 * 128 * 4) * i + (128 * 128 * 4 * 64) * 2);
                    load_image(cosine_vec3_name.c_str(), buffer_ptr + (128 * 128 * 4) * i + (128 * 128 * 4 * 64) * 3);
                }

                auto cmd_list = task_runtime.get_command_list();
                cmd_list.pipeline_barrier({
                    .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
                });
                cmd_list.destroy_buffer_deferred(staging_buffer);
                cmd_list.copy_buffer_to_image({
                    .buffer = staging_buffer,
                    .buffer_offset = (usize{128} * 128 * 4 * 64) * 0,
                    .image = task_blue_noise_vec1_image.get_state().images[0],
                    .image_extent = {128, 128, 64},
                });
                cmd_list.copy_buffer_to_image({
                    .buffer = staging_buffer,
                    .buffer_offset = (usize{128} * 128 * 4 * 64) * 1,
                    .image = task_blue_noise_vec2_image.get_state().images[0],
                    .image_extent = {128, 128, 64},
                });
                cmd_list.copy_buffer_to_image({
                    .buffer = staging_buffer,
                    .buffer_offset = (usize{128} * 128 * 4 * 64) * 2,
                    .image = task_blue_noise_unit_vec3_image.get_state().images[0],
                    .image_extent = {128, 128, 64},
                });
                cmd_list.copy_buffer_to_image({
                    .buffer = staging_buffer,
                    .buffer_offset = (usize{128} * 128 * 4 * 64) * 3,
                    .image = task_blue_noise_cosine_vec3_image.get_state().images[0],
                    .image_extent = {128, 128, 64},
                });
                needs_vram_calc = true;
            },
            .name = "upload_blue_noise",
        });
        temp_task_graph.submit({});
        temp_task_graph.complete({});
        temp_task_graph.execute({});
    }
}
VoxelApp::~VoxelApp() {
    gvox_destroy_context(gvox_ctx);
    device.wait_idle();
    device.collect_garbage();
    gpu_resources.destroy(device);
}

// [Main loop]
// handle resize event
// VoxelApp::on_update()
void VoxelApp::run() {
    while (true) {
        glfwPollEvents();
        if (glfwWindowShouldClose(AppWindow::glfw_window_ptr) != 0) {
            break;
        }

        if (!AppWindow::minimized) {
            auto resized = render_res_scl != ui.settings.render_res_scl;
            if (resized) {
                on_resize(window_size.x, window_size.y);
            }

            if (ui.settings.battery_saving_mode) {
                std::this_thread::sleep_for(10ms);
            }

            on_update();
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
}

void VoxelApp::calc_vram_usage() {
    auto &result_size = ui.debug_vram_usage;
    ui.debug_gpu_resource_infos.clear();
    result_size = 0;

    auto format_to_pixel_size = [](daxa::Format format) -> u32 {
        switch (format) {
        case daxa::Format::R16G16B16_SFLOAT: return 3 * 2;
        case daxa::Format::R16G16B16A16_SFLOAT: return 4 * 2;
        case daxa::Format::R32G32B32_SFLOAT: return 3 * 4;
        default:
        case daxa::Format::R32G32B32A32_SFLOAT: return 4 * 4;
        }
    };

    auto image_size = [this, &format_to_pixel_size, &result_size](daxa::ImageId image) {
        if (image.is_empty()) {
            return;
        }
        auto image_info = device.info_image(image);
        auto size = format_to_pixel_size(image_info.format) * image_info.size.x * image_info.size.y * image_info.size.z;
        ui.debug_gpu_resource_infos.push_back({
            .type = "image",
            .name = image_info.name,
            .size = size,
        });
        result_size += size;
    };
    auto buffer_size = [this, &result_size](daxa::BufferId buffer) {
        if (buffer.is_empty()) {
            return;
        }
        auto buffer_info = device.info_buffer(buffer);
        ui.debug_gpu_resource_infos.push_back({
            .type = "buffer",
            .name = buffer_info.name,
            .size = buffer_info.size,
        });
        result_size += buffer_info.size;
    };

    image_size(gpu_resources.render_images.depth_prepass_image);
    image_size(gpu_resources.render_images.raster_color_image);
    image_size(gpu_resources.render_images.raster_depth_image);
    image_size(gpu_resources.render_images.g_buffer_image);
    image_size(gpu_resources.render_images.depth32_image);
    image_size(gpu_resources.render_images.scaled_depth32_image);
    image_size(gpu_resources.render_images.ssao_image);
    image_size(gpu_resources.render_images.indirect_diffuse_image);
    image_size(gpu_resources.render_images.reconstructed_shading_images[0]);
    image_size(gpu_resources.render_images.reconstructed_shading_images[1]);

    buffer_size(gpu_resources.settings_buffer);
    buffer_size(gpu_resources.input_buffer);
    buffer_size(gpu_resources.globals_buffer);
    buffer_size(gpu_resources.temp_voxel_chunks_buffer);
    buffer_size(gpu_resources.voxel_chunks.buffer);

    gpu_resources.voxel_malloc.for_each_buffer(buffer_size);
    gpu_resources.voxel_leaf_chunk_malloc.for_each_buffer(buffer_size);
    gpu_resources.voxel_parent_chunk_malloc.for_each_buffer(buffer_size);

    buffer_size(gpu_resources.gvox_model_buffer);
    buffer_size(gpu_resources.simulated_voxel_particles_buffer);
    buffer_size(gpu_resources.rendered_voxel_particles_buffer);
    buffer_size(gpu_resources.placed_voxel_particles_buffer);

    needs_vram_calc = false;
}
auto VoxelApp::load_gvox_data() -> GvoxModelData {
    auto result = GvoxModelData{};
    auto file = std::ifstream(ui.gvox_model_path, std::ios::binary);
    if (!file.is_open()) {
        ui.console.add_log("[error] Failed to load the model");
        ui.should_upload_gvox_model = false;
        return result;
    }
    file.seekg(0, std::ios_base::end);
    auto temp_gvox_model_size = static_cast<u32>(file.tellg());
    auto temp_gvox_model = std::vector<uint8_t>{};
    temp_gvox_model.resize(temp_gvox_model_size);
    {
        // time_t start = clock();
        file.seekg(0, std::ios_base::beg);
        file.read(reinterpret_cast<char *>(temp_gvox_model.data()), static_cast<std::streamsize>(temp_gvox_model_size));
        file.close();
        // time_t end = clock();
        // double cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
        // ui.console.add_log("(pulling file into memory: {}s)", cpu_time_used);
    }
    GvoxByteBufferInputAdapterConfig i_config = {
        .data = temp_gvox_model.data(),
        .size = temp_gvox_model_size,
    };
    GvoxByteBufferOutputAdapterConfig o_config = {
        .out_size = &result.size,
        .out_byte_buffer_ptr = &result.ptr,
        .allocate = nullptr,
    };
    void *i_config_ptr = nullptr;
    auto voxlap_config = GvoxVoxlapParseAdapterConfig{
        .size_x = 512,
        .size_y = 512,
        .size_z = 64,
        .make_solid = 1,
        .is_ace_of_spades = 1,
    };
    char const *gvox_model_type = "gvox_palette";
    if (ui.gvox_model_path.has_extension()) {
        auto ext = ui.gvox_model_path.extension();
        if (ext == ".vox") {
            gvox_model_type = "magicavoxel";
        }
        if (ext == ".rle") {
            gvox_model_type = "gvox_run_length_encoding";
        }
        if (ext == ".oct") {
            gvox_model_type = "gvox_octree";
        }
        if (ext == ".glp") {
            gvox_model_type = "gvox_global_palette";
        }
        if (ext == ".brk") {
            gvox_model_type = "gvox_brickmap";
        }
        if (ext == ".gvr") {
            gvox_model_type = "gvox_raw";
        }
        if (ext == ".vxl") {
            i_config_ptr = &voxlap_config;
            gvox_model_type = "voxlap";
        }
    }
    GvoxAdapterContext *i_ctx = gvox_create_adapter_context(gvox_ctx, gvox_get_input_adapter(gvox_ctx, "byte_buffer"), &i_config);
    GvoxAdapterContext *o_ctx = gvox_create_adapter_context(gvox_ctx, gvox_get_output_adapter(gvox_ctx, "byte_buffer"), &o_config);
    GvoxAdapterContext *p_ctx = gvox_create_adapter_context(gvox_ctx, gvox_get_parse_adapter(gvox_ctx, gvox_model_type), i_config_ptr);
    GvoxAdapterContext *s_ctx = gvox_create_adapter_context(gvox_ctx, gvox_get_serialize_adapter(gvox_ctx, "gvox_palette"), nullptr);

    {
        // time_t start = clock();
        gvox_blit_region(
            i_ctx, o_ctx, p_ctx, s_ctx,
            nullptr,
            // &ui.gvox_region_range,
            // GVOX_CHANNEL_BIT_COLOR | GVOX_CHANNEL_BIT_MATERIAL_ID | GVOX_CHANNEL_BIT_EMISSIVITY);
            GVOX_CHANNEL_BIT_COLOR);
        // time_t end = clock();
        // double cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
        // ui.console.add_log("{}s, new size: {} bytes", cpu_time_used, result.size);

        GvoxResult res = gvox_get_result(gvox_ctx);
        // int error_count = 0;
        while (res != GVOX_RESULT_SUCCESS) {
            size_t size = 0;
            gvox_get_result_message(gvox_ctx, nullptr, &size);
            char *str = new char[size + 1];
            gvox_get_result_message(gvox_ctx, str, nullptr);
            str[size] = '\0';
            ui.console.add_log("ERROR loading model: {}", str);
            gvox_pop_result(gvox_ctx);
            delete[] str;
            res = gvox_get_result(gvox_ctx);
            // ++error_count;
        }
    }

    gvox_destroy_adapter_context(i_ctx);
    gvox_destroy_adapter_context(o_ctx);
    gvox_destroy_adapter_context(p_ctx);
    gvox_destroy_adapter_context(s_ctx);
    return result;
}

// [Update engine state]
// Reload pipeline manager
// Update UI
// Update swapchain
// Recreate voxel chunks (conditional)
// Handle GVOX model upload
// Voxel malloc management
// Execute main task graph
void VoxelApp::on_update() {
    auto now = Clock::now();
    gpu_input.time = std::chrono::duration<f32>(now - start).count();
    gpu_input.delta_time = std::chrono::duration<f32>(now - prev_time).count();
    prev_time = now;
    gpu_input.frame_dim = gpu_resources.render_images.size;
    gpu_input.rounded_frame_dim = gpu_resources.render_images.rounded_size;
    gpu_input.render_res_scl = ui.settings.render_res_scl;

    {
        auto reload_result = main_pipeline_manager.reload_all();
        if (auto *reload_err = std::get_if<daxa::PipelineReloadError>(&reload_result)) {
            ui.console.add_log(reload_err->message);
        }
    }

    ui.update(gpu_input.delta_time);

    swapchain_image = swapchain.acquire_next_image();
    task_swapchain_image.set_images({.images = {&swapchain_image, 1}});
    if (swapchain_image.is_empty()) {
        return;
    }

    if (ui.should_recreate_voxel_buffers) {
        recreate_voxel_chunks();
    }

    if (ui.should_upload_gvox_model) {
        if (!model_is_loading) {
            gvox_model_data_future = std::async(std::launch::async, &VoxelApp::load_gvox_data, this);
            model_is_loading = true;
        }
        if (model_is_loading && gvox_model_data_future.wait_for(0.01s) == std::future_status::ready) {
            model_is_ready = true;
            model_is_loading = false;

            gvox_model_data = gvox_model_data_future.get();
            prev_gvox_model_buffer = gpu_resources.gvox_model_buffer;
            gpu_resources.gvox_model_buffer = device.create_buffer({
                .size = static_cast<u32>(gvox_model_data.size),
                .name = "gvox_model_buffer",
            });
            task_gvox_model_buffer.set_buffers({.buffers = std::array{gpu_resources.gvox_model_buffer}});
        }
    }

    if (needs_vram_calc) {
        calc_vram_usage();
    }

    gpu_resources.voxel_malloc.check_for_realloc(device, gpu_output.voxel_malloc_output.current_element_count);
    gpu_resources.voxel_leaf_chunk_malloc.check_for_realloc(device, gpu_output.voxel_leaf_chunk_output.current_element_count);
    gpu_resources.voxel_parent_chunk_malloc.check_for_realloc(device, gpu_output.voxel_parent_chunk_output.current_element_count);

    condition_values[static_cast<usize>(Conditions::STARTUP)] = ui.should_run_startup || model_is_ready;
    condition_values[static_cast<usize>(Conditions::UPLOAD_SETTINGS)] = ui.should_upload_settings;
    condition_values[static_cast<usize>(Conditions::UPLOAD_GVOX_MODEL)] = model_is_ready;
    condition_values[static_cast<usize>(Conditions::DYNAMIC_BUFFERS_REALLOC)] =
        gpu_resources.voxel_malloc.needs_realloc() ||
        gpu_resources.voxel_leaf_chunk_malloc.needs_realloc() ||
        gpu_resources.voxel_parent_chunk_malloc.needs_realloc();
    gpu_input.fif_index = gpu_input.frame_index % (FRAMES_IN_FLIGHT + 1);
    main_task_graph.execute({.permutation_condition_values = condition_values});
    model_is_ready = false;

    gpu_input.resize_factor = 1.0f;
    gpu_input.mouse.pos_delta = {0.0f, 0.0f};
    gpu_input.mouse.scroll_delta = {0.0f, 0.0f};

    ui.debug_gpu_heap_usage = gpu_output.voxel_malloc_output.current_element_count * VOXEL_MALLOC_PAGE_SIZE_BYTES;
    ui.debug_player_pos = gpu_output.player_pos;
    ui.debug_chunk_offset = gpu_output.chunk_offset;
    ui.debug_page_count = gpu_resources.voxel_malloc.current_element_count;
    ui.debug_job_counters = std::bit_cast<ChunkHierarchyJobCounters>(gpu_output.job_counters_packed);
    ui.debug_total_jobs_ran = gpu_output.total_jobs_ran;

    // task_render_pos_image.swap_images(task_render_prev_pos_image);
    // task_render_col_image.swap_images(task_render_prev_col_image);
    task_render_reconstructed_shading_image.swap_images(task_render_prev_reconstructed_shading_image);
    ++gpu_input.frame_index;
}
void VoxelApp::on_mouse_move(f32 x, f32 y) {
    f32vec2 const center = {static_cast<f32>(window_size.x / 2), static_cast<f32>(window_size.y / 2)};
    gpu_input.mouse.pos = f32vec2{x, y};
    auto offset = gpu_input.mouse.pos - center;
    gpu_input.mouse.pos = gpu_input.mouse.pos *f32vec2{static_cast<f32>(gpu_resources.render_images.size.x), static_cast<f32>(gpu_resources.render_images.size.y)} / f32vec2{static_cast<f32>(window_size.x), static_cast<f32>(window_size.y)};
    if (!ui.paused) {
        gpu_input.mouse.pos_delta = gpu_input.mouse.pos_delta + offset;
        set_mouse_pos(center.x, center.y);
    }
}
void VoxelApp::on_mouse_scroll(f32 dx, f32 dy) {
    auto &io = ImGui::GetIO();
    if (io.WantCaptureMouse) {
        return;
    }

    gpu_input.mouse.scroll_delta = gpu_input.mouse.scroll_delta + f32vec2{dx, dy};
}
void VoxelApp::on_mouse_button(i32 button_id, i32 action) {
    auto &io = ImGui::GetIO();
    if (io.WantCaptureMouse) {
        return;
    }
    if (ui.limbo_action_index != GAME_ACTION_LAST + 1) {
        return;
    }

    if (ui.settings.mouse_button_binds.contains(button_id)) {
        gpu_input.actions[ui.settings.mouse_button_binds.at(button_id)] = static_cast<u32>(action);
    }
}
void VoxelApp::on_key(i32 key_id, i32 action) {
    auto &io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
        return;
    }
    if (ui.limbo_action_index != GAME_ACTION_LAST + 1) {
        return;
    }

    if (key_id == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        std::fill(std::begin(gpu_input.actions), std::end(gpu_input.actions), 0);
        ui.toggle_pause();
        set_mouse_capture(!ui.paused);
    }

    if (key_id == GLFW_KEY_F3 && action == GLFW_PRESS) {
        ui.toggle_debug();
    }

    if (ui.paused) {
        if (key_id == GLFW_KEY_F1 && action == GLFW_PRESS) {
            ui.toggle_help();
        }
        if (key_id == GLFW_KEY_GRAVE_ACCENT && action == GLFW_PRESS) {
            ui.toggle_console();
        }
    }

    if (key_id == GLFW_KEY_R && action == GLFW_PRESS) {
        ui.should_run_startup = true;
        start = Clock::now();
    }

    if (ui.settings.keybinds.contains(key_id)) {
        gpu_input.actions[ui.settings.keybinds.at(key_id)] = static_cast<u32>(action);
    }
}
void VoxelApp::on_resize(u32 sx, u32 sy) {
    minimized = (sx == 0 || sy == 0);
    auto resized = sx != window_size.x || sy != window_size.y || render_res_scl != ui.settings.render_res_scl;
    if (!minimized && resized) {
        swapchain.resize();
        window_size.x = swapchain.get_surface_extent().x;
        window_size.y = swapchain.get_surface_extent().y;
        render_res_scl = ui.settings.render_res_scl;
        {
            // resize render images
            gpu_resources.render_images.size.x = static_cast<u32>(static_cast<f32>(window_size.x) * render_res_scl);
            gpu_resources.render_images.size.y = static_cast<u32>(static_cast<f32>(window_size.y) * render_res_scl);
            recreate_render_images();
        }
        gpu_input.resize_factor = 0.0f;
        on_update();
    }
}
void VoxelApp::on_drop(std::span<char const *> filepaths) {
    ui.gvox_model_path = filepaths[0];
    ui.should_upload_gvox_model = true;
}

void VoxelApp::recreate_render_images() {
    device.wait_idle();
    gpu_resources.render_images.destroy(device);
    gpu_resources.render_images.create(device);
    set_task_render_images();
    needs_vram_calc = true;
}
void VoxelApp::recreate_voxel_chunks() {
    gpu_resources.voxel_chunks.destroy(device);
    gpu_resources.voxel_chunks.create(device, ui.settings.log2_chunks_per_axis);
    task_voxel_chunks_buffer.set_buffers({.buffers = {&gpu_resources.voxel_chunks.buffer, 1}});
    ui.should_recreate_voxel_buffers = false;
    needs_vram_calc = true;
}

// [Engine initializations tasks]
//
// Startup Task (Globals Clear):
// Clear task_globals_buffer
// Clear task_temp_voxel_chunks_buffer
// Clear task_voxel_chunks_buffer
// Clear task_voxel_malloc_pages_buffer (x3)
//
// GPU Task:
// startup.comp.glsl (Run on 1 thread)
//
// Initialize Task:
// init VoxelMallocPageAllocator buffer
void VoxelApp::run_startup(daxa::TaskGraph &temp_task_graph) {
    temp_task_graph.add_task({
        .uses = {
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{task_globals_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{task_temp_voxel_chunks_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{task_voxel_chunks_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{gpu_resources.voxel_malloc.task_element_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{gpu_resources.voxel_leaf_chunk_malloc.task_element_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{gpu_resources.voxel_parent_chunk_malloc.task_element_buffer},
        },
        .task = [this](daxa::TaskInterface task_runtime) {
            auto cmd_list = task_runtime.get_command_list();
            cmd_list.clear_buffer({
                .buffer = task_globals_buffer.get_state().buffers[0],
                .offset = 0,
                .size = sizeof(GpuGlobals),
                .clear_value = 0,
            });
            cmd_list.clear_buffer({
                .buffer = task_temp_voxel_chunks_buffer.get_state().buffers[0],
                .offset = 0,
                .size = sizeof(TempVoxelChunk) * MAX_CHUNK_UPDATES_PER_FRAME,
                .clear_value = 0,
            });
            auto chunk_n = (1u << ui.settings.log2_chunks_per_axis);
            chunk_n = chunk_n * chunk_n * chunk_n;
            cmd_list.clear_buffer({
                .buffer = task_voxel_chunks_buffer.get_state().buffers[0],
                .offset = 0,
                .size = sizeof(VoxelLeafChunk) * chunk_n,
                .clear_value = 0,
            });
            gpu_resources.voxel_malloc.clear_buffers(cmd_list);
            gpu_resources.voxel_leaf_chunk_malloc.clear_buffers(cmd_list);
            gpu_resources.voxel_parent_chunk_malloc.clear_buffers(cmd_list);
        },
        .name = "StartupTask (Globals Clear)",
    });
    temp_task_graph.add_task(StartupComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .globals = task_globals_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
            },
        },
        &startup_task_state,
    });

    temp_task_graph.add_task({
        .uses = {
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{gpu_resources.voxel_malloc.task_allocator_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{gpu_resources.voxel_leaf_chunk_malloc.task_allocator_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{gpu_resources.voxel_parent_chunk_malloc.task_allocator_buffer},
        },
        .task = [this](daxa::TaskInterface task_runtime) {
            auto cmd_list = task_runtime.get_command_list();
            gpu_resources.voxel_malloc.init(device, cmd_list);
            gpu_resources.voxel_leaf_chunk_malloc.init(device, cmd_list);
            gpu_resources.voxel_parent_chunk_malloc.init(device, cmd_list);
        },
        .name = "Initialize",
    });
}

// [Task: Upload settings to GPU]
// Init task_settings_buffer
void VoxelApp::upload_settings(daxa::TaskGraph &temp_task_graph) {
    temp_task_graph.add_task({
        .uses = {
            daxa::TaskBufferUse<daxa::TaskBufferAccess::HOST_TRANSFER_WRITE>{task_settings_buffer},
        },
        .task = [this](daxa::TaskInterface task_runtime) {
            auto cmd_list = task_runtime.get_command_list();
            auto staging_settings_buffer = device.create_buffer({
                .size = sizeof(GpuSettings),
                .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                .name = "staging_settings_buffer",
            });
            cmd_list.destroy_buffer_deferred(staging_settings_buffer);
            auto *buffer_ptr = device.get_host_address_as<GpuSettings>(staging_settings_buffer);
            *buffer_ptr = {
                .fov = ui.settings.camera_fov * (std::numbers::pi_v<f32> / 180.0f),
                .sensitivity = ui.settings.mouse_sensitivity,
                .log2_chunks_per_axis = ui.settings.log2_chunks_per_axis,
            };
            cmd_list.copy_buffer_to_buffer({
                .src_buffer = staging_settings_buffer,
                .dst_buffer = task_settings_buffer.get_state().buffers[0],
                .size = sizeof(GpuSettings),
            });
            ui.should_upload_settings = false;
        },
        .name = "StartupTask (Globals Clear)",
    });
}

void VoxelApp::upload_model(daxa::TaskGraph &temp_task_graph) {
    temp_task_graph.add_task({
        .uses = {
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{task_gvox_model_buffer},
        },
        .task = [this](daxa::TaskInterface task_runtime) {
            auto cmd_list = task_runtime.get_command_list();
            if (!prev_gvox_model_buffer.is_empty()) {
                cmd_list.destroy_buffer_deferred(prev_gvox_model_buffer);
            }
            cmd_list.pipeline_barrier({
                .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
            });
            auto staging_gvox_model_buffer = device.create_buffer({
                .size = static_cast<u32>(gvox_model_data.size),
                .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                .name = "staging_gvox_model_buffer",
            });
            cmd_list.destroy_buffer_deferred(staging_gvox_model_buffer);
            char *buffer_ptr = device.get_host_address_as<char>(staging_gvox_model_buffer);
            std::copy(gvox_model_data.ptr, gvox_model_data.ptr + gvox_model_data.size, buffer_ptr);
            if (gvox_model_data.ptr != nullptr) {
                free(gvox_model_data.ptr);
            }
            cmd_list.copy_buffer_to_buffer({
                .src_buffer = staging_gvox_model_buffer,
                .dst_buffer = gpu_resources.gvox_model_buffer,
                .size = static_cast<u32>(gvox_model_data.size),
            });
            ui.should_upload_gvox_model = false;
            has_model = true;
            needs_vram_calc = true;
        },
        .name = "upload_model",
    });
}

void VoxelApp::dynamic_buffers_realloc(daxa::TaskGraph &temp_task_graph) {
    temp_task_graph.add_task({
        .uses = {
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_READ>{gpu_resources.voxel_malloc.task_old_element_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{gpu_resources.voxel_malloc.task_element_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_READ>{gpu_resources.voxel_leaf_chunk_malloc.task_old_element_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{gpu_resources.voxel_leaf_chunk_malloc.task_element_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_READ>{gpu_resources.voxel_parent_chunk_malloc.task_old_element_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{gpu_resources.voxel_parent_chunk_malloc.task_element_buffer},
        },
        .task = [&](daxa::TaskInterface task_runtime) {
            auto cmd_list = task_runtime.get_command_list();
            if (gpu_resources.voxel_malloc.needs_realloc()) {
                gpu_resources.voxel_malloc.realloc(device, cmd_list);
                needs_vram_calc = true;
            }
            if (gpu_resources.voxel_leaf_chunk_malloc.needs_realloc()) {
                gpu_resources.voxel_leaf_chunk_malloc.realloc(device, cmd_list);
                needs_vram_calc = true;
            }
            if (gpu_resources.voxel_parent_chunk_malloc.needs_realloc()) {
                gpu_resources.voxel_parent_chunk_malloc.realloc(device, cmd_list);
                needs_vram_calc = true;
            }
        },
        .name = "Transfer Task",
    });
}

// [Record the command list sent to the GPU each frame]

// List of tasks:

// (Conditional tasks)
// Startup (startup.comp.glsl), run on 1 thread
// Upload settings, Upload model, Voxel malloc realloc

// GpuInputUploadTransferTask
// -> copy buffer from gpu_input to task_input_buffer

// PerframeTask (perframe.comp.glsl), run on 1 thread
// -> player_perframe() : update player
// -> voxel_world_perframe() : init voxel world
// -> update brush
// -> update voxel_malloc_page_allocator
// -> update thread pool
// -> update particles

// VoxelParticleSimComputeTask
// -> Simulate the particles

// ChunkHierarchy (chunk_hierarchy.comp.glsl) (x2)
// -> Creates hierarchical structure / chunk work items to be generated and processed by Chunk Edit

// ChunkEdit (chunk_edit.comp.glsl)
// -> Actually build the chunks depending on the chunk work items (containing brush infos)

// ChunkOpt_x2x4                 [Optim]
// ChunkOpt_x8up                 [Optim]
// ChunkAlloc                    [Optim]
// VoxelParticleRasterTask       [Particles]
// TraceDepthPrepassTask         [Optim]
// TracePrimaryTask              [Render]
// ColorSceneTask                [Render]
// GpuOutputDownloadTransferTask [I/O]
// PostprocessingTask            [Render]
// ImGui draw                    [GUI Render]
auto VoxelApp::record_main_task_graph() -> daxa::TaskGraph {
    daxa::TaskGraph result_task_graph = daxa::TaskGraph({
        .device = device,
        .swapchain = swapchain,
        .permutation_condition_count = static_cast<usize>(Conditions::LAST),
        .name = "main_task_graph",
    });

    result_task_graph.use_persistent_image(task_value_noise_image);
    result_task_graph.use_persistent_image(task_blue_noise_vec1_image);
    result_task_graph.use_persistent_image(task_blue_noise_vec2_image);
    result_task_graph.use_persistent_image(task_blue_noise_unit_vec3_image);
    result_task_graph.use_persistent_image(task_blue_noise_cosine_vec3_image);
    task_value_noise_image.set_images({.images = std::array{gpu_resources.value_noise_image}});
    task_blue_noise_vec1_image.set_images({.images = std::array{gpu_resources.blue_noise_vec1_image}});
    task_blue_noise_vec2_image.set_images({.images = std::array{gpu_resources.blue_noise_vec2_image}});
    task_blue_noise_unit_vec3_image.set_images({.images = std::array{gpu_resources.blue_noise_unit_vec3_image}});
    task_blue_noise_cosine_vec3_image.set_images({.images = std::array{gpu_resources.blue_noise_cosine_vec3_image}});

    result_task_graph.use_persistent_buffer(task_settings_buffer);
    result_task_graph.use_persistent_buffer(task_input_buffer);
    result_task_graph.use_persistent_buffer(task_output_buffer);
    result_task_graph.use_persistent_buffer(task_staging_output_buffer);
    result_task_graph.use_persistent_buffer(task_globals_buffer);
    result_task_graph.use_persistent_buffer(task_temp_voxel_chunks_buffer);
    result_task_graph.use_persistent_buffer(task_voxel_chunks_buffer);
    result_task_graph.use_persistent_buffer(task_gvox_model_buffer);
    gpu_resources.voxel_malloc.for_each_task_buffer([&result_task_graph](auto &task_buffer) { result_task_graph.use_persistent_buffer(task_buffer); });
    gpu_resources.voxel_leaf_chunk_malloc.for_each_task_buffer([&result_task_graph](auto &task_buffer) { result_task_graph.use_persistent_buffer(task_buffer); });
    gpu_resources.voxel_parent_chunk_malloc.for_each_task_buffer([&result_task_graph](auto &task_buffer) { result_task_graph.use_persistent_buffer(task_buffer); });

    task_settings_buffer.set_buffers({.buffers = std::array{gpu_resources.settings_buffer}});
    task_input_buffer.set_buffers({.buffers = std::array{gpu_resources.input_buffer}});
    task_output_buffer.set_buffers({.buffers = std::array{gpu_resources.output_buffer}});
    task_staging_output_buffer.set_buffers({.buffers = std::array{gpu_resources.staging_output_buffer}});
    task_globals_buffer.set_buffers({.buffers = std::array{gpu_resources.globals_buffer}});
    task_temp_voxel_chunks_buffer.set_buffers({.buffers = std::array{gpu_resources.temp_voxel_chunks_buffer}});
    task_voxel_chunks_buffer.set_buffers({.buffers = std::array{gpu_resources.voxel_chunks.buffer}});
    task_gvox_model_buffer.set_buffers({.buffers = std::array{gpu_resources.gvox_model_buffer}});

    result_task_graph.use_persistent_buffer(task_simulated_voxel_particles_buffer);
    result_task_graph.use_persistent_buffer(task_rendered_voxel_particles_buffer);
    result_task_graph.use_persistent_buffer(task_placed_voxel_particles_buffer);
    task_simulated_voxel_particles_buffer.set_buffers({.buffers = std::array{gpu_resources.simulated_voxel_particles_buffer}});
    task_rendered_voxel_particles_buffer.set_buffers({.buffers = std::array{gpu_resources.rendered_voxel_particles_buffer}});
    task_placed_voxel_particles_buffer.set_buffers({.buffers = std::array{gpu_resources.placed_voxel_particles_buffer}});

    result_task_graph.use_persistent_image(task_swapchain_image);
    result_task_graph.use_persistent_image(task_render_depth_prepass_image);
    result_task_graph.use_persistent_image(task_render_raster_color_image);
    result_task_graph.use_persistent_image(task_render_raster_depth_image);
    result_task_graph.use_persistent_image(task_render_g_buffer_image);
    result_task_graph.use_persistent_image(task_render_depth32_image);
    result_task_graph.use_persistent_image(task_render_scaled_depth32_image);
    result_task_graph.use_persistent_image(task_render_ssao_image);
    result_task_graph.use_persistent_image(task_render_indirect_diffuse_image);
    result_task_graph.use_persistent_image(task_render_reconstructed_shading_image);
    result_task_graph.use_persistent_image(task_render_prev_reconstructed_shading_image);
    task_swapchain_image.set_images({.images = std::array{swapchain_image}});
    set_task_render_images();

    result_task_graph.conditional({
        .condition_index = static_cast<u32>(Conditions::STARTUP),
        .when_true = [&, this]() { this->run_startup(result_task_graph); },
    });
    result_task_graph.conditional({
        .condition_index = static_cast<u32>(Conditions::UPLOAD_SETTINGS),
        .when_true = [&, this]() { this->upload_settings(result_task_graph); },
    });
    result_task_graph.conditional({
        .condition_index = static_cast<u32>(Conditions::UPLOAD_GVOX_MODEL),
        .when_true = [&, this]() { this->upload_model(result_task_graph); },
    });
    result_task_graph.conditional({
        .condition_index = static_cast<u32>(Conditions::DYNAMIC_BUFFERS_REALLOC),
        .when_true = [&, this]() { this->dynamic_buffers_realloc(result_task_graph); },
    });

    // GpuInputUploadTransferTask
    result_task_graph.add_task({
        .uses = {
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_WRITE>{task_input_buffer},
        },
        .task = [this](daxa::TaskInterface task_runtime) {
            auto cmd_list = task_runtime.get_command_list();
            auto staging_input_buffer = device.create_buffer({
                .size = sizeof(GpuInput),
                .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                .name = "staging_input_buffer",
            });
            cmd_list.destroy_buffer_deferred(staging_input_buffer);
            auto *buffer_ptr = device.get_host_address_as<GpuInput>(staging_input_buffer);
            *buffer_ptr = gpu_input;
            cmd_list.copy_buffer_to_buffer({
                .src_buffer = staging_input_buffer,
                .dst_buffer = task_input_buffer.get_state().buffers[0],
                .size = sizeof(GpuInput),
            });
        },
        .name = "GpuInputUploadTransferTask",
    });

    // PerframeTask
    result_task_graph.add_task(PerframeComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .gpu_output = task_output_buffer,
                .globals = task_globals_buffer,
                .simulated_voxel_particles = task_simulated_voxel_particles_buffer,
                .voxel_malloc_page_allocator = gpu_resources.voxel_malloc.task_allocator_buffer,
                .voxel_leaf_chunk_allocator = gpu_resources.voxel_leaf_chunk_malloc.task_allocator_buffer,
                .voxel_parent_chunk_allocator = gpu_resources.voxel_parent_chunk_malloc.task_allocator_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
            },
        },
        &perframe_task_state,
    });

#if MAX_RENDERED_VOXEL_PARTICLES > 0
    // VoxelParticleSimComputeTask
    result_task_graph.add_task(VoxelParticleSimComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .globals = task_globals_buffer,
                .voxel_malloc_page_allocator = task_voxel_malloc_page_allocator_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
                .simulated_voxel_particles = task_simulated_voxel_particles_buffer,
                .rendered_voxel_particles = task_rendered_voxel_particles_buffer,
                .placed_voxel_particles = task_placed_voxel_particles_buffer,
            },
        },
        &voxel_particle_sim_task_state,
    });
#endif

#if ENABLE_HIERARCHICAL_QUEUE
    // ChunkHierarchy
    result_task_graph.add_task(ChunkHierarchyComputeTaskL0{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .gvox_model = task_gvox_model_buffer,
                .globals = task_globals_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
            },
        },
        &chunk_hierarchy_task_state,
    });
    result_task_graph.add_task(ChunkHierarchyComputeTaskL1{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .gvox_model = task_gvox_model_buffer,
                .globals = task_globals_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
            },
        },
        &chunk_hierarchy_task_state,
    });
#else
    result_task_graph.add_task(PerChunkComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .gvox_model = task_gvox_model_buffer,
                .globals = task_globals_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
            },
        },
        &per_chunk_task_state,
    });
#endif

    // ChunkEdit
    result_task_graph.add_task(ChunkEditComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .globals = task_globals_buffer,
                .gvox_model = task_gvox_model_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
                .temp_voxel_chunks = task_temp_voxel_chunks_buffer,
                .voxel_malloc_page_allocator = gpu_resources.voxel_malloc.task_allocator_buffer,
                .simulated_voxel_particles = task_simulated_voxel_particles_buffer,
                .placed_voxel_particles = task_placed_voxel_particles_buffer,
                .value_noise_texture = task_value_noise_image.view().view({.layer_count = 256}),
            },
        },
        &chunk_edit_task_state,
        &gpu_resources.value_noise_sampler,
    });

    // ChunkOpt_x2x4
    result_task_graph.add_task(ChunkOpt_x2x4_ComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .globals = task_globals_buffer,
                .temp_voxel_chunks = task_temp_voxel_chunks_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
            },
        },
        &chunk_opt_x2x4_task_state,
    });

    // ChunkOpt_x8up
    result_task_graph.add_task(ChunkOpt_x8up_ComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .globals = task_globals_buffer,
                .temp_voxel_chunks = task_temp_voxel_chunks_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
            },
        },
        &chunk_opt_x8up_task_state,
    });

    // ChunkAlloc
    result_task_graph.add_task(ChunkAllocComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .globals = task_globals_buffer,
                .temp_voxel_chunks = task_temp_voxel_chunks_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
                .voxel_malloc_page_allocator = gpu_resources.voxel_malloc.task_allocator_buffer,
            },
        },
        &chunk_alloc_task_state,
    });

#if MAX_RENDERED_VOXEL_PARTICLES > 0
    // VoxelParticleRasterTask
    result_task_graph.add_task(VoxelParticleRasterTask{
        {
            .uses = {
                .gpu_input = task_input_buffer,
                .globals = task_globals_buffer,
                .simulated_voxel_particles = task_simulated_voxel_particles_buffer,
                .rendered_voxel_particles = task_rendered_voxel_particles_buffer,
                .render_image = task_render_raster_color_image,
                .depth_image = task_render_raster_depth_image,
            },
        },
        &voxel_particle_raster_task_state,
    });
#endif

    // TraceDepthPrepassTask
    result_task_graph.add_task(TraceDepthPrepassComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .globals = task_globals_buffer,
                .voxel_malloc_page_allocator = gpu_resources.voxel_malloc.task_allocator_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
                .blue_noise_vec2 = task_blue_noise_vec2_image,
                .render_depth_prepass_image = task_render_depth_prepass_image,
            },
        },
        &trace_depth_prepass_task_state,
    });

    // TracePrimaryTask
    result_task_graph.add_task(TracePrimaryComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .globals = task_globals_buffer,
                .voxel_malloc_page_allocator = gpu_resources.voxel_malloc.task_allocator_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
                .blue_noise_vec2 = task_blue_noise_vec2_image,
                .render_depth_prepass_image = task_render_depth_prepass_image,
                .g_buffer_image_id = task_render_g_buffer_image,
                .depth_image_id = task_render_depth32_image,
            },
        },
        &trace_primary_task_state,
    });

    // result_task_graph.add_task({
    //     .uses = {
    //         daxa::TaskImageUse<daxa::TaskImageAccess::TRANSFER_READ>{task_render_depth32_image},
    //         daxa::TaskImageUse<daxa::TaskImageAccess::TRANSFER_WRITE>{task_render_scaled_depth32_image},
    //     },
    //     .task = [this](daxa::TaskInterface task_runtime) {
    //         auto cmd_list = task_runtime.get_command_list();
    //         auto size0 = task_runtime.get_device().info_image(task_render_depth32_image.get_state().images[0]).size;
    //         auto size1 = task_runtime.get_device().info_image(task_render_scaled_depth32_image.get_state().images[0]).size;
    //         cmd_list.blit_image_to_image(daxa::ImageBlitInfo{
    //             .src_image = task_render_depth32_image.get_state().images[0],
    //             .dst_image = task_render_scaled_depth32_image.get_state().images[0],
    //             .src_offsets = {daxa::Offset3D{0, 0, 0}, daxa::Offset3D{static_cast<i32>(size0.x), static_cast<i32>(size0.y), 1}},
    //             .dst_offsets = {daxa::Offset3D{0, 0, 0}, daxa::Offset3D{static_cast<i32>(size1.x), static_cast<i32>(size1.y), 1}},
    //             .filter = daxa::Filter::NEAREST,
    //         });
    //     },
    //     .name = "Downscale depth",
    // });

    result_task_graph.add_task(DownscaleComputeTask{
        {
            .uses = {
                .gpu_input = task_input_buffer,
                .src_image_id = task_render_depth32_image,
                .dst_image_id = task_render_scaled_depth32_image,
            },
        },
        &downscale_task_state,
    });

    result_task_graph.add_task(SsaoComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .globals = task_globals_buffer,
                .blue_noise_vec2 = task_blue_noise_vec2_image,
                .g_buffer_image_id = task_render_g_buffer_image,
                .depth_image = task_render_scaled_depth32_image,
                .ssao_image_id = task_render_ssao_image,
            },
        },
        &ssao_task_state,
    });

    result_task_graph.add_task(TraceSecondaryComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .globals = task_globals_buffer,
                .voxel_malloc_page_allocator = gpu_resources.voxel_malloc.task_allocator_buffer,
                .voxel_chunks = task_voxel_chunks_buffer,
                .blue_noise_vec2 = task_blue_noise_vec2_image,
                .g_buffer_image_id = task_render_g_buffer_image,
                .depth_image = task_render_scaled_depth32_image,
                .indirect_diffuse_image_id = task_render_indirect_diffuse_image,
            },
        },
        &trace_secondary_task_state,
    });

    result_task_graph.add_task(UpscaleReconstructComputeTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .globals = task_globals_buffer,
                .depth_image_id = task_render_depth32_image,
                .ssao_image_id = task_render_ssao_image,
                .indirect_diffuse_image_id = task_render_indirect_diffuse_image,
                .src_image_id = task_render_prev_reconstructed_shading_image,
                .dst_image_id = task_render_reconstructed_shading_image,
            },
        },
        &upscale_reconstruct_task_state,
    });

    // GpuOutputDownloadTransferTask
    result_task_graph.add_task({
        .uses = {
            daxa::TaskBufferUse<daxa::TaskBufferAccess::TRANSFER_READ>{task_output_buffer},
            daxa::TaskBufferUse<daxa::TaskBufferAccess::HOST_TRANSFER_WRITE>{task_staging_output_buffer},
        },
        .task = [this](daxa::TaskInterface task_runtime) {
            auto cmd_list = task_runtime.get_command_list();
            auto output_buffer = task_output_buffer.get_state().buffers[0];
            auto staging_output_buffer = gpu_resources.staging_output_buffer;
            auto frame_index = gpu_input.frame_index + 1;
            auto *buffer_ptr = device.get_host_address_as<std::array<GpuOutput, (FRAMES_IN_FLIGHT + 1)>>(staging_output_buffer);
            u32 const offset = frame_index % (FRAMES_IN_FLIGHT + 1);
            gpu_output = (*buffer_ptr)[offset];
            cmd_list.copy_buffer_to_buffer({
                .src_buffer = output_buffer,
                .dst_buffer = staging_output_buffer,
                .size = sizeof(GpuOutput) * (FRAMES_IN_FLIGHT + 1),
            });
        },
        .name = "GpuOutputDownloadTransferTask",
    });

    // PostprocessingTask
    result_task_graph.add_task(PostprocessingRasterTask{
        {
            .uses = {
                .settings = task_settings_buffer,
                .gpu_input = task_input_buffer,
                .g_buffer_image_id = task_render_g_buffer_image,
                .ssao_image_id = task_render_ssao_image,
                .indirect_diffuse_image_id = task_render_indirect_diffuse_image,
                .reconstructed_shading_image_id = task_render_reconstructed_shading_image,
                .render_image = task_swapchain_image,
            },
        },
        &postprocessing_task_state,
    });

    // ImGui draw
    result_task_graph.add_task({
        .uses = {
            daxa::TaskImageUse<daxa::TaskImageAccess::COLOR_ATTACHMENT, daxa::ImageViewType::REGULAR_2D>{task_swapchain_image},
        },
        .task = [this](daxa::TaskInterface task_runtime) {
            auto cmd_list = task_runtime.get_command_list();
            imgui_renderer.record_commands(ImGui::GetDrawData(), cmd_list, swapchain_image, window_size.x, window_size.y);
        },
        .name = "ImGui draw",
    });

    result_task_graph.submit({});
    result_task_graph.present({});
    result_task_graph.complete({});

    return result_task_graph;
}
