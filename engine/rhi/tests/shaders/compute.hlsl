// Compute pipeline test: Out[i] = 3 * i + 1.
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> Out;

[numthreads(64, 1, 1)] void cs_main(uint3 id : SV_DispatchThreadID) { Out[id.x] = id.x * 3u + 1u; }
