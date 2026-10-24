RWStructuredBuffer<uint> RWBuf : register(u0);

// Two levels with empty records only, for test_workgraph_drain_layout, which does not dispatch the graph: the pool of
// level 1 has no payload, so its W is 0.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void Producer([MaxRecords(8)] EmptyNodeOutput Leaf)
{
	Leaf.GroupIncrementOutputCount(8);
}

[Shader("node")]
[NodeLaunch("thread")]
void Leaf()
{
	uint o;
	InterlockedAdd(RWBuf[0], 1, o);
}
