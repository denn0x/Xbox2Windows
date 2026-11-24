ByteAddressBuffer vertices : register(t0, space3);

struct Varyings {
    float4 position : SV_Position;
    nointerpolation float4 color : COLOR0;
};

Varyings VSMain(uint vertex : SV_VertexID)
{
    uint record = 16 + vertex * vertices.Load(0);
    Varyings result;
    result.position = float4(asfloat(vertices.Load2(record)), 0, 1);
    result.color = asfloat(vertices.Load4(record + 8));
    return result;
}

float4 PSMain(Varyings input) : SV_Target
{
    return input.color;
}
