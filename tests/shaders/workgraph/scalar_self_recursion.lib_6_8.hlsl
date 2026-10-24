RWStructuredBuffer<uint> RWBuf : register(u0);

struct Payload
{
	uint depth;
};

// The same as sparse_self_recursion, with a scalar output to N[1].
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
[NodeID("N", 1)]
[NodeMaxRecursionDepth(1)]
void N1(ThreadNodeInputRecord<Payload> p, [MaxRecords(1)] [NodeID("N", 1)] NodeOutput<Payload> output)
{
	uint o;
	InterlockedAdd(RWBuf[p.Get().depth], GetRemainingRecursionLevels() + 1, o);

	if (GetRemainingRecursionLevels() != 0)
	{
		ThreadNodeOutputRecords<Payload> r = output.GetThreadNodeOutputRecords(1);
		r.Get().depth = p.Get().depth + 1;
		r.OutputComplete();
	}
}
