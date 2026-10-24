RWStructuredBuffer<uint> RWBuf : register(u0);

struct Payload
{
	uint depth;
};

// The longest chain is EntryNode and Recursive at recursion depth 0 to 31: 33 nodes. The limit is 32
// (D3D12_WORK_GRAPHS_MAX_NODE_DEPTH, a recursive node counts by its declared depth).
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
void EntryNode([MaxRecords(1)] [NodeID("Recursive")] NodeOutput<Payload> p)
{
	ThreadNodeOutputRecords<Payload> outputs = p.GetThreadNodeOutputRecords(1);
	outputs[0].depth = 0;
	outputs.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeMaxRecursionDepth(31)]
void Recursive(ThreadNodeInputRecord<Payload> p,
		[MaxRecords(1)] [NodeID("Recursive")] NodeOutput<Payload> op)
{
	uint o;
	InterlockedAdd(RWBuf[p.Get().depth], 1, o);

	if (op.IsValid())
	{
		ThreadNodeOutputRecords<Payload> outputs = op.GetThreadNodeOutputRecords(1);
		outputs[0].depth = p.Get().depth + 1;
		outputs.OutputComplete();
	}
}
