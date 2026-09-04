#pragma once
#include "copy_common.hlsli"

Texture2DMS<float, SAMPLE_COUNT> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

float main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Depth
{
    Texture2DMS<float, SAMPLE_COUNT> tex =
        g_Texture2DMSDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h, samples;
    tex.GetDimensions(w, h, samples);
    int2 coord = min(int2(texCoord * float2(w, h)), int2(w - 1, h - 1));
    // min() across samples = nearest-surface depth (matches UR's depth resolve
    // and is correct for the posteff DOF/fog that samples this). No scale.
    float result = tex.Load(coord, 0);
    [unroll] for (int i = 1; i < SAMPLE_COUNT; i++)
        result = min(result, tex.Load(coord, i));
    return result;
}
