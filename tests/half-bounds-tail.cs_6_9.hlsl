// Nonzero byte offsets into the final component must stay valid for every
// in-bounds element. Accessing element NumElements must not touch the next one.
StructuredBuffer<vector<half, 5> > input5 : register(t0);
StructuredBuffer<vector<half, 7> > input7 : register(t2);
RWStructuredBuffer<vector<half, 5> > output5 : register(u0);
RWStructuredBuffer<vector<half, 7> > output7 : register(u2);
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    output5[id.x][4] = input5[id.x][4] + half(1);
    output7[id.x][6] = input7[id.x][6] + half(1);
}
