// Dynamic resolution: the game renders into a sub-rect of its targets and its composite stretches that sub-rect to
// the screen. DLSS produces a full-size output; this pass resamples it back into the sub-rect (4-tap bilinear
// supersample) so the game's composite shows the anti-aliased image without any change to the game's draws.
cbuffer CB : register(b0)
{
    float2 subSize;    // sub-rect (destination) size
    float2 fullSize;   // DLSS output size
};
Texture2D<float4>   src    : register(t0);
Texture2D<float4>   unused : register(t1);
RWTexture2D<float4> dst    : register(u0);
RWTexture2D<float>  dummy  : register(u1);

float4 bilinear(float2 p)
{
    p = clamp(p, 0.0, fullSize - 1.0);
    int2 p0 = int2(floor(p)); float2 f = p - p0;
    int2 p1 = min(p0 + 1, int2(fullSize) - 1);
    float4 a = src.Load(int3(p0.x, p0.y, 0)), b = src.Load(int3(p1.x, p0.y, 0));
    float4 c = src.Load(int3(p0.x, p1.y, 0)), d = src.Load(int3(p1.x, p1.y, 0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)subSize.x || id.y >= (uint)subSize.y) return;
    float2 scale = fullSize / subSize;
    float2 c = (float2(id.xy) + 0.5) * scale - 0.5;
    float2 h = 0.25 * scale;   // 2x2 supersample over the destination pixel footprint
    float4 s = bilinear(c + float2(-h.x, -h.y)) + bilinear(c + float2(h.x, -h.y)) + bilinear(c + float2(-h.x, h.y)) + bilinear(c + float2(h.x, h.y));
    dst[id.xy] = s * 0.25;
}
