#include "dof_common.hlsli"
Texture2D<float4> blur : register(t0);       // half-res blurred layer + coverage
Texture2D<float4> unused : register(t1);
RWTexture2D<float4> outTex : register(u0);   // DLSS output, blended in place

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y) return;
    float4 b = blur.SampleLevel(linClamp, (float2(id.xy) + 0.5) / fullSize, 0);
    if (b.a <= 0.003922) return;   // the game's pass discards these (no blend)
    float4 o = outTex[id.xy];
    o.rgb = lerp(o.rgb, saturate(b.rgb), saturate(b.a));
    outTex[id.xy] = o;
}
