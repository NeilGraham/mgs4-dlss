// UI mask: wherever the replayed UI layer has content (the game's HUD is drawn into the final texture before the
// composite, so DLSS sees it), mark the pixel in DLSS's bias-current-colour mask and zero its motion vector. DLSS then
// takes the HUD from the current frame instead of reprojecting it with the camera - no HUD ghosting under motion -
// and frame generation gets zero motion on the HUD as well.
cbuffer CB : register(b0)
{
    float2 size;      // UI layer / motion vector texture size
    float  forceAll;  // 1 = every pixel (frames without a 3D scene: nothing to reconstruct temporally)
    float  pad;
};
Texture2D<float4>   uiTex   : register(t0);
Texture2D<float4>   unused  : register(t1);
RWTexture2D<float2> mvTex   : register(u0);
RWTexture2D<float>  maskTex : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
    float4 ui = uiTex.Load(int3(id.xy, 0));
    // The layer is cleared to zero. Only the bright HUD detail (text, bars, icons) is masked: it must not be reprojected
    // with the camera. The dim translucent panel backgrounds keep their camera vectors and normal accumulation - a
    // uniform tint cannot visibly ghost, while masking it would strip the anti-aliasing from the scene seen through it.
    if (forceAll > 0.5 || max(max(ui.r, ui.g), ui.b) > 0.35) {
        maskTex[id.xy] = 1.0;
        mvTex[id.xy] = float2(0, 0);
    }
}
