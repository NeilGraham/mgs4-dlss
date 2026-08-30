#include "dof_common.hlsli"
Texture2D<float4> blur : register(t0);       // half-res blurred layer + coverage
Texture2D<float4> maskTex : register(t1);    // overlay mask layer (full res) when hasMask, else the CoC texture
RWTexture2D<float4> outTex : register(u0);   // DLSS output, blended in place

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)fullSize.x || id.y >= (uint)fullSize.y) return;
    float2 uv = (float2(id.xy) + 0.5) / fullSize;
    float4 b = blur.SampleLevel(linClamp, uv, 0);
    if (debugView > 0.5) {
        float4 dbg = float4(0, 0, 0, 1);
        if (debugView < 1.5) dbg.rgb = b.a > 0.003922 ? saturate(b.rgb) : 0.0;
        else if (debugView < 2.5) dbg.rgb = saturate(b.a).xxx;
        else { float4 mk = hasMask > 0.5 ? maskTex.Load(int3(id.xy, 0)) : 0.0; dbg.rgb = saturate(max(max(mk.r, mk.g), max(mk.b, mk.a)) * 2.0).xxx; }
        outTex[id.xy] = dbg; return;
    }
    if (b.a <= 0.003922) return;   // the game's pass discards these (no blend)
    float keep = 0.0;   // overlays the game draws after its DoF (title cards, captions) keep the sharp pixel
    if (hasMask > 0.5) { float4 mk = maskTex.Load(int3(id.xy, 0)); keep = saturate(max(max(mk.r, mk.g), max(mk.b, mk.a)) * 2.0); }
    float4 o = outTex[id.xy];
    o.rgb = lerp(o.rgb, saturate(b.rgb), saturate(b.a) * (1.0 - keep));
    outTex[id.xy] = o;
}
