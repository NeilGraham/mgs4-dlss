// Pass 1b, at the DLSS insertion: the half-resolution color of the DLSS output packed with the circle of confusion
// that dof_coc_cs evaluated at the game's CoC draw - the input of the spiral gather (rgb = color, a = CoC).
#include "dof_common.hlsli"
Texture2D<float4>   colorTex : register(t0);   // DLSS output (full resolution)
Texture2D<float>    cocIn    : register(t1);   // this frame's CoC (half resolution, full grid)
RWTexture2D<float4> cocTex   : register(u0);   // rgb = half-res color, a = signed circle of confusion
RWTexture2D<float>  dummy    : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)halfSize.x || id.y >= (uint)halfSize.y) return;
    float2 uvc = (float2(id.xy) + 0.5) / halfSize;
    float3 col = colorTex.SampleLevel(linClamp, uvc, 0).rgb;
    cocTex[id.xy] = float4(col, cocIn.Load(int3(id.xy, 0)));
}
