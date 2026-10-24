RaytracingAccelerationStructure AS : register(t0);
StructuredBuffer<float4> Origins : register(t1);
RWStructuredBuffer<float> Buf : register(u0);

cbuffer Constants : register(b0)
{
	uint flags;
};

struct Payload
{
	float hits;
};

/* Ray N can only intersect triangle N % 8. Each step adds its value, so the result also
 * counts non-opaque candidates, and a wrong primitive adds 64. */
float check_primitive(uint primitive, uint thr, float value)
{
	return primitive == thr % 8 ? value : 64.0;
}

void MissShader(inout Payload payload)
{
	payload.hits += 0.125;
}

[numthreads(64, 1, 1)]
void main(uint thr : SV_DispatchThreadID)
{
	Payload payload;
	payload.hits = 0.0;

	float4 origin = Origins[thr];

	RayDesc ray;
	ray.Origin = origin.xyz;
	ray.Direction = float3(0.0, 0.0, 1.0);
	ray.TMin = 0;
	ray.TMax = origin.w;

	RayQuery<RAY_FLAG_NONE, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> rq;
	rq.TraceRayInline(AS, flags, 0x01, ray);

	// Register the anyhit, but don't commit it as closest hit.
	while (rq.Proceed())
		if (rq.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
			payload.hits += check_primitive(rq.CandidatePrimitiveIndex(), thr, 0.75);

	if (rq.CommittedStatus() == COMMITTED_NOTHING)
		MissShader(payload);
	else if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		payload.hits += check_primitive(rq.CommittedPrimitiveIndex(), thr, 1.0);
	else
		payload.hits += 64.0;

	Buf[thr] = payload.hits;
}
