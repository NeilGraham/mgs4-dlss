#include "dof_common.hlsli"
Texture2D<float4> src : register(t0);        // half-res colour + CoC
Texture2D<float4> unused : register(t1);
RWTexture2D<float4> dst : register(u0);      // rgb = blurred colour, a = blur coverage (0 = in focus)

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)halfSize.x || id.y >= (uint)halfSize.y) return;
    const float2 uvMax = (halfSize - 0.5) / halfSize;
    float2 uv = clamp((float2(id.xy) + 0.5) / halfSize, 0.0, uvMax);
    float4 cen = src.SampleLevel(linClamp, uv, 0); cen.rgb = saturate(cen.rgb);
    const float4 c10 = g[2], c11 = g[3];
    if (abs(cen.w) < c10.w) { dst[id.xy] = float4(cen.rgb, 0.0); return; }
    const float modBase = 1.0 - c11.z, modAmp = 2.0 * c11.z; const bool modOn = 1.0 < c11.y; const float modFreq = c11.y * 0.159155;
    const float wCap = abs(cen.w) * c10.z;
    const bool plainSum = c11.y < 0.0;
    float3 acc = cen.rgb; float angle = 0.0, count = 1.0, wsum = 0.0, radius = c11.x;
    [loop] for (int i = 0; i < 256; ++i) {
        if (radius >= c10.x) break;
        float m = abs(frac(angle * modFreq + c11.w) - 0.5) * modAmp + modBase; m = modOn ? m : 1.0;
        float sn, cs; sincos(angle, sn, cs);
        float2 suv = clamp(uv + stepUV * float2(cs, sn) * (m * radius), 0.0, uvMax);
        float4 s = src.SampleLevel(linClamp, suv, 0); s.rgb = saturate(s.rgb);
        float w = (cen.w < s.w) ? min(wCap, abs(s.w)) : abs(s.w);
        float lo = radius - c11.y, span = (radius + c11.y) - lo;
        float t = saturate((w - lo) / span); t = t * t * (3.0 - 2.0 * t);
        float3 mixv = acc + lerp(acc / count, s.rgb, t);
        acc = plainSum ? (acc + s.rgb) : mixv;
        wsum += w; count += 1.0; angle += 2.399963; radius += c11.x / radius;
    }
    dst[id.xy] = float4(acc / count, saturate(wsum / (count - 1.0)));
}
