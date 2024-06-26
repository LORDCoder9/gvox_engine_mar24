#include "sky.inl"

#include <renderer/atmosphere/sky_utils.glsl>
#include <renderer/atmosphere/atmosphere.glsl>
#include <g_samplers>

#if SkyTransmittanceComputeShader

DAXA_DECL_PUSH_CONSTANT(SkyTransmittanceComputePush, push)
daxa_BufferPtr(GpuInput) gpu_input = push.uses.gpu_input;
daxa_ImageViewIndex transmittance_lut = push.uses.transmittance_lut;

layout(local_size_x = 8, local_size_y = 4, local_size_z = 1) in;
vec3 integrate_transmittance(vec3 world_position, vec3 world_direction, uint sample_count) {
    /* The length of ray between position and nearest atmosphere top boundary */
    float integration_length = ray_sphere_intersect_nearest(
        world_position,
        world_direction,
        vec3(0.0, 0.0, 0.0),
        deref(gpu_input).sky_settings.atmosphere_top);

    float integration_step = integration_length / float(sample_count);

    /* Result of the integration */
    vec3 optical_depth = vec3(0.0, 0.0, 0.0);

    for (int i = 0; i < sample_count; i++) {
        /* Move along the world direction ray to new position */
        vec3 new_pos = world_position + i * integration_step * world_direction;
        vec3 atmosphere_extinction = sample_medium_extinction(gpu_input, new_pos);
        optical_depth += atmosphere_extinction * integration_step;
    }
    return optical_depth;
}

void main() {
    if (any(greaterThan(gl_GlobalInvocationID.xy, SKY_TRANSMITTANCE_RES))) {
        return;
    }

    vec2 uv = vec2(gl_GlobalInvocationID.xy) / vec2(SKY_TRANSMITTANCE_RES);

    TransmittanceParams mapping = uv_to_transmittance_lut_params(
        uv,
        deref(gpu_input).sky_settings.atmosphere_bottom,
        deref(gpu_input).sky_settings.atmosphere_top);

    vec3 world_position = vec3(0.0, 0.0, mapping.height);
    vec3 world_direction = vec3(
        safe_sqrt(1.0 - mapping.zenith_cos_angle * mapping.zenith_cos_angle),
        0.0,
        mapping.zenith_cos_angle);

    vec3 transmittance = exp(-integrate_transmittance(world_position, world_direction, 400));

    imageStore(daxa_image2D(transmittance_lut), ivec2(gl_GlobalInvocationID.xy), vec4(transmittance, 1.0));
}

#endif

#if SkyMultiscatteringComputeShader

DAXA_DECL_PUSH_CONSTANT(SkyMultiscatteringComputePush, push)
daxa_BufferPtr(GpuInput) gpu_input = push.uses.gpu_input;
daxa_ImageViewIndex transmittance_lut = push.uses.transmittance_lut;
daxa_ImageViewIndex multiscattering_lut = push.uses.multiscattering_lut;

layout(local_size_x = 1, local_size_y = 1, local_size_z = 64) in;
/* This number should match the number of local threads -> z dimension */
const float SPHERE_SAMPLES = 64.0;
const float GOLDEN_RATIO = 1.6180339;
const float uniformPhase = 1.0 / (4.0 * M_PI);

shared vec3 multiscatt_shared[64];
shared vec3 luminance_shared[64];

struct RaymarchResult {
    vec3 luminance;
    vec3 multiscattering;
};

RaymarchResult integrate_scattered_luminance(vec3 world_position, vec3 world_direction, vec3 sun_direction, float sample_count) {
    RaymarchResult result = RaymarchResult(vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, 0.0));
    vec3 planet_zero = vec3(0.0, 0.0, 0.0);
    float planet_intersection_distance = ray_sphere_intersect_nearest(
        world_position, world_direction, planet_zero, deref(gpu_input).sky_settings.atmosphere_bottom);
    float atmosphere_intersection_distance = ray_sphere_intersect_nearest(
        world_position, world_direction, planet_zero, deref(gpu_input).sky_settings.atmosphere_top);

    float integration_length;
    /* ============================= CALCULATE INTERSECTIONS ============================ */
    if ((planet_intersection_distance == -1.0) && (atmosphere_intersection_distance == -1.0)) {
        /* ray does not intersect planet or atmosphere -> no point in raymarching*/
        return result;
    } else if ((planet_intersection_distance == -1.0) && (atmosphere_intersection_distance > 0.0)) {
        /* ray intersects only atmosphere */
        integration_length = atmosphere_intersection_distance;
    } else if ((planet_intersection_distance > 0.0) && (atmosphere_intersection_distance == -1.0)) {
        /* ray intersects only planet */
        integration_length = planet_intersection_distance;
    } else {
        /* ray intersects both planet and atmosphere -> return the first intersection */
        integration_length = min(planet_intersection_distance, atmosphere_intersection_distance);
    }
    float integration_step = integration_length / float(sample_count);

    /* stores accumulated transmittance during the raymarch process */
    vec3 accum_transmittance = vec3(1.0, 1.0, 1.0);
    /* stores accumulated light contribution during the raymarch process */
    vec3 accum_light = vec3(0.0, 0.0, 0.0);
    float old_ray_shift = 0;

    /* ============================= RAYMARCH ==========================================  */
    for (int i = 0; i < sample_count; i++) {
        /* Sampling at 1/3rd of the integration step gives better results for exponential
           functions */
        float new_ray_shift = integration_length * (float(i) + 0.3) / sample_count;
        integration_step = new_ray_shift - old_ray_shift;
        vec3 new_position = world_position + new_ray_shift * world_direction;
        old_ray_shift = new_ray_shift;

        /* Raymarch shifts the angle to the sun a bit recalculate */
        vec3 up_vector = normalize(new_position);
        TransmittanceParams transmittance_lut_params = TransmittanceParams(length(new_position), dot(sun_direction, up_vector));

        /* uv coordinates later used to sample transmittance texture */
        vec2 trans_texture_uv = transmittance_lut_to_uv(transmittance_lut_params, deref(gpu_input).sky_settings.atmosphere_bottom, deref(gpu_input).sky_settings.atmosphere_top);

        vec3 transmittance_to_sun = texture(daxa_sampler2D(transmittance_lut, g_sampler_llc), trans_texture_uv).rgb;

        vec3 medium_scattering = sample_medium_scattering(gpu_input, new_position);
        vec3 medium_extinction = sample_medium_extinction(gpu_input, new_position);

        /* TODO: This probably should be a texture lookup altho might be slow*/
        vec3 trans_increase_over_integration_step = exp(-(medium_extinction * integration_step));
        /* Check if current position is in earth's shadow */
        float earth_intersection_distance = ray_sphere_intersect_nearest(
            new_position, sun_direction, planet_zero + PLANET_RADIUS_OFFSET * up_vector, deref(gpu_input).sky_settings.atmosphere_bottom);
        float in_earth_shadow = earth_intersection_distance == -1.0 ? 1.0 : 0.0;

        /* Light arriving from the sun to this point */
        vec3 sunLight = in_earth_shadow * transmittance_to_sun * medium_scattering * uniformPhase;
        vec3 multiscattered_cont_int = (medium_scattering - medium_scattering * trans_increase_over_integration_step) / medium_extinction;
        vec3 inscatteredContInt = (sunLight - sunLight * trans_increase_over_integration_step) / medium_extinction;

        if (medium_extinction.r == 0.0) {
            multiscattered_cont_int.r = 0.0;
            inscatteredContInt.r = 0.0;
        }
        if (medium_extinction.g == 0.0) {
            multiscattered_cont_int.g = 0.0;
            inscatteredContInt.g = 0.0;
        }
        if (medium_extinction.b == 0.0) {
            multiscattered_cont_int.b = 0.0;
            inscatteredContInt.b = 0.0;
        }

        result.multiscattering += accum_transmittance * multiscattered_cont_int;
        accum_light += accum_transmittance * inscatteredContInt;
        // accum_light = accum_transmittance;
        accum_transmittance *= trans_increase_over_integration_step;
    }
    result.luminance = accum_light;
    return result;
    /* TODO: Check for bounced light off the earth */
}

void main() {
    const float sample_count = 20;

    vec2 uv = (vec2(gl_GlobalInvocationID.xy) + vec2(0.5, 0.5)) /
              SKY_MULTISCATTERING_RES;
    uv = vec2(from_subuv_to_unit(uv.x, SKY_MULTISCATTERING_RES.x),
              from_subuv_to_unit(uv.y, SKY_MULTISCATTERING_RES.y));

    /* Mapping uv to multiscattering LUT parameters
       TODO -> Is the range from 0.0 to -1.0 really needed? */
    float sun_cos_zenith_angle = uv.x * 2.0 - 1.0;
    vec3 sun_direction = vec3(
        0.0,
        safe_sqrt(clamp(1.0 - sun_cos_zenith_angle * sun_cos_zenith_angle, 0.0, 1.0)),
        sun_cos_zenith_angle);

    float view_height = deref(gpu_input).sky_settings.atmosphere_bottom +
                        clamp(uv.y + PLANET_RADIUS_OFFSET, 0.0, 1.0) *
                            (deref(gpu_input).sky_settings.atmosphere_top - deref(gpu_input).sky_settings.atmosphere_bottom - PLANET_RADIUS_OFFSET);

    vec3 world_position = vec3(0.0, 0.0, view_height);

    float sample_idx = gl_LocalInvocationID.z;
    // local thread dependent raymarch
    {
#define USE_HILL_SAMPLING 0
#if USE_HILL_SAMPLING
#define SQRTSAMPLECOUNT 8
        const float sqrt_sample = float(SQRTSAMPLECOUNT);
        float i = 0.5 + float(sample_idx / SQRTSAMPLECOUNT);
        float j = 0.5 + mod(sample_idx, SQRTSAMPLECOUNT);
        float randA = i / sqrt_sample;
        float randB = j / sqrt_sample;

        float theta = 2.0 * M_PI * randA;
        float phi = M_PI * randB;
#else
        /* Fibbonaci lattice -> http://extremelearning.com.au/how-to-evenly-distribute-points-on-a-sphere-more-effectively-than-the-canonical-fibonacci-lattice/ */
        float theta = acos(1.0 - 2.0 * (sample_idx + 0.5) / SPHERE_SAMPLES);
        float phi = (2 * M_PI * sample_idx) / GOLDEN_RATIO;
#endif

        vec3 world_direction = vec3(cos(theta) * sin(phi), sin(theta) * sin(phi), cos(phi));
        RaymarchResult result = integrate_scattered_luminance(world_position, world_direction, sun_direction, sample_count);

        multiscatt_shared[gl_LocalInvocationID.z] = result.multiscattering / SPHERE_SAMPLES;
        luminance_shared[gl_LocalInvocationID.z] = result.luminance / SPHERE_SAMPLES;
    }

    groupMemoryBarrier();
    barrier();

    if (gl_LocalInvocationID.z < 32) {
        multiscatt_shared[gl_LocalInvocationID.z] += multiscatt_shared[gl_LocalInvocationID.z + 32];
        luminance_shared[gl_LocalInvocationID.z] += luminance_shared[gl_LocalInvocationID.z + 32];
    }
    groupMemoryBarrier();
    barrier();
    if (gl_LocalInvocationID.z < 16) {
        multiscatt_shared[gl_LocalInvocationID.z] += multiscatt_shared[gl_LocalInvocationID.z + 16];
        luminance_shared[gl_LocalInvocationID.z] += luminance_shared[gl_LocalInvocationID.z + 16];
    }
    groupMemoryBarrier();
    barrier();
    if (gl_LocalInvocationID.z < 8) {
        multiscatt_shared[gl_LocalInvocationID.z] += multiscatt_shared[gl_LocalInvocationID.z + 8];
        luminance_shared[gl_LocalInvocationID.z] += luminance_shared[gl_LocalInvocationID.z + 8];
    }
    groupMemoryBarrier();
    barrier();
    if (gl_LocalInvocationID.z < 4) {
        multiscatt_shared[gl_LocalInvocationID.z] += multiscatt_shared[gl_LocalInvocationID.z + 4];
        luminance_shared[gl_LocalInvocationID.z] += luminance_shared[gl_LocalInvocationID.z + 4];
    }
    groupMemoryBarrier();
    barrier();
    if (gl_LocalInvocationID.z < 2) {
        multiscatt_shared[gl_LocalInvocationID.z] += multiscatt_shared[gl_LocalInvocationID.z + 2];
        luminance_shared[gl_LocalInvocationID.z] += luminance_shared[gl_LocalInvocationID.z + 2];
    }
    groupMemoryBarrier();
    barrier();
    if (gl_LocalInvocationID.z < 1) {
        multiscatt_shared[gl_LocalInvocationID.z] += multiscatt_shared[gl_LocalInvocationID.z + 1];
        luminance_shared[gl_LocalInvocationID.z] += luminance_shared[gl_LocalInvocationID.z + 1];
    }
    groupMemoryBarrier();
    barrier();
    if (gl_LocalInvocationID.z != 0)
        return;

    vec3 multiscatt_sum = multiscatt_shared[0];
    vec3 inscattered_luminance_sum = luminance_shared[0];

    const vec3 r = multiscatt_sum;
    const vec3 sum_of_all_multiscattering_events_contribution = vec3(1.0 / (1.0 - r.x), 1.0 / (1.0 - r.y), 1.0 / (1.0 - r.z));
    vec3 lum = inscattered_luminance_sum * sum_of_all_multiscattering_events_contribution;

    imageStore(daxa_image2D(multiscattering_lut), ivec2(gl_GlobalInvocationID.xy), vec4(lum, 1.0));
}

#endif

#if SkySkyComputeShader

DAXA_DECL_PUSH_CONSTANT(SkySkyComputePush, push)
daxa_BufferPtr(GpuInput) gpu_input = push.uses.gpu_input;
daxa_ImageViewIndex transmittance_lut = push.uses.transmittance_lut;
daxa_ImageViewIndex multiscattering_lut = push.uses.multiscattering_lut;
daxa_ImageViewIndex sky_lut = push.uses.sky_lut;

layout(local_size_x = 8, local_size_y = 4, local_size_z = 1) in;
/* ============================= PHASE FUNCTIONS ============================ */
float cornette_shanks_mie_phase_function(float g, float cos_theta) {
    float k = 3.0 / (8.0 * M_PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cos_theta * cos_theta) / pow(1.0 + g * g - 2.0 * g * -cos_theta, 1.5);
}

float kleinNishinaPhase(float cosTheta, float e) {
    return e / (2.0 * M_PI * (e * (1.0 - cosTheta) + 1.0) * log(2.0 * e + 1.0));
}

float rayleigh_phase(float cos_theta) {
    float factor = 3.0 / (16.0 * M_PI);
    return factor * (1.0 + cos_theta * cos_theta);
}
/* ========================================================================== */

vec3 get_multiple_scattering(vec3 world_position, float view_zenith_cos_angle) {
    vec2 uv = clamp(vec2(
                        view_zenith_cos_angle * 0.5 + 0.5,
                        (length(world_position) - deref(gpu_input).sky_settings.atmosphere_bottom) /
                            (deref(gpu_input).sky_settings.atmosphere_top - deref(gpu_input).sky_settings.atmosphere_bottom)),
                    0.0, 1.0);
    uv = vec2(from_unit_to_subuv(uv.x, SKY_MULTISCATTERING_RES.x),
              from_unit_to_subuv(uv.y, SKY_MULTISCATTERING_RES.y));

    return texture(daxa_sampler2D(multiscattering_lut, g_sampler_llc), uv).rgb;
}

vec3 integrate_scattered_luminance(vec3 world_position,
                                   vec3 world_direction, vec3 sun_direction, int sample_count) {
    vec3 planet_zero = vec3(0.0, 0.0, 0.0);
    float planet_intersection_distance = ray_sphere_intersect_nearest(
        world_position, world_direction, planet_zero, deref(gpu_input).sky_settings.atmosphere_bottom);
    float atmosphere_intersection_distance = ray_sphere_intersect_nearest(
        world_position, world_direction, planet_zero, deref(gpu_input).sky_settings.atmosphere_top);

    float integration_length;
    /* ============================= CALCULATE INTERSECTIONS ============================ */
    if ((planet_intersection_distance == -1.0) && (atmosphere_intersection_distance == -1.0)) {
        /* ray does not intersect planet or atmosphere -> no point in raymarching*/
        return vec3(0.0, 0.0, 0.0);
    } else if ((planet_intersection_distance == -1.0) && (atmosphere_intersection_distance > 0.0)) {
        /* ray intersects only atmosphere */
        integration_length = atmosphere_intersection_distance;
    } else if ((planet_intersection_distance > 0.0) && (atmosphere_intersection_distance == -1.0)) {
        /* ray intersects only planet */
        integration_length = planet_intersection_distance;
    } else {
        /* ray intersects both planet and atmosphere -> return the first intersection */
        integration_length = min(planet_intersection_distance, atmosphere_intersection_distance);
    }

    float cos_theta = dot(sun_direction, world_direction);
    float mie_phase_value = kleinNishinaPhase(cos_theta, 2800.0);
    float rayleigh_phase_value = rayleigh_phase(cos_theta);

    vec3 accum_transmittance = vec3(1.0, 1.0, 1.0);
    vec3 accum_light = vec3(0.0, 0.0, 0.0);
    /* ============================= RAYMARCH ============================ */
    for (int i = 0; i < sample_count; i++) {
        /* Step size computation */
        float step_0 = float(i) / sample_count;
        float step_1 = float(i + 1) / sample_count;

        /* Nonuniform step size*/
        step_0 *= step_0;
        step_1 *= step_1;

        step_0 = step_0 * integration_length;
        step_1 = step_1 > 1.0 ? integration_length : step_1 * integration_length;
        /* Sample at one third of the integrated interval -> better results for exponential functions */
        float integration_step = step_0 + (step_1 - step_0) * 0.3;
        float d_int_step = step_1 - step_0;

        /* Position shift */
        vec3 new_position = world_position + integration_step * world_direction;
        ScatteringSample medium_scattering = sample_medium_scattering_detailed(gpu_input, new_position);
        vec3 medium_extinction = sample_medium_extinction(gpu_input, new_position);

        vec3 up_vector = normalize(new_position);
        TransmittanceParams transmittance_lut_params = TransmittanceParams(length(new_position), dot(sun_direction, up_vector));

        /* uv coordinates later used to sample transmittance texture */
        vec2 trans_texture_uv = transmittance_lut_to_uv(transmittance_lut_params, deref(gpu_input).sky_settings.atmosphere_bottom, deref(gpu_input).sky_settings.atmosphere_top);
        vec3 transmittance_to_sun = texture(daxa_sampler2D(transmittance_lut, g_sampler_llc), trans_texture_uv).rgb;

        vec3 phase_times_scattering = medium_scattering.mie * mie_phase_value + medium_scattering.ray * rayleigh_phase_value;

        float earth_intersection_distance = ray_sphere_intersect_nearest(
            new_position, sun_direction, planet_zero, deref(gpu_input).sky_settings.atmosphere_bottom);
        float in_earth_shadow = earth_intersection_distance == -1.0 ? 1.0 : 0.0;

        vec3 multiscattered_luminance = get_multiple_scattering(new_position, dot(sun_direction, up_vector));

        /* Light arriving from the sun to this point */
        vec3 sun_light = in_earth_shadow * transmittance_to_sun * phase_times_scattering +
                         multiscattered_luminance * (medium_scattering.ray + medium_scattering.mie);

        /* TODO: This probably should be a texture lookup*/
        vec3 trans_increase_over_integration_step = exp(-(medium_extinction * d_int_step));

        vec3 sun_light_integ = (sun_light - sun_light * trans_increase_over_integration_step) / medium_extinction;

        if (medium_extinction.r == 0.0) {
            sun_light_integ.r = 0.0;
        }
        if (medium_extinction.g == 0.0) {
            sun_light_integ.g = 0.0;
        }
        if (medium_extinction.b == 0.0) {
            sun_light_integ.b = 0.0;
        }

        accum_light += accum_transmittance * sun_light_integ;
        accum_transmittance *= trans_increase_over_integration_step;
    }
    return accum_light;
}

void main() {
    if (any(greaterThan(gl_GlobalInvocationID.xy, SKY_SKY_RES))) {
        return;
    }

    vec3 camera_ws = sky_space_camera_position(gpu_input);
    float camera_height = length(camera_ws);

    bool inside_atmosphere = camera_height < deref(gpu_input).sky_settings.atmosphere_top;

    vec2 uv = vec2(gl_GlobalInvocationID.xy) / vec2(SKY_SKY_RES);
    SkyviewParams skyview_params = uv_to_skyview_lut_params(
        inside_atmosphere,
        uv,
        deref(gpu_input).sky_settings.atmosphere_bottom,
        deref(gpu_input).sky_settings.atmosphere_top,
        SKY_SKY_RES,
        camera_height);

    vec3 ray_direction = vec3(
        cos(skyview_params.light_view_angle) * sin(skyview_params.view_zenith_angle),
        sin(skyview_params.light_view_angle) * sin(skyview_params.view_zenith_angle),
        cos(skyview_params.view_zenith_angle));

    const mat3 camera_basis = build_orthonormal_basis(camera_ws / camera_height);
    camera_ws = vec3(0, 0, camera_height);

    float sun_zenith_cos_angle = dot(vec3(0, 0, 1), deref(gpu_input).sky_settings.sun_direction * camera_basis);
    // sin^2 + cos^2 = 1 -> sqrt(1 - cos^2) = sin
    // rotate the sun direction so that we are aligned with the y = 0 axis
    vec3 local_sun_direction = normalize(vec3(
        safe_sqrt(1.0 - sun_zenith_cos_angle * sun_zenith_cos_angle),
        0.0,
        sun_zenith_cos_angle));

    if (!move_to_top_atmosphere(camera_ws, ray_direction, deref(gpu_input).sky_settings.atmosphere_bottom, deref(gpu_input).sky_settings.atmosphere_top)) {
        /* No intersection with the atmosphere */
        imageStore(daxa_image2D(sky_lut), ivec2(gl_GlobalInvocationID.xy), vec4(0.0, 0.0, 0.0, 1.0));
        return;
    }
    vec3 luminance = integrate_scattered_luminance(camera_ws, ray_direction, local_sun_direction, 30);
    imageStore(daxa_image2D(sky_lut), ivec2(gl_GlobalInvocationID.xy), atmosphere_pack(luminance));

    // imageStore(daxa_image2D(sky_lut), ivec2(gl_GlobalInvocationID.xy), vec4(vec3(sun_zenith_cos_angle), 1.0));
}

#endif

#if SkyAeComputeShader

DAXA_DECL_PUSH_CONSTANT(SkyAeComputePush, push)
daxa_BufferPtr(GpuInput) gpu_input = push.uses.gpu_input;
daxa_ImageViewIndex transmittance_lut = push.uses.transmittance_lut;
daxa_ImageViewIndex multiscattering_lut = push.uses.multiscattering_lut;
daxa_ImageViewIndex aerial_perspective_lut = push.uses.aerial_perspective_lut;

// Terms:
// _ws is "world" space, which is more like the render-space, as world-space is meant to be global
// _ks I'm calling sky space, which is an offset version of world-space
// _fs is froxel space, which is a non-linear z-scaled clip-space

// returns the center of the froxel in xy, and the nearest z
vec3 sky_froxel_get_uvw(uvec3 px) {
    return (vec3(px) + 0.5) / vec3(SKY_AE_RES);
}

/* ============================= PHASE FUNCTIONS ============================ */
float cornette_shanks_mie_phase_function(float g, float cos_theta) {
    float k = 3.0 / (8.0 * M_PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cos_theta * cos_theta) / pow(1.0 + g * g - 2.0 * g * -cos_theta, 1.5);
}

float kleinNishinaPhase(float cosTheta, float e) {
    return e / (2.0 * M_PI * (e * (1.0 - cosTheta) + 1.0) * log(2.0 * e + 1.0));
}

float rayleigh_phase(float cos_theta) {
    float factor = 3.0 / (16.0 * M_PI);
    return factor * (1.0 + cos_theta * cos_theta);
}
/* ========================================================================== */

vec3 get_multiple_scattering(vec3 world_position, float view_zenith_cos_angle) {
    vec2 uv = clamp(vec2(
                        view_zenith_cos_angle * 0.5 + 0.5,
                        (length(world_position) - deref(gpu_input).sky_settings.atmosphere_bottom) /
                            (deref(gpu_input).sky_settings.atmosphere_top - deref(gpu_input).sky_settings.atmosphere_bottom)),
                    0.0, 1.0);
    uv = vec2(from_unit_to_subuv(uv.x, SKY_MULTISCATTERING_RES.x),
              from_unit_to_subuv(uv.y, SKY_MULTISCATTERING_RES.y));

    return texture(daxa_sampler2D(multiscattering_lut, g_sampler_llc), uv).rgb;
}

struct RaymarchResult {
    vec3 luminance;
    vec3 transmittance;
};

RaymarchResult integrate_scattered_luminance(
    vec3 world_position, vec3 world_direction,
    vec3 sun_direction, int sample_count, float max_dist) {
    RaymarchResult result = RaymarchResult(vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, 0.0));

    vec3 planet_zero = vec3(0.0, 0.0, 0.0);
    float planet_intersection_distance = ray_sphere_intersect_nearest(
        world_position, world_direction, planet_zero, deref(gpu_input).sky_settings.atmosphere_bottom);
    float atmosphere_intersection_distance = ray_sphere_intersect_nearest(
        world_position, world_direction, planet_zero, deref(gpu_input).sky_settings.atmosphere_top);

    float integration_length;
    /* ============================= CALCULATE INTERSECTIONS ============================ */
    if ((planet_intersection_distance == -1.0) && (atmosphere_intersection_distance == -1.0)) {
        /* ray does not intersect planet or atmosphere -> no point in raymarching*/
        return result;
    } else if ((planet_intersection_distance == -1.0) && (atmosphere_intersection_distance > 0.0)) {
        /* ray intersects only atmosphere */
        integration_length = atmosphere_intersection_distance;
    } else if ((planet_intersection_distance > 0.0) && (atmosphere_intersection_distance == -1.0)) {
        /* ray intersects only planet */
        integration_length = planet_intersection_distance;
    } else {
        /* ray intersects both planet and atmosphere -> return the first intersection */
        integration_length = min(planet_intersection_distance, atmosphere_intersection_distance);
    }
    integration_length = min(integration_length, max_dist);

    float cos_theta = dot(sun_direction, world_direction);
    float mie_phase_value = kleinNishinaPhase(cos_theta, 2800.0);
    float rayleigh_phase_value = rayleigh_phase(cos_theta);
    float old_ray_shift = 0.0;
    float integration_step = 0.0;

    vec3 accum_transmittance = vec3(1.0, 1.0, 1.0);
    vec3 accum_light = vec3(0.0, 0.0, 0.0);
    /* ============================= RAYMARCH ============================ */
    for (int i = 0; i < sample_count; i++) {
        float new_ray_shift = integration_length * (float(i) + 0.3) / sample_count;
        integration_step = new_ray_shift - old_ray_shift;
        vec3 new_position = world_position + new_ray_shift * world_direction;
        old_ray_shift = new_ray_shift;

        ScatteringSample medium_scattering = sample_medium_scattering_detailed(gpu_input, new_position);
        vec3 medium_extinction = sample_medium_extinction(gpu_input, new_position);

        vec3 up_vector = normalize(new_position);
        TransmittanceParams transmittance_lut_params = TransmittanceParams(length(new_position), dot(sun_direction, up_vector));

        /* uv coordinates later used to sample transmittance texture */
        vec2 trans_texture_uv = transmittance_lut_to_uv(transmittance_lut_params, deref(gpu_input).sky_settings.atmosphere_bottom, deref(gpu_input).sky_settings.atmosphere_top);
        vec3 transmittance_to_sun = texture(daxa_sampler2D(transmittance_lut, g_sampler_llc), trans_texture_uv).rgb;

        vec3 phase_times_scattering = medium_scattering.mie * mie_phase_value + medium_scattering.ray * rayleigh_phase_value;

        float earth_intersection_distance = ray_sphere_intersect_nearest(
            new_position, sun_direction, planet_zero, deref(gpu_input).sky_settings.atmosphere_bottom);
        float in_earth_shadow = earth_intersection_distance == -1.0 ? 1.0 : 0.0;

        vec3 multiscattered_luminance = get_multiple_scattering(new_position, dot(sun_direction, up_vector));

        /* Light arriving from the sun to this point */
        vec3 sun_light = in_earth_shadow * transmittance_to_sun * phase_times_scattering +
                         multiscattered_luminance * (medium_scattering.ray + medium_scattering.mie);

        /* TODO: This probably should be a texture lookup*/
        vec3 trans_increase_over_integration_step = exp(-(medium_extinction * integration_step));

        vec3 sun_light_integ = (sun_light - sun_light * trans_increase_over_integration_step) / medium_extinction;

        if (medium_extinction.r == 0.0) {
            sun_light_integ.r = 0.0;
        }
        if (medium_extinction.g == 0.0) {
            sun_light_integ.g = 0.0;
        }
        if (medium_extinction.b == 0.0) {
            sun_light_integ.b = 0.0;
        }

        accum_light += accum_transmittance * sun_light_integ;
        accum_transmittance *= trans_increase_over_integration_step;
    }
    result.luminance = accum_light;
    result.transmittance = accum_transmittance;

    return result;
}

#define GATHER_METHOD 1

layout(local_size_x = 1, local_size_y = 1, local_size_z = 32) in;
void main() {
    float slice_begin_fs = float(gl_SubgroupInvocationID + 0) / float(SKY_AE_RES.z);
    float slice_end_fs = float(gl_SubgroupInvocationID + 1) / float(SKY_AE_RES.z);

    const vec3 ae_uvw = sky_froxel_get_uvw(gl_GlobalInvocationID.xyz);
    vec4 froxel_begin_ws_h = deref(gpu_input).player.cam.view_to_world *
                             deref(gpu_input).player.cam.sample_to_view *
                             vec4(fs_to_cs(gpu_input, vec3(ae_uvw.xy, slice_begin_fs)), 1.0);
    vec4 froxel_end_ws_h = deref(gpu_input).player.cam.view_to_world *
                           deref(gpu_input).player.cam.sample_to_view *
                           vec4(fs_to_cs(gpu_input, vec3(ae_uvw.xy, slice_end_fs)), 1.0);
    vec3 froxel_begin_ws = froxel_begin_ws_h.xyz / froxel_begin_ws_h.w;
    vec3 froxel_end_ws = froxel_end_ws_h.xyz / froxel_end_ws_h.w;
    vec3 froxel_begin_to_end_ws = froxel_end_ws - froxel_begin_ws;
    // const float froxel_size_z = length(froxel_begin_to_end_ws);
    // We can use the ws direction because sky space and ws are just translations and uniform scales of each other.
    vec3 camera_to_froxel_ks_dir = normalize(froxel_begin_to_end_ws);

#if GATHER_METHOD
    const vec3 froxel_begin_ks = sky_space_from_ws(gpu_input, froxel_begin_ws);
#else
    const vec3 froxel_begin_ks = sky_space_from_ws(gpu_input, deref(gpu_input).player.pos);
#endif
    const vec3 froxel_end_ks = sky_space_from_ws(gpu_input, froxel_end_ws);

    vec3 sun_direction = deref(gpu_input).sky_settings.sun_direction;
    const int sample_count = 4;
    RaymarchResult res = integrate_scattered_luminance(froxel_begin_ks, camera_to_froxel_ks_dir, sun_direction, sample_count, length(froxel_end_ks - froxel_begin_ks));

#if GATHER_METHOD
    // this is identical
    // res.luminance = subgroupInclusiveAdd(res.luminance);
    // res.transmittance = subgroupInclusiveMul(res.transmittance);

    // to this
    for (uint i = 1; i < 32; ++i) {
        vec3 prev_lum = subgroupBroadcast(res.luminance, i - 1);
        vec3 prev_trn = subgroupBroadcast(res.transmittance, i - 1);
        if (gl_SubgroupInvocationID == i) {
            res.luminance = prev_lum + prev_trn * res.luminance;
            res.transmittance *= prev_trn;
        }
    }
#endif

    float average_transmittance = (res.transmittance.x + res.transmittance.y + res.transmittance.z) / 3.0;

    imageStore(daxa_image3D(aerial_perspective_lut), ivec3(gl_GlobalInvocationID.xy, gl_SubgroupInvocationID), vec4(res.luminance, average_transmittance));
    // imageStore(daxa_image3D(aerial_perspective_lut), ivec3(gl_GlobalInvocationID.xy, gl_SubgroupInvocationID), vec4(vec3(res.transmittance), average_transmittance));
}

#endif
