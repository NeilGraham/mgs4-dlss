// Projector pass vertex shader: the in-world monitor showing the video call's caller, captured by stream-out with its
// texture coordinates (clip position + texcoord per vertex, 32 bytes). The pixel shader maps the caller feed's motion
// vectors through the monitor's texture mapping onto the screen.
StructuredBuffer<float4> curPos  : register(t0);
StructuredBuffer<float4> prevPos : register(t1);
ByteAddressBuffer        curCnt  : register(t2);
ByteAddressBuffer        prevCnt : register(t3);

cbuffer Draw : register(b1) { uint curOff; uint rectW; uint curCtr; uint rectH; uint flags; uint rectX; uint rectY; uint pad2; };

struct VSOut { float4 pos : SV_Position; float4 cur : TEXCOORD0; float4 uv : TEXCOORD1; };

VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    const uint n = curCnt.Load(curCtr * 16) / 32;   // whole vertices of 32 bytes
    if (vid >= n) { o.pos = float4(0, 0, 0, 1); o.cur = o.pos; o.uv = 0; return o; }
    float4 c = curPos[curOff + 2 * vid];
    o.pos = c;
    o.cur = c;
    o.uv = curPos[curOff + 2 * vid + 1];
    return o;
}
