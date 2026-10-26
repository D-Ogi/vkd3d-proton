typedef float16_t V0;
StructuredBuffer<V0> I0 : register(t0);
RWStructuredBuffer<V0> O0 : register(u0);
typedef vector<float16_t, 3> V1;
StructuredBuffer<V1> I1 : register(t1);
RWStructuredBuffer<V1> O1 : register(u1);
typedef vector<float16_t, 5> V2;
StructuredBuffer<V2> I2 : register(t2);
RWStructuredBuffer<V2> O2 : register(u2);
typedef vector<float16_t, 7> V3;
StructuredBuffer<V3> I3 : register(t3);
RWStructuredBuffer<V3> O3 : register(u3);
typedef vector<float16_t, 16> V4;
StructuredBuffer<V4> I4 : register(t4);
RWStructuredBuffer<V4> O4 : register(u4);
ByteAddressBuffer IB : register(t5);
RWByteAddressBuffer OB : register(u5);
[numthreads(6,1,1)]
void main(uint thr : SV_DispatchThreadID)
{
    O0[thr] = I0[thr] + V0(1);
    OB.Store<V0>(2 * (thr + 0), IB.Load<V0>(2 * thr) + V0(1));
    O1[thr] = I1[thr] + V1(1,2,3);
    OB.Store<V1>(6 * (thr + 8), IB.Load<V1>(6 * thr) + V1(1,2,3));
    O2[thr] = I2[thr] + V2(1,2,3,4,5);
    OB.Store<V2>(10 * (thr + 16), IB.Load<V2>(10 * thr) + V2(1,2,3,4,5));
    O3[thr] = I3[thr] + V3(1,2,3,4,5,6,7);
    OB.Store<V3>(14 * (thr + 24), IB.Load<V3>(14 * thr) + V3(1,2,3,4,5,6,7));
    O4[thr] = I4[thr] + V4(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16);
    OB.Store<V4>(32 * (thr + 32), IB.Load<V4>(32 * thr) + V4(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16));
    if (thr == 0) {
        uint count, stride;
        I0.GetDimensions(count, stride); OB.Store<uint>(2000, count); OB.Store<uint>(2004, stride);
        O0.GetDimensions(count, stride); OB.Store<uint>(2008, count); OB.Store<uint>(2012, stride);
    }
}
