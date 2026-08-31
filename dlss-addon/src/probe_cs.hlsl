// Pipeline probe: 240x135 luminance downsample of a pipeline stage, read back to the CPU every frame so layout
// anomalies (dynamic-resolution sub-rect content where full-grid content is expected, missing blur) are detected
// programmatically per frame instead of from screen captures.
#include "dof_common.hlsli"
Texture2D<float4> src : register(t0);
Texture2D<float4> unused2 : register(t1);
RWTexture2D<float> dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 240 || id.y >= 135) return;
    // halfSize.xy carries the source size here; average a 4-tap box in the source block for stability
    float2 blockUv = (float2(id.xy) + 0.5) / float2(240.0, 135.0);
    float2 h = 0.25 / float2(240.0, 135.0);
    float lum = 0.0;
    lum += dot(src.SampleLevel(linClamp, blockUv + float2(-h.x, -h.y), 0).rgb, float3(0.299, 0.587, 0.114));
    lum += dot(src.SampleLevel(linClamp, blockUv + float2( h.x, -h.y), 0).rgb, float3(0.299, 0.587, 0.114));
    lum += dot(src.SampleLevel(linClamp, blockUv + float2(-h.x,  h.y), 0).rgb, float3(0.299, 0.587, 0.114));
    lum += dot(src.SampleLevel(linClamp, blockUv + float2( h.x,  h.y), 0).rgb, float3(0.299, 0.587, 0.114));
    dst[id.xy] = lum * 0.25;
}
