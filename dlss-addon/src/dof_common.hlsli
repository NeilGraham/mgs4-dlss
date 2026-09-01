// Depth of field re-applied after DLSS / DLSS 5 NR: an exact transcription of the port's own three DoF passes
// (pixel shaders bf2a546d733f4efc = circle of confusion, 9feb2d2e92bbc108 = spiral bokeh gather, 01978e62bca9c941 =
// blend of the blurred layer over the sharp image), run on the DLSS output with the game's constants for this frame.
// The circle of confusion is evaluated at the game's own CoC draw (dof_coc_cs, depth -> CoC, while the depth copy is
// exactly what the game's pass would have sampled); the colour is added at the DLSS insertion (dof_pack_cs), then the
// gather and the composite run on the DLSS output.
cbuffer CB : register(b0)
{
    float4 c[10];        // the game's constants at its CoC pass, rows cb0[8..17] (c[i] = cb0[8+i])
    float4 g[10];        // the same rows at its spiral gather pass (each draw carries its own constant buffer)
    float2 halfSize;     // CoC / blur textures (half resolution)
    float2 fullSize;     // DLSS output
    float2 depthScale;   // dynamic resolution: the depth copy holds the scene in its top-left sub-rect (scale <= 1)
    float2 depthSize;    // the game's depth copy texture
    float2 stepUV;       // spiral step per radius unit, in UV of the half-resolution image
    float  cocUnorm;     // 1 = the game stored the CoC in an 8-bit UNORM alpha (negative = near blur was lost) - mirror that
    float  radiusScale;  // tuning multiplier on the CoC (1 = the game's)
    float2 depthOff;     // the CoC pass's c12.xy: depth uv = (2 * pixel + depthOff) / depthSize
    float  debugView;    // 0 = normal, 1 = blurred layer only, 2 = coverage (grey), 3 = overlay mask
    float  hasMask;      // t1 of the composite = the overlay mask layer (title cards / captions drawn after the game DoF stay sharp)
    float2 depthJitter;  // this frame's camera jitter in depth texels (the depth copy is jittered, the DLSS output is not)
    float2 maskScale;    // the overlay mask was drawn at the game's (sub-rect) viewport: mask texel = pixel * maskScale
};
SamplerState linClamp : register(s0);
