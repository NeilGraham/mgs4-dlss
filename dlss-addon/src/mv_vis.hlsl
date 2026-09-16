// Debug: paint the motion-vector field into the output texture. Gray = no motion; red/green shift = x/y motion;
// blue = dynamic-object mask. mode 1: the scene depth (bound at t1) as view distance on a log scale.
cbuffer CB : register(b0)
{
    float2 inSize;    // motion vector texture size
    float2 outSize;   // output texture size
    float  scale;     // color units per pixel of motion
    float  blend;     // > 0: blend the field over the existing output instead of replacing it
    float  mode;      // 0 = motion vectors (t0 + mask at t1); 1 = depth at t1
    float  nearZ;     // the game's clip-space near constant: reversed-Z, depth = nearZ / w
};
Texture2D<float2>   mvTex   : register(t0);
Texture2D<float>    maskTex : register(t1);
RWTexture2D<float4> outTex  : register(u0);
RWTexture2D<float>  dummy   : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)outSize.x || id.y >= (uint)outSize.y) return;
    uint2 src = uint2((uint)(id.x * inSize.x / outSize.x), (uint)(id.y * inSize.y / outSize.y));
    if (mode > 0.5)
    {
        // the game's reversed-Z depth (d = nearZ / w, 0 = far) as view distance: white at the near plane, black 4096
        // near-plane distances out (about 200 m at this game's 51 mm near plane), one octave per 1/12 of the swing
        float d = maskTex.Load(int3(src, 0));
        float dist = d > 0.0 ? nearZ / d : 1e9;
        float g = 1.0 - saturate(log2(max(dist / nearZ, 1.0)) / 12.0);
        outTex[id.xy] = float4(g, g, g, 1.0);
        return;
    }
    float2 mv = mvTex.Load(int3(src, 0));
    float m = maskTex.Load(int3(src, 0));
    float4 vis = float4(saturate(0.5 + mv.x * scale), saturate(0.5 + mv.y * scale), m > 0.5 ? 1.0 : saturate(length(mv) * scale * 0.5), 1.0);
    outTex[id.xy] = blend > 0.0 ? lerp(outTex[id.xy], vis, blend) : vis;
}
