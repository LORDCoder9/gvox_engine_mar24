#include <shared/shared.inl>

DAXA_USE_PUSH_CONSTANT(ChunkEditCompPush)

#include <utils/impl_brush_header.glsl>

void chunk_edit(in u32 chunk_index, in u32vec3 voxel_i) {
    u32 voxel_index = voxel_i.x + voxel_i.y * CHUNK_SIZE + voxel_i.z * CHUNK_SIZE * CHUNK_SIZE;

    f32vec3 voxel_p = f32vec3(voxel_i) / VOXEL_SCL + VOXEL_WORLD.voxel_chunks[chunk_index].box.bound_min - VOXEL_BRUSH.box.bound_min;

    PackedVoxel result = sample_brush_packed_voxel(voxel_p);
    Voxel voxel = unpack_voxel(result);
    if (voxel.block_id != BlockID_Debug)
        VOXEL_WORLD.voxel_chunks[chunk_index].packed_voxels[voxel_index] = result;
}

layout(local_size_x = 8, local_size_y = 8, local_size_z = 8) in;
void main() {
    u32 workgroup_index = gl_WorkGroupID.z / 8;
    u32 chunk_index = VOXEL_WORLD.chunk_update_indices[workgroup_index];

    if (VOXEL_WORLD.chunks_genstate[chunk_index].edit_stage != 3)
        return;

    u32vec3 voxel_i = gl_GlobalInvocationID.xyz - u32vec3(0, 0, workgroup_index * 64);
    chunk_edit(chunk_index, voxel_i);
}
