#include <renderer/kajiya/ircache.inl>
#include <utils/sh.glsl>
#include "ircache_constants.glsl"

DAXA_DECL_PUSH_CONSTANT(IrcacheResetComputePush, push)
daxa_BufferPtr(daxa_u32) ircache_life_buf = push.uses.ircache_life_buf;
daxa_RWBufferPtr(IrcacheMetadata) ircache_meta_buf = push.uses.ircache_meta_buf;
daxa_BufferPtr(daxa_f32vec4) ircache_irradiance_buf = push.uses.ircache_irradiance_buf;
daxa_RWBufferPtr(daxa_f32vec4) ircache_aux_buf = push.uses.ircache_aux_buf;
daxa_BufferPtr(daxa_u32) ircache_entry_indirection_buf = push.uses.ircache_entry_indirection_buf;

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
void main() {
    uint dispatch_idx = gl_GlobalInvocationID.x;
    if (IRCACHE_FREEZE) {
        return;
    }

    const uint total_alloc_count = deref(ircache_meta_buf).tracing_alloc_count;
    if (dispatch_idx >= total_alloc_count) {
        return;
    }

    const uint entry_idx = deref(ircache_entry_indirection_buf[dispatch_idx]);

    const bool should_reset = all(equal(vec4(0.0), deref(ircache_irradiance_buf[entry_idx * IRCACHE_IRRADIANCE_STRIDE])));

    if (should_reset) {
        for (uint i = 0; i < IRCACHE_AUX_STRIDE; ++i) {
            deref(ircache_aux_buf[entry_idx * IRCACHE_AUX_STRIDE + i]) = 0.0.xxxx;
        }
    }
}