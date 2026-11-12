#include <bgfx_compute.sh>

UIMAGE2D_RO(s_source, r32ui, 0);
UIMAGE2D_RW(s_target, r32ui, 1);
uniform vec4 u_batch;

NUM_THREADS(8, 8, 1)
void main()
{
    uvec2 position = gl_GlobalInvocationID.xy;
    if (position.x >= uint(u_batch.x) || position.y >= uint(u_batch.y))
        return;

    uint pixel = imageLoad(s_source, ivec2(position)).x;
    uint shade = uint(u_batch.z);
    uint red = (pixel & 31u) * shade / 255u;
    uint green = ((pixel >> 5u) & 31u) * shade / 255u;
    uint blue = ((pixel >> 10u) & 31u) * shade / 255u;
    uint result = (pixel & 32768u) | red | (green << 5u) | (blue << 10u);
    imageStore(s_target, ivec2(position), uvec4(result, 0u, 0u, 0u));
}
