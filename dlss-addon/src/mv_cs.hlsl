// Camera-only motion vectors from depth: reproject each pixel of the current frame with the previous frame's
// view-projection and write (prev - cur) in pixels, pointing to where the pixel was last frame (DLSS convention).
// Matrices are the game's row-major clip matrices (rows = clip x,y,z,w), reversed-Z with z_clip == near.
cbuffer CB : register(b0)
{
    row_major float4x4 invVP;    // inverse of the current (unjittered) view-projection
    row_major float4x4 prevVP;   // previous frame's view-projection
    float2 size;                 // render resolution
    float  nearZ;                // z_clip constant (row 2, w component)
    float  reset;                // 1 = write zero motion (camera cut / no history)
};
Texture2D<float>  depthTex : register(t0);
RWTexture2D<float2> mvTex  : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
    float d = depthTex.Load(int3(id.xy, 0));
    if (reset > 0.5 || d <= 0.0) { mvTex[id.xy] = float2(0, 0); return; }

    float2 cur = float2(id.x + 0.5, id.y + 0.5);
    float2 ndc = float2(cur.x / size.x * 2.0 - 1.0, 1.0 - cur.y / size.y * 2.0);
    float w = nearZ / d;                                   // depth = z_clip / w = nearZ / w
    float4 clip = float4(ndc.x * w, ndc.y * w, nearZ, w);
    float4 world = mul(invVP, clip);
    float4 pc = mul(prevVP, world);
    if (pc.w <= 1e-3) { mvTex[id.xy] = float2(0, 0); return; }
    float2 pndc = pc.xy / pc.w;
    float2 prev = float2((pndc.x * 0.5 + 0.5) * size.x, (0.5 - pndc.y * 0.5) * size.y);
    mvTex[id.xy] = prev - cur;
}
