$input v_texcoord0
#include <bgfx_shader.sh>

USAMPLER2D(s_frame, 0);
uniform vec4 u_batch;

void main()
{
    ivec2 size = ivec2(u_batch.xy);
    ivec2 position = clamp(ivec2(v_texcoord0 * u_batch.xy), ivec2(0), size - ivec2(1));
    uint pixel = texelFetch(s_frame, position, 0).x;
    gl_FragColor = vec4(float(pixel & 31u), float((pixel >> 5u) & 31u),
                        float((pixel >> 10u) & 31u), 31.0) / 31.0;
}
