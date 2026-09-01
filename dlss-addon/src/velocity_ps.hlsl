// Velocity pass pixel shader: motion in pixels from the previous to the current position (DLSS convention: where
// this pixel was last frame, relative to now; y down). Perspective-correct interpolation of the clip positions gives
// the previous position of the surface point under the pixel. The captured positions carry the add-on's sub-pixel
// camera jitter (the clip matrices are patched in place before the draw); it is removed here so the vectors are
// jitter-free as DLSS expects.
// Plausibility: the previous positions come from a stream-out capture paired on the CPU (same geometry, nearest
// constant signature); a wrong pairing - another instance of the mesh metres away, or the same mesh captured in another
// projection - gives vectors of hundreds of pixels, or a field that varies wildly across one surface. A legitimate
// object vector is bounded (well under 200 px a frame even in fast pans) and smooth across the triangle, so a fragment
// beyond either limit is discarded and keeps the camera vector already in the target.
cbuffer Frame : register(b0) { float2 size; float2 jitCur; float2 jitPrev; float2 prevSize; float maxPixels; float maxGradient; float2 pad; };
cbuffer Draw  : register(b1) { uint curOff; uint prevOff; uint curCtr; uint prevCtr; uint flags; uint pad0; uint pad1; uint pad2; };

Texture2D<float> depthFull : register(t4);   // full-grid scene depth (dynamic resolution) for the manual depth test

struct VSOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prv : TEXCOORD1; };

float2 main(VSOut i) : SV_Target
{
    // vector and its screen-space gradients first: derivatives must be taken before any lane of the quad discards
    float2 cn = i.cur.xy / i.cur.w - ((flags & 1u) ? jitCur : 0.0.xx);
    float2 pn = i.prv.xy / i.prv.w - ((flags & 2u) ? jitPrev : 0.0.xx);
    float2 cp = float2((cn.x * 0.5 + 0.5) * size.x, (0.5 - cn.y * 0.5) * size.y);
    float2 pp = float2((pn.x * 0.5 + 0.5) * prevSize.x, (0.5 - pn.y * 0.5) * prevSize.y);   // previous frame's render scale (dynamic resolution)
    float2 mv = pp - cp;
    const float2 gx = ddx(mv), gy = ddy(mv);
    const float grad = max(abs(gx.x) + abs(gx.y), abs(gy.x) + abs(gy.y));   // pixels of motion per pixel of screen
    if (i.prv.w <= 1e-4 || i.cur.w <= 1e-4) discard;   // behind the camera last frame: keep the camera vector
    if (flags & 4u) {   // reversed-Z: only the visible surface (its depth equals the scene depth) writes
        float d = depthFull.Load(int3(int2(i.pos.xy), 0));
        if (i.pos.z < d - 2e-4) discard;
    }
    if (maxPixels > 0.0 && dot(mv, mv) > maxPixels * maxPixels) discard;
    if (maxGradient > 0.0 && grad > maxGradient) discard;
    return mv;
}
