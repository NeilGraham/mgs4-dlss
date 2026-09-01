// Pass 1a, at the game's CoC draw: the signed circle of confusion of every pixel of the full half-resolution grid from
// the game's linear depth copy - the same texture, constants and moment as the game's own pass, so whatever the depth
// copy holds later in the frame cannot matter. Output in spiral units (dof_gather_cs), the colour comes later.
#include "dof_common.hlsli"
Texture2D<float>   depthTex : register(t0);   // the game's linear depth copy (R32_FLOAT, view-space depth)
Texture2D<float>   unused   : register(t1);
RWTexture2D<float> cocTex   : register(u0);   // signed circle of confusion, half resolution, full grid
RWTexture2D<float> dummy    : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)halfSize.x || id.y >= (uint)halfSize.y) return;
    // the game: uv = (2 * halfResPixel + c12.xy) / c16.xy, with its half-res pixel on the sub-rect grid (ours * k)
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
    cocTex[id.xy] = coc;
}
