// Dynamic resolution: the game renders the 3D scene into a (scale x scale) part of its full-size targets and its post
// chain upscales that to the rectangle the scene occupies in the final image (the whole image normally; a layout window
// in the mission briefings) before the composite. DLSS runs on the full image, so the depth has to be brought to the
// same grid: nearest-neighbor stretch of the sub-rect depth into a full-size R32 copy. Pixels outside the rectangle
// are the frozen panels around the window: far plane (reversed-Z: 0).
cbuffer CB : register(b0)
{
    float2 outSize;   // full size
    float2 scale;     // sub-rect size / rectangle size (x, y)
    float2 origin;    // full-grid pixel p samples the depth at p * scale + origin
    float2 pad;
    float4 rect;      // the rectangle the scene occupies in the full image (x, y, w, h)
};
Texture2D<float>    depthTex : register(t0);
Texture2D<float>    unused   : register(t1);
RWTexture2D<float>  outTex   : register(u0);
RWTexture2D<float>  dummy    : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)outSize.x || id.y >= (uint)outSize.y) return;
    float2 p = float2(id.xy) + 0.5;
    if (p.x < rect.x || p.y < rect.y || p.x >= rect.x + rect.z || p.y >= rect.y + rect.w) { outTex[id.xy] = 0.0; return; }
    int2 sp = int2(floor(p * scale + origin));
    outTex[id.xy] = depthTex.Load(int3(sp, 0));
}
