#pragma once

#include <voxels/impl/voxel_malloc.inl>
#include <voxels/gvox_model.inl>
#include <voxels/brushes.inl>

#define VOXELS_ORIGINAL_IMPL

// 1364 daxa_u32's
// 10.65625 bytes per 8x8x8
struct TempVoxelChunkUniformity {
    daxa_u32 lod_x2[1024];
    daxa_u32 lod_x4[256];
    daxa_u32 lod_x8[64];
    daxa_u32 lod_x16[16];
    daxa_u32 lod_x32[4];
};

// 8 bytes per 8x8x8
struct PaletteHeader {
    daxa_u32 variant_n;
    VoxelMalloc_Pointer blob_ptr;
};

struct VoxelParentChunk {
    daxa_u32 is_uniform;
    daxa_u32 children[512];
    daxa_u32 is_ptr[16];
};
DAXA_DECL_BUFFER_PTR(VoxelParentChunk)

#define VOXEL_BRICK_SIZE 8

struct VoxelBrickBitmask {
    daxa_u32 data[VOXEL_BRICK_SIZE * VOXEL_BRICK_SIZE * VOXEL_BRICK_SIZE / 32];
};
struct VoxelBrickAttribs {
    PackedVoxel packed_voxels[VOXEL_BRICK_SIZE * VOXEL_BRICK_SIZE * VOXEL_BRICK_SIZE];
};
DAXA_DECL_BUFFER_PTR(VoxelBrickAttribs)
DAXA_DECL_BUFFER_PTR(daxa_BufferPtr(VoxelBrickAttribs))

struct VoxelLeafChunk {
    daxa_u32 flags;
    daxa_u32 update_index;
    daxa_u32 uniformity_bits[3];
    // 8 bytes per 8x8x8
    VoxelMalloc_ChunkLocalPageSubAllocatorState sub_allocator_state;
    // 8 bytes per 8x8x8
    PaletteHeader palette_headers[PALETTES_PER_CHUNK];
};
DAXA_DECL_BUFFER_PTR(VoxelLeafChunk)

// DECL_SIMPLE_ALLOCATOR(VoxelLeafChunkAllocator, VoxelLeafChunk, 1, daxa_u32, (MAX_CHUNK_WORK_ITEMS_L2))
// DECL_SIMPLE_ALLOCATOR(VoxelParentChunkAllocator, VoxelParentChunk, 1, daxa_u32, (MAX_CHUNK_WORK_ITEMS_L0 + MAX_CHUNK_WORK_ITEMS_L1))

struct TempVoxelChunk {
    PackedVoxel voxels[CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE];
    TempVoxelChunkUniformity uniformity;
};
DAXA_DECL_BUFFER_PTR(TempVoxelChunk)

struct VoxelChunkUpdateInfo {
    daxa_i32vec3 i;
    daxa_i32vec3 chunk_offset;
    daxa_u32 brush_flags;
    BrushInput brush_input;
};

struct VoxelWorldGpuIndirectDispatch {
    daxa_u32vec3 chunk_edit_dispatch;
    daxa_u32vec3 subchunk_x2x4_dispatch;
    daxa_u32vec3 subchunk_x8up_dispatch;
};

struct BrushState {
    daxa_u32 initial_frame;
    daxa_f32vec3 initial_ray;
    daxa_u32 is_editing;
};

struct VoxelWorldGlobals {
    BrushInput brush_input;
    BrushState brush_state;
    VoxelWorldGpuIndirectDispatch indirect_dispatch;

    VoxelChunkUpdateInfo chunk_update_infos[MAX_CHUNK_UPDATES_PER_FRAME];
    daxa_i32vec3 prev_offset;
    daxa_u32 chunk_update_n; // Number of chunks to update
    daxa_i32vec3 offset;
    daxa_u32 chunk_update_heap_alloc_n;
};
DAXA_DECL_BUFFER_PTR(VoxelWorldGlobals)

struct CpuChunkUpdateInfo {
    daxa_u32 chunk_index;
    daxa_u32 flags;
};
struct ChunkUpdate {
    CpuChunkUpdateInfo info;
    PaletteHeader palette_headers[PALETTES_PER_CHUNK];
};
DAXA_DECL_BUFFER_PTR(ChunkUpdate)

#define VOXEL_BUFFER_USE_N 3

#define VOXELS_USE_BUFFERS(ptr_type, mode)                               \
    DAXA_TH_BUFFER_PTR(mode, ptr_type(VoxelWorldGlobals), voxel_globals) \
    DAXA_TH_BUFFER_PTR(mode, ptr_type(VoxelLeafChunk), voxel_chunks)     \
    DAXA_TH_BUFFER_PTR(mode, ptr_type(VoxelMallocPageAllocator), voxel_malloc_page_allocator)

#define VOXELS_USE_BUFFERS_PUSH_USES(ptr_type)                           \
    ptr_type(VoxelWorldGlobals) voxel_globals = push.uses.voxel_globals; \
    ptr_type(VoxelLeafChunk) voxel_chunks = push.uses.voxel_chunks;      \
    ptr_type(VoxelMallocPageAllocator) voxel_malloc_page_allocator = push.uses.voxel_malloc_page_allocator;

#define VOXELS_BUFFER_USES_ASSIGN(TaskHeadName, voxel_buffers)                                                       \
    daxa::TaskViewVariant{std::pair{TaskHeadName::AT.voxel_globals, voxel_buffers.voxel_globals.task_resource}},     \
        daxa::TaskViewVariant{std::pair{TaskHeadName::AT.voxel_chunks, voxel_buffers.voxel_chunks.task_resource}},   \
        daxa::TaskViewVariant {                                                                                      \
        std::pair { TaskHeadName::AT.voxel_malloc_page_allocator, voxel_buffers.voxel_malloc.task_allocator_buffer } \
    }

struct VoxelWorldOutput {
    VoxelMallocPageAllocatorGpuOutput voxel_malloc_output;
    // VoxelLeafChunkAllocatorGpuOutput voxel_leaf_chunk_output;
    // VoxelParentChunkAllocatorGpuOutput voxel_parent_chunk_output;
};

struct VoxelBufferPtrs {
    daxa_BufferPtr(VoxelMallocPageAllocator) allocator;
    daxa_BufferPtr(VoxelLeafChunk) voxel_chunks_ptr;
    daxa_BufferPtr(VoxelWorldGlobals) globals;
};
struct VoxelRWBufferPtrs {
    daxa_RWBufferPtr(VoxelMallocPageAllocator) allocator;
    daxa_RWBufferPtr(VoxelLeafChunk) voxel_chunks_ptr;
    daxa_RWBufferPtr(VoxelWorldGlobals) globals;
};

#define VOXELS_BUFFER_PTRS VoxelBufferPtrs(daxa_BufferPtr(VoxelMallocPageAllocator)(as_address(voxel_malloc_page_allocator)), daxa_BufferPtr(VoxelLeafChunk)(as_address(voxel_chunks)), daxa_BufferPtr(VoxelWorldGlobals)(as_address(voxel_globals)))
#define VOXELS_RW_BUFFER_PTRS VoxelRWBufferPtrs(daxa_RWBufferPtr(VoxelMallocPageAllocator)(as_address(voxel_malloc_page_allocator)), daxa_RWBufferPtr(VoxelLeafChunk)(as_address(voxel_chunks)), daxa_RWBufferPtr(VoxelWorldGlobals)(as_address(voxel_globals)))

#define MAX_CHUNK_UPDATES_PER_FRAME_VOXEL_COUNT (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * MAX_CHUNK_UPDATES_PER_FRAME)

#if defined(__cplusplus)

#include <utilities/allocator.inl>

struct VoxelWorldBuffers {
    TemporalBuffer voxel_globals;
    TemporalBuffer voxel_chunks;
    TemporalBuffer chunk_updates;
    TemporalBuffer chunk_update_heap;
    AllocatorBufferState<VoxelMallocPageAllocator> voxel_malloc;

    TemporalBuffer blas_geom_pointers;
    TemporalBuffer blas_attr_pointers;
    TemporalBuffer blas_transforms;

    daxa::TlasId tlas;
    daxa::TaskTlas task_tlas;

    // AllocatorBufferState<VoxelLeafChunkAllocator> voxel_leaf_chunk_malloc;
    // AllocatorBufferState<VoxelParentChunkAllocator> voxel_parent_chunk_malloc;
};

#endif
