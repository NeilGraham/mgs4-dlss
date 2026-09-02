// Velocity pass vertex shader: reads the stream-out captures of one object (clip positions of every emitted vertex,
// this frame and last frame, same order) and rasterizes the current ones. Vertices past the captured count (the draw
// is issued with an upper bound) collapse to a point so their triangles have no area.
StructuredBuffer<float4> curPos  : register(t0);
StructuredBuffer<float4> prevPos : register(t1);
ByteAddressBuffer        curCnt  : register(t2);   // stream-output buffer-filled-size counters, 16 bytes apart
ByteAddressBuffer        prevCnt : register(t3);

cbuffer Draw : register(b1) { uint curOff; uint prevOff; uint curCtr; uint prevCtr; uint flags; uint pad0; uint pad1; uint pad2; };

struct VSOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prv : TEXCOORD1; };

VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    const uint n = min(curCnt.Load(curCtr * 16), prevCnt.Load(prevCtr * 16)) / 16;   // whole primitives only: a multiple of 3
    if (vid >= n) { o.pos = float4(0, 0, 0, 1); o.cur = o.pos; o.prv = o.pos; return o; }
    float4 c = curPos[curOff + vid];
    o.pos = c;
    o.cur = c;
    o.prv = prevPos[prevOff + vid];
    return o;
}
