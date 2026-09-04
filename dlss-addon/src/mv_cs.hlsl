// Camera-only motion vectors from depth, plus the dynamic-object mask.
// For each pixel: reproject with the previous frame's view-projection and write (prev - cur) in pixels, pointing to
// where the pixel was last frame (DLSS convention). Pixels covered by dynamic draws (character/prop depth written into
// dynDepth by the add-on's replayed draws) get mask = 1 and, optionally, zero motion (third-person characters mostly
// keep their screen position while the camera turns).
// Matrices are the game's row-major clip matrices (rows = clip x,y,z,w), reversed-Z with z_clip == near.
cbuffer CB : register(b0)
{
    row_major float4x4 invVP;    // inverse of the current (unjittered) view-projection
    row_major float4x4 prevVP;   // previous frame's view-projection
    float2 size;                 // render resolution
    float  nearZ;                // z_clip constant (row 2, w component)
    float  reset;                // 1 = write zero motion (camera cut / no history)
    float  dynZeroMV;            // 1 = zero motion on dynamic pixels
    float2 depthScale;           // dynamic resolution: the scene depth occupies the top-left (scale x size) of its texture
    float  pad;
    float4 rect;                 // the 3D scene's rectangle in the frame (x, y, w, h): the camera's NDC maps to it; outside it the screen is static
    float2 depthOrigin;          // full-grid pixel p samples the scene depth at p * depthScale + depthOrigin (a scaled layout window keeps its position)
    float2 pad2;
};
Texture2D<float>    depthTex : register(t0);
Texture2D<float>    dynDepth : register(t1);
RWTexture2D<float2> mvTex    : register(u0);
RWTexture2D<float>  maskTex  : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
    int2 sp = int2(floor((float2(id.xy) + 0.5) * depthScale + depthOrigin));   // full-grid pixel -> sub-res depth sample
    float d = depthTex.Load(int3(sp, 0));
    float dd = dynDepth.Load(int3(sp, 0));
    // dynamic if the replayed draws wrote depth here and it is (about) the visible surface
    bool dyn = dd > 1e-7 && dd >= d * 0.995;
    maskTex[id.xy] = dyn ? 1.0 : 0.0;
    if (reset > 0.5 || (dyn && dynZeroMV > 0.5)) { mvTex[id.xy] = float2(0, 0); return; }

    float2 cur = float2(id.x + 0.5, id.y + 0.5);
    if (cur.x < rect.x || cur.y < rect.y || cur.x >= rect.x + rect.z || cur.y >= rect.y + rect.w) { mvTex[id.xy] = float2(0, 0); return; }   // frozen screen around a 3D window
    float2 ndc = float2((cur.x - rect.x) / rect.z * 2.0 - 1.0, 1.0 - (cur.y - rect.y) / rect.w * 2.0);
    float4 clip;
    if (d <= 1e-7)
        clip = float4(ndc.x, ndc.y, 0.0, 1.0);             // far plane: a direction (point at infinity), still moves under rotation
    else {
        float w = nearZ / d;                               // depth = z_clip / w = nearZ / w
        clip = float4(ndc.x * w, ndc.y * w, nearZ, w);
    }
    float4 world = mul(invVP, clip);
    float4 pc = mul(prevVP, world);
    if (pc.w <= 1e-3) { mvTex[id.xy] = float2(0, 0); return; }
    float2 pndc = pc.xy / pc.w;
    float2 prev = float2(rect.x + (pndc.x * 0.5 + 0.5) * rect.z, rect.y + (0.5 - pndc.y * 0.5) * rect.w);
    mvTex[id.xy] = prev - cur;
}
