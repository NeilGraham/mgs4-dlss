// Velocity pass pixel shader: motion in pixels from the previous to the current position (DLSS convention: where
// this pixel was last frame, relative to now; y down). Perspective-correct interpolation of the clip positions gives
// the previous position of the surface point under the pixel. The captured positions carry the add-on's sub-pixel
// camera jitter (the clip matrices are patched in place before the draw); it is removed here so the vectors are
// jitter-free as DLSS expects.
cbuffer Frame : register(b0) { float2 size; float2 jitCur; float2 jitPrev; float2 pad; };
cbuffer Draw  : register(b1) { uint curOff; uint prevOff; uint curCtr; uint prevCtr; uint flags; uint pad0; uint pad1; uint pad2; };

struct VSOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prv : TEXCOORD1; };

float2 main(VSOut i) : SV_Target
{
    if (i.prv.w <= 1e-4 || i.cur.w <= 1e-4) discard;   // behind the camera last frame: keep the camera vector
    float2 cn = i.cur.xy / i.cur.w - ((flags & 1u) ? jitCur : 0.0.xx);
    float2 pn = i.prv.xy / i.prv.w - ((flags & 2u) ? jitPrev : 0.0.xx);
    float2 cp = float2((cn.x * 0.5 + 0.5) * size.x, (0.5 - cn.y * 0.5) * size.y);
    float2 pp = float2((pn.x * 0.5 + 0.5) * size.x, (0.5 - pn.y * 0.5) * size.y);
    return pp - cp;
}
