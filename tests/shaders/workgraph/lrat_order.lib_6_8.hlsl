RWStructuredBuffer<uint> RWBuf : register(u0);
cbuffer Local : register(b0) { uint Value; };

LocalRootSignature LRS = { "RootConstants(num32BitConstants = 1, b0)" };
SubobjectToExportsAssociation assoc = { "LRS", "A;B;C" };

// A, B and C read their local root constant. C has the explicit table index 0, so the default indices of A and B
// are 1 and 2, in the order in which the nodes are added to the graph.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
void EntryNode([MaxRecords(1)] EmptyNodeOutput A, [MaxRecords(1)] EmptyNodeOutput B, [MaxRecords(1)] EmptyNodeOutput C)
{
	A.ThreadIncrementOutputCount(1);
	B.ThreadIncrementOutputCount(1);
	C.ThreadIncrementOutputCount(1);
}

[Shader("node")]
[NodeLaunch("thread")]
void A()
{
	uint o;
	InterlockedAdd(RWBuf[0], Value, o);
}

[Shader("node")]
[NodeLaunch("thread")]
void B()
{
	uint o;
	InterlockedAdd(RWBuf[1], Value, o);
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeLocalRootArgumentsTableIndex(0)]
void C()
{
	uint o;
	InterlockedAdd(RWBuf[2], Value, o);
}
