#include <bgfx_compute.sh>

UIMAGE2D_RW(s_target, r32ui, 0);
BUFFER_RO(s_atlas, uint, 1);
BUFFER_RO(s_table, uint, 2);
BUFFER_RO(s_coverage, uint, 3);
uniform vec4 u_batch;

uint byteAt(uint address)
{
    address &= 1048575u;
    return (s_atlas[address >> 2u] >> ((address & 3u) * 8u)) & 255u;
}

uint wordAt(uint address)
{
    address &= 1048574u;
    return byteAt(address) | (byteAt(address + 1u) << 8u);
}

uint modulate(uint color, uint rgb, bool mask)
{
    uint result = mask ? (color & 32768u) : 0u;
    for (uint channel = 0u; channel < 3u; ++channel)
    {
        uint index = (((rgb >> (channel * 8u)) & 255u) >> 3u) * 32u
            + ((color >> (channel * 5u)) & 31u);
        result |= ((s_table[index >> 2u] >> ((index & 3u) * 8u)) & 255u) << (channel * 5u);
    }
    return result;
}

NUM_THREADS(64, 1, 1)
void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= uint(u_batch.y))
        return;
    uint width = uint(u_batch.x);
    uint nodeOffset = uint(u_batch.z);
    uint materialOffset = uint(u_batch.w);
    uint destination = s_coverage[index * 2u];
    uint node = s_coverage[index * 2u + 1u];
    ivec2 position = ivec2(destination % width, destination / width);
    uint pixel = imageLoad(s_target, position).x;
    while (node != 4294967295u)
    {
        uint at = nodeOffset + node * 3u;
        uint uv = s_coverage[at];
        uint material = materialOffset + s_coverage[at + 1u] * 4u;
        node = s_coverage[at + 2u];
        uint page = s_coverage[material];
        uint clut = s_coverage[material + 1u];
        uint opcode = s_coverage[material + 2u];
        uint rgb = s_coverage[material + 3u];
        uint code = opcode & 253u;
        uint format = (page >> 7u) & 3u;
        bool textured = code == 44u || code == 100u;
        uint color;
        if (textured)
        {
            uint address = (page & 15u) * 128u + ((page & 16u) != 0u ? 524288u : 0u)
                + (uv >> 8u) * 2048u;
            uint x = uv & 255u;
            if (format == 0u)
                color = wordAt(clut * 32u + ((byteAt(address + x / 2u) >> ((x & 1u) * 4u)) & 15u) * 2u);
            else if (format == 1u)
                color = wordAt(clut * 32u + byteAt(address + x) * 2u);
            else
                color = wordAt(address + x * 2u);
            if (format < 2u)
                color = modulate(color, rgb, true);
            if (color == 0u)
                continue;
            if (code == 100u && (opcode & 2u) == 0u && format == 2u &&
                (rgb & 255u) != 128u && ((rgb >> 8u) & 255u) != 128u && ((rgb >> 16u) & 255u) != 128u)
                color = modulate(color, rgb, false);
        }
        else
            color = ((rgb & 255u) >> 3u) | ((((rgb >> 8u) & 255u) >> 3u) << 5u)
                | ((((rgb >> 16u) & 255u) >> 3u) << 10u);
        if ((opcode & 2u) != 0u && (!textured || (color & 32768u) != 0u))
            color = ((color >> 1u) & 15855u) + ((pixel >> 1u) & 15855u);
        pixel = color;
    }
    imageStore(s_target, position, uvec4(pixel, 0u, 0u, 0u));
}
