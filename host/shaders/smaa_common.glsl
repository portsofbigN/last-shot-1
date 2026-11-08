// Shared set-up of the SMAA 1x compute passes (smaa_*.comp): the official SMAA.hlsl (third_party/smaa, MIT) in its
// GLSL mode, its vertex-stage set-up computed per pixel. The passes cover a rectangle of the frame; the textures have
// the frame's coordinates and size (metrics).
layout(local_size_x = 16, local_size_y = 16) in;
layout(push_constant) uniform Pc { vec4 rect; vec4 metrics; } pc;   // x, y, w, h; 1/w, 1/h, w, h
#define SMAA_GLSL_4
#define SMAA_RT_METRICS pc.metrics
#define SMAA_PRESET_HIGH
#define discard return float2(0.0, 0.0)   // the edge passes return "no edge" (compute shaders cannot discard)
#include "../../third_party/smaa/SMAA.hlsl"
#undef discard

bool smaa_pixel(out ivec2 p, out vec2 uv) {
    ivec2 g = ivec2(gl_GlobalInvocationID.xy);
    p = g + ivec2(pc.rect.xy);
    uv = (vec2(p) + 0.5) * pc.metrics.xy;
    return g.x < int(pc.rect.z) && g.y < int(pc.rect.w);
}
