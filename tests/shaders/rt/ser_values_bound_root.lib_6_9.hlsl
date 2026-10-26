struct [raypayload] Payload { uint value : read(miss, closesthit, caller) : write(miss, closesthit, caller); };
RWStructuredBuffer<uint> Out : register(u0);
RaytracingAccelerationStructure Scene : register(t0);
cbuffer Params : register(b0) { uint Choice; uint Index; uint Seed; uint Padding; };
[shader("raygeneration")] void BoundRoot() {
    RayDesc r;
    r.Origin = float3(0,0,-1); r.Direction = float3(0,0,1); r.TMin = 0; r.TMax = 100;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(Scene, 0, 255, r); while (q.Proceed()) {}
    dx::HitObject h = dx::HitObject::FromRayQuery(q);
    h.SetShaderTableIndex(0);
    Payload p = {Seed};
    dx::HitObject::Invoke(h, p);
    Out[0] = h.LoadLocalRootTableConstant(0); Out[1] = p.value; Out[2] = h.IsHit();
}
