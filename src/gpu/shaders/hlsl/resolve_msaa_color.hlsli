#pragma once
#include "copy_common.hlsli"

// Multisampled source bound at the same bindless slot/space as the non-MS
// heap. plume creates a Texture2DMS SRV automatically when the source
// texture's sampleCount > 1. Declaring the heap as Texture2DMS here reads
// that descriptor correctly (mirrors UR's g_Texture2DMSDescriptorHeap).
Texture2DMS<float4, SAMPLE_COUNT> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2DMS<float4, SAMPLE_COUNT> tex =
        g_Texture2DMSDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h, samples;
    tex.GetDimensions(w, h, samples);
    int2 coord = min(int2(texCoord * float2(w, h)), int2(w - 1, h - 1));
    float4 result = tex.Load(coord, 0);
    [unroll] for (int i = 1; i < SAMPLE_COUNT; i++)
        result += tex.Load(coord, i);
    result /= SAMPLE_COUNT;
    // Param0 = the X360 resolve exponent-bias scale (2^bias). Must be applied
    // exactly as copy_color_ps does, or BD's HDR scene (bias=-2, x0.25) stays
    // 4x too bright and the posteff blows the frame white. Force alpha=1.
    return float4(result.rgb * g_PushConstants.Param0, 1.0);
}
