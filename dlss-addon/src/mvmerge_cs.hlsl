// Merges the per-object vectors (velocity pass, rendered into their own texture over a sentinel) into the camera
// vectors: a pixel takes the object vector only where it differs from the camera vector by less than maxDelta pixels.
// A bogus pairing of stream-out captures (another instance of the same mesh, the same mesh drawn in another space)
// produces vectors of tens to hundreds of pixels; a character's own screen motion relative to the camera is far below
// that, so the check drops exactly the wrong ones and those pixels keep the camera vector.
cbuffer CB : register(b0)
{
    float2 size;       // vector texture size
    float  maxDelta;   // pixels; <= 0 = accept every object vector
    float  sentinel;   // objMv.x >= sentinel = not written by the velocity pass
};
Texture2D<float2>   objMv  : register(t0);
Texture2D<float2>   unused : register(t1);
RWTexture2D<float2> mvTex  : register(u0);
RWTexture2D<float>  dummy  : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
    const float2 o = objMv.Load(int3(id.xy, 0));
    if (o.x >= sentinel) return;                       // camera vector stays
    const float2 c = mvTex[id.xy];
    if (maxDelta > 0.0 && length(o - c) > maxDelta) return;   // implausible: a wrong capture pairing
    mvTex[id.xy] = o;
}
