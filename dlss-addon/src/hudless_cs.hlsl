// Builds the HUD-less color for frame generation in composite mode: the DLAA output (already copied into the
// target) keeps its anti-aliased pixels everywhere except where the replayed UI layer has content; there the
// pre-HUD capture of the game's final texture (post-processed scene, before the HUD was drawn) is used instead.
Texture2D<float4>   preHud : register(t0);
Texture2D<float4>   ui     : register(t1);
RWTexture2D<float4> img    : register(u0);
RWTexture2D<float>  dummy  : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; img.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 u = ui.Load(int3(id.xy, 0));
    if (max(u.a, max(u.r, max(u.g, u.b))) > 0.004)
        img[id.xy] = preHud.Load(int3(id.xy, 0));
}
