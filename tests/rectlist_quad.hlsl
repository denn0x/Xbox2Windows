struct VSIn {
    float4 pos : POSITION;
    float4 col : COLOR;
};

struct VSOut {
    float4 pos : SV_Position;
    float4 col : COLOR;
};

VSOut VSMain(VSIn input) {
    VSOut output;
    output.pos = input.pos;
    output.col = input.col;
    return output;
}

float4 PSMain(VSOut input) : SV_Target {
    return input.col;
}
