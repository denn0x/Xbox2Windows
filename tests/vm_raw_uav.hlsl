RWByteAddressBuffer window : register(u0);

cbuffer StoreRequest : register(b0)
{
    uint offset;
    uint lanes;
    uint2 padding;
    uint4 value;
};

[numthreads(1, 1, 1)]
void main()
{
    if (lanes == 4)
        window.Store4(offset, value);
    else
        window.Store(offset, value.x);
}
