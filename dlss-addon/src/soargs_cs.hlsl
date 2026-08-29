// Turns the stream-output buffer-filled-size counters into DrawInstanced arguments for the velocity pass.
ByteAddressBuffer   counters : register(t0);   // [0] bytes written to the current-position buffer, [64] previous
RWByteAddressBuffer args     : register(u0);   // D3D12_DRAW_ARGUMENTS

[numthreads(1, 1, 1)]
void main()
{
    uint a = counters.Load(0);
    uint b = counters.Load(64);
    uint n = min(a, b) / 16;
    args.Store4(0, uint4(n, 1, 0, 0));
}
