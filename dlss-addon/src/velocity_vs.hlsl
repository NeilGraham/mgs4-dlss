// Velocity pass vertex shader: reads the stream-out captures (clip positions of every emitted vertex, current and
// previous frame, same order) and rasterises the current ones.
StructuredBuffer<float4> curPos  : register(t0);
StructuredBuffer<float4> prevPos : register(t1);

struct VSOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 prv : TEXCOORD1; };

VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    float4 c = curPos[vid];
    o.pos = c;
    o.cur = c;
    o.prv = prevPos[vid];
    return o;
}
