#include "dof_common.hlsli"
Texture2D<float4> colorTex : register(t0);   // DLSS output (full resolution)
Texture2D<float>  depthTex : register(t1);   // the game's linear depth copy (R32_FLOAT, view-space depth)
RWTexture2D<float4> cocTex : register(u0);   // rgb = half-res colour, a = signed circle of confusion (in spiral units)

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)halfSize.x || id.y >= (uint)halfSize.y) return;
    float2 uvc = (float2(id.xy) + 0.5) / halfSize;
    float3 col = colorTex.SampleLevel(linClamp, uvc, 0).rgb;
    float2 uvd = (float2(id.xy) * 2.0 * depthScale + depthOff + depthJitter) / depthSize;
    float d = depthTex.SampleLevel(linClamp, uvd, 0).x;
    const float4 c8 = c[0], c9 = c[1], c10 = c[2];
    // game: r1 = d - c8.ywxz; relative (mode 2) and linear (mode 1) near / far terms, clamped, selected by thresholds
    float relNear = ((d - c8.y) / d) * c8.x;
    float relFar  = ((d - c8.w) / d) * c8.z;
    float linNear = (d - c8.x) * c8.y;
    float linFar  = (d - c8.z) * c8.w;
    float v = (d < c8.y) ? relNear : 0.0;
    v = (c8.w < d) ? relFar : v;
    v = (c10.y == 2.0) ? v : 0.0;
    float w = (d < c8.x) ? max(linNear, -c9.x) : v;
    w = (c8.z < d) ? min(linFar, c9.y) : w;
    float coc = (c10.y == 1.0) ? w : v;
    coc = clamp(coc, -1.0, 1.0) * c10.x * radiusScale;
    if (cocUnorm > 0.5) coc = saturate(coc);
    cocTex[id.xy] = float4(col, coc);
}
