RWStructuredBuffer<uint> RWBuf : register(u0);

struct Payload
{
	uint depth;
};

// N[1] is the only element of the node array N. It recurses through element 1 of its own output array; element 0 is
// a hole. RWBuf[depth] gets GetRemainingRecursionLevels() + 1, so depth 0 adds 2 and depth 1 adds 1.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
[NodeID("N", 1)]
[NodeMaxRecursionDepth(1)]
void N1(ThreadNodeInputRecord<Payload> p,
		[MaxRecords(1)] [NodeID("N")] [NodeArraySize(2)] [AllowSparseNodes] NodeOutputArray<Payload> outputs)
{
	uint o;
	InterlockedAdd(RWBuf[p.Get().depth], GetRemainingRecursionLevels() + 1, o);

	if (GetRemainingRecursionLevels() != 0)
	{
		ThreadNodeOutputRecords<Payload> r = outputs[1].GetThreadNodeOutputRecords(1);
		r.Get().depth = p.Get().depth + 1;
		r.OutputComplete();
	}
}
