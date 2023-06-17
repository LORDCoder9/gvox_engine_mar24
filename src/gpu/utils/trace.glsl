#pragma once

#include <shared/shared.inl>

#include <utils/math.glsl>
#include <utils/voxels.glsl>

f32vec3 create_view_pos(in Player player) {
    return player.cam.pos;
}

f32vec3 create_view_dir(in Player player, f32vec2 uv) {
    f32vec3 nrm = normalize(f32vec3(uv.x * player.cam.tan_half_fov, uv.y * player.cam.tan_half_fov, 1));
    nrm = player.cam.rot_mat * nrm;
    return nrm;
}

f32 sdmap(f32vec3 p) {
    f32 value = MAX_SD;
    // value = sd_union(value, +sd_plane_x(p - f32vec3(-6.4, 0.0, 0.0)));
    // value = sd_union(value, -sd_plane_x(p - f32vec3(+6.4, 0.0, 0.0)));
    // value = sd_union(value, +sd_plane_y(p - f32vec3(0.0, -6.4, 0.0)));
    // value = sd_union(value, -sd_plane_y(p - f32vec3(0.0, +6.4, 0.0)));
    // value = sd_union(value, +sd_plane_z(p - f32vec3(0.0, 0.0, -6.4)));
    // value = sd_union(value, -sd_plane_z(p - f32vec3(0.0, 0.0, +6.4)));
    value = sd_union(value, sd_sphere(p - f32vec3(0.0, 2.0, 0.0), 0.5));
    return value;
}

f32vec3 sdmap_nrm(f32vec3 pos) {
    f32vec3 n = f32vec3(0.0);
    for (u32 i = 0; i < 4; ++i) {
        f32vec3 e = 0.5773 * (2.0 * f32vec3((((i + 3) >> 1) & 1), ((i >> 1) & 1), (i & 1)) - 1.0);
        n += e * sdmap(pos + 0.001 * e);
    }
    return normalize(n);
}

void trace_sphere_trace(in out f32vec3 ray_pos, f32vec3 ray_dir) {
    f32 t = 0.0;
    f32 final_dist = MAX_SD;
    for (u32 i = 0; i < 512 && t < MAX_SD; ++i) {
        f32vec3 p = ray_pos + ray_dir * t;
        f32 d = sdmap(p);
        if (abs(d) < (0.0001 * t)) {
            final_dist = t;
            break;
        }
        t += d;
    }
    ray_pos = ray_pos + ray_dir * t;
}

#if !defined(TRAVERSAL_MODE)
#define TRAVERSAL_MODE 2
#endif

#if TRAVERSAL_MODE == 3
#include <utils/hdda.glsl>
#endif

float trace_hierarchy_traversal(daxa_RWBufferPtr(VoxelMalloc_GlobalAllocator) allocator, daxa_BufferPtr(VoxelLeafChunk) voxel_chunks_ptr, u32vec3 chunk_n, in out f32vec3 ray_pos, f32vec3 ray_dir, u32 max_steps, float max_dist, bool extend_to_max_dist) {
    BoundingBox b;
    b.bound_min = f32vec3(0, 0, 0);
    b.bound_max = f32vec3(chunk_n) * (CHUNK_SIZE / VOXEL_SCL);

    intersect(ray_pos, ray_dir, f32vec3(1) / ray_dir, b);
    ray_pos += ray_dir * 0.01 / VOXEL_SCL;
    if (!inside(ray_pos, b)) {
        if (extend_to_max_dist) {
            return max_dist;
        } else {
            return 0.0;
        }
    }

#if TRAVERSAL_MODE == 1
    f32vec3 delta_dist = f32vec3(
        ray_dir.x == 0 ? 1 : abs(1.0 / ray_dir.x),
        ray_dir.y == 0 ? 1 : abs(1.0 / ray_dir.y),
        ray_dir.z == 0 ? 1 : abs(1.0 / ray_dir.z));
    f32vec3 unit_space_ray_pos = ray_pos * VOXEL_SCL;
    i32vec3 tile_i = i32vec3(unit_space_ray_pos);
    i32vec3 tile_steps_i = i32vec3(0);
    i32vec3 ray_step = i32vec3(sign(ray_dir));
    f32vec3 start_dist = ((unit_space_ray_pos - tile_i) * -sign(ray_dir) + step(0.0, ray_dir)) * delta_dist;
    f32vec3 to_side_dist = start_dist;
    f32 dist = MAX_SD;
    bvec3 mask = bvec3(
        to_side_dist.x < to_side_dist.y && to_side_dist.x < to_side_dist.z,
        to_side_dist.y <= to_side_dist.x && to_side_dist.y < to_side_dist.z,
        to_side_dist.z <= to_side_dist.x && to_side_dist.z <= to_side_dist.y);

    if (sample_lod(allocator, voxel_chunks_ptr, chunk_n, f32vec3(tile_i) / VOXEL_SCL) == 0) {
        return;
    }

    for (u32 total_steps = 0; total_steps < 50000; ++total_steps) {
        if (!inside(f32vec3(tile_i) / VOXEL_SCL, b)) {
            break;
        }
        to_side_dist = delta_dist * tile_steps_i + start_dist;
        if (sample_lod(allocator, voxel_chunks_ptr, chunk_n, f32vec3(tile_i) / VOXEL_SCL) == 0) {
            dist = dot(to_side_dist - delta_dist, f32vec3(mask)) / VOXEL_SCL;
            break;
        }
        mask = bvec3(
            to_side_dist.x < to_side_dist.y && to_side_dist.x < to_side_dist.z,
            to_side_dist.y <= to_side_dist.x && to_side_dist.y < to_side_dist.z,
            to_side_dist.z <= to_side_dist.x && to_side_dist.z <= to_side_dist.y);
        tile_steps_i += abs(ray_step) * i32vec3(mask);
        tile_i += ray_step * i32vec3(mask);
    }
    ray_pos = ray_pos + ray_dir * (dist + 0.0001);
#elif TRAVERSAL_MODE == 2
    f32vec3 delta = f32vec3(
        ray_dir.x == 0 ? 3.0 * max_steps : abs(1.0 / ray_dir.x),
        ray_dir.y == 0 ? 3.0 * max_steps : abs(1.0 / ray_dir.y),
        ray_dir.z == 0 ? 3.0 * max_steps : abs(1.0 / ray_dir.z));
    u32 lod = sample_lod(allocator, voxel_chunks_ptr, chunk_n, ray_pos);
    if (lod == 0) {
        if (extend_to_max_dist) {
            return max_dist;
        } else {
            return 0.0;
        }
    }
    f32 cell_size = f32(1l << (lod - 1)) / VOXEL_SCL;
    f32vec3 t_start;
    if (ray_dir.x < 0) {
        t_start.x = (ray_pos.x / cell_size - floor(ray_pos.x / cell_size)) * cell_size * delta.x;
    } else {
        t_start.x = (ceil(ray_pos.x / cell_size) - ray_pos.x / cell_size) * cell_size * delta.x;
    }
    if (ray_dir.y < 0) {
        t_start.y = (ray_pos.y / cell_size - floor(ray_pos.y / cell_size)) * cell_size * delta.y;
    } else {
        t_start.y = (ceil(ray_pos.y / cell_size) - ray_pos.y / cell_size) * cell_size * delta.y;
    }
    if (ray_dir.z < 0) {
        t_start.z = (ray_pos.z / cell_size - floor(ray_pos.z / cell_size)) * cell_size * delta.z;
    } else {
        t_start.z = (ceil(ray_pos.z / cell_size) - ray_pos.z / cell_size) * cell_size * delta.z;
    }
    f32 t_curr = min(min(t_start.x, t_start.y), t_start.z);
    f32vec3 current_pos = ray_pos;
    f32vec3 t_next = t_start;
    f32 dist = max_dist;
    for (u32 x1_steps = 0; x1_steps < max_steps; ++x1_steps) {
        current_pos = ray_pos + ray_dir * t_curr;
        if (!inside(current_pos + ray_dir * 0.001, b) || t_curr > max_dist) {
            break;
        }
        lod = sample_lod(allocator, voxel_chunks_ptr, chunk_n, current_pos);
        if (lod == 0) {
            dist = t_curr;
            break;
        }
        cell_size = f32(1l << (lod - 1)) / VOXEL_SCL;
        t_next = (0.5 + sign(ray_dir) * (0.5 - fract(current_pos / cell_size))) * cell_size * delta;
        // HACK to mitigate fp imprecision...
        t_next += 0.00001 * (sign(ray_dir) * -0.5 + 0.5);
        // t_curr += (min(min(t_next.x, t_next.y), t_next.z));
        t_curr += (min(min(t_next.x, t_next.y), t_next.z) + 0.0001 / VOXEL_SCL);
    }
    if (extend_to_max_dist) {
        ray_pos = ray_pos + ray_dir * dist;
    } else {
        ray_pos = ray_pos + ray_dir * t_curr;
    }

    return t_curr;
#elif TRAVERSAL_MODE == 3
    const f32 DELTA = 0.0001;
    f32vec3 ray_orig = ray_pos;
    ivec3 ijk = hdda_round_down(ray_orig);
    u32 lod = sample_lod(allocator, voxel_chunks_ptr, chunk_n, ray_orig);
    HDDA hdda;
    hdda_init(hdda, ray_orig, ray_dir, 1 << lod);
    f32 dist = MAX_SD;

    while (true) {
        if (!inside(f32vec3(ijk), b)) {
            break;
        }

        lod = sample_lod(allocator, voxel_chunks_ptr, chunk_n, f32vec3(ijk));
        if (lod == 0) {
            dist = hdda.t0;
            break;
        }

        hdda_update(hdda, ray_pos, ray_dir, 1 << lod);

        if (!hdda_step(hdda)) {
            break;
        }

        ijk = hdda_round_down(ray_pos);
        ray_pos = fma(vec3(hdda.t0 + DELTA), ray_dir, ray_orig);
    }

    ray_pos = f32vec3(ijk);
#endif
}

void trace_sparse(daxa_BufferPtr(VoxelLeafChunk) voxel_chunks_ptr, u32vec3 chunk_n, in out f32vec3 ray_pos, f32vec3 ray_dir, u32 max_steps) {
    BoundingBox b;
    b.bound_min = f32vec3(0, 0, 0);
    b.bound_max = f32vec3(chunk_n) * (CHUNK_SIZE / VOXEL_SCL);

    intersect(ray_pos, ray_dir, f32vec3(1) / ray_dir, b);
    ray_pos += ray_dir * 0.001 / VOXEL_SCL;
    if (!inside(ray_pos, b)) {
        ray_pos = ray_pos + ray_dir * MAX_SD;
        return;
    }

#define STEP(N)                                                                                        \
    for (u32 i = 0; i < max_steps; ++i) {                                                              \
        ray_pos += ray_dir * N / 8;                                                                    \
        if (!inside(f32vec3(ray_pos), b)) {                                                            \
            ray_pos += ray_dir * MAX_SD;                                                               \
            return;                                                                                    \
        }                                                                                              \
        b32 present = sample_lod_presence(N)(voxel_chunks_ptr, chunk_n, u32vec3(ray_pos * VOXEL_SCL)); \
        if (present) {                                                                                 \
            return;                                                                                    \
        }                                                                                              \
    }
    STEP(2)
    STEP(4)
    STEP(8)
    STEP(16)
    STEP(32)
    STEP(64)
    ray_pos += ray_dir * MAX_SD;
}

void trace(daxa_RWBufferPtr(VoxelMalloc_GlobalAllocator) allocator, daxa_BufferPtr(VoxelLeafChunk) voxel_chunks_ptr, u32vec3 chunk_n, in out f32vec3 ray_pos, f32vec3 ray_dir) {
    // trace_sphere_trace(ray_pos, ray_dir);
    trace_hierarchy_traversal(allocator, voxel_chunks_ptr, chunk_n, ray_pos, ray_dir, 512, MAX_SD, true);
}

f32vec3 scene_nrm(daxa_RWBufferPtr(VoxelMalloc_GlobalAllocator) allocator, daxa_BufferPtr(VoxelLeafChunk) voxel_chunks_ptr, u32vec3 chunk_n, f32vec3 pos) {
#if 0
    const i32 RANGE = 1;
    f32vec3 result = f32vec3(0);
    for (i32 zi = -RANGE; zi <= RANGE; ++zi) {
        for (i32 yi = -RANGE; yi <= RANGE; ++yi) {
            for (i32 xi = -RANGE; xi <= RANGE; ++xi) {
                u32 voxel = sample_voxel_chunk(allocator, voxel_chunks_ptr, chunk_n, u32vec3(pos * VOXEL_SCL + f32vec3(xi, yi, zi)), true);
                u32 id = voxel >> 0x18;
                if (voxel != 0) {
                    result += f32vec3(xi, yi, zi);
                }
            }
        }
    }
    return -normalize(result);
#else
    // return -sdmap_nrm(pos);
    f32vec3 d = fract(pos * VOXEL_SCL) - .5;
    f32vec3 ad = abs(d);
    f32 m = max(max(ad.x, ad.y), ad.z);
    return -(abs(sign(ad - m)) - 1.) * sign(d);
#endif
}
