// Debug: paint the motion-vector field into the output texture. Grey = no motion; red/green shift = x/y motion.
cbuffer CB : register(b0)
{
    float2 inSize;    // motion vector texture size
    float2 outSize;   // output texture size
    float  scale;     // colour units per pixel of motion
    float3 pad;
};
Texture2D<float2>   mvTex  : register(t0);
RWTexture2D<float4> outTex : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)outSize.x || id.y >= (uint)outSize.y) return;
    uint2 src = uint2((uint)(id.x * inSize.x / outSize.x), (uint)(id.y * inSize.y / outSize.y));
    float2 mv = mvTex.Load(int3(src, 0));
    outTex[id.xy] = float4(saturate(0.5 + mv.x * scale), saturate(0.5 + mv.y * scale), saturate(length(mv) * scale * 2.0), 1.0);
}
