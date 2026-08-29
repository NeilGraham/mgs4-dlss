// Dynamic resolution: the game renders the 3D scene into the top-left (scale x scale) part of its full-size targets and
// its post chain upscales that to the full final image before the composite. DLSS runs on the full image, so the
// depth has to be brought to the same grid: nearest-neighbour stretch of the sub-rect depth into a full-size R32 copy.
cbuffer CB : register(b0)
{
    float2 outSize;   // full size
    float2 scale;     // sub-rect size / full size (x, y)
};
Texture2D<float>    depthTex : register(t0);
Texture2D<float>    unused   : register(t1);
RWTexture2D<float>  outTex   : register(u0);
RWTexture2D<float>  dummy    : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)outSize.x || id.y >= (uint)outSize.y) return;
    int2 sp = int2(floor((float2(id.xy) + 0.5) * scale));
    outTex[id.xy] = depthTex.Load(int3(sp, 0));
}
