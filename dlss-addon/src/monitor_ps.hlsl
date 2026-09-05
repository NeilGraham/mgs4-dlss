// Projector pass pixel shader: for a pixel of the in-world monitor, look up the caller feed's motion at the feed pixel
// the monitor shows there (the feed occupies the rectangle rect of the feed-vector texture; the monitor's texture
// coordinates span it) and turn that feed-space motion into screen-space motion through the monitor's mapping - the
// screen-space derivatives of the feed pixel coordinate give feed-per-screen-pixel, inverted here. Added (blend) to the
// monitor surface's own camera vector already in the target. Depth-tested against the scene depth so a character in
// front of the monitor keeps their own vectors.
cbuffer Frame : register(b0) { float2 size; float2 jitCur; float2 jitPrev; float2 prevSize; float maxPixels; float maxGradient; float2 pad; };
cbuffer Draw  : register(b1) { uint curOff; uint rectW; uint curCtr; uint rectH; uint flags; uint rectX; uint rectY; uint pad2; };

Texture2D<float>  depthFull : register(t4);
Texture2D<float2> feedMv    : register(t5);

struct VSOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 uv : TEXCOORD1; };

float2 main(VSOut i) : SV_Target
{
    const float4 rect = float4(asfloat(rectX), asfloat(rectY), asfloat(rectW), asfloat(rectH));
    const float2 uv = (flags & 32u) ? float2(i.uv.x, 1.0 - i.uv.y) : i.uv.xy;
    const float2 fp = rect.xy + saturate(uv) * rect.zw;   // the feed pixel under this screen pixel
    const float2 dx = ddx(fp), dy = ddy(fp);               // derivatives before any discard
    if (flags & 4u) { const float d = depthFull.Load(int3(int2(i.pos.xy), 0)); if (i.pos.z < d - max(2e-5, d * 1e-3)) discard; }
    const float2 m = feedMv.Load(int3(int2(fp), 0));
    if (dot(m, m) < 1e-8) return float2(0, 0);
    const float det = dx.x * dy.y - dy.x * dx.y;
    if (abs(det) < 1e-9) return float2(0, 0);
    // feed motion -> screen motion: the inverse of the 2x2 [dx dy] (columns = feed change per screen x / y step)
    float2 s = float2(dy.y * m.x - dy.x * m.y, -dx.y * m.x + dx.x * m.y) / det;
    if (maxPixels > 0.0 && dot(s, s) > maxPixels * maxPixels) return float2(0, 0);
    return s;
}
