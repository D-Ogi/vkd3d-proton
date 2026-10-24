RWStructuredBuffer<uint> RWBuf : register(u0);

// The export order of the node shaders is different from the alphabetical order of the node IDs, so that a
// node order that follows the IDs is visible. Zeta and Omega have no writer. Lone has two writers (Mid and
// Omega). Arr is a sparse array with only Arr[3] and Arr[1], exported in that order.

[Shader("node")]
[NodeLaunch("thread")]
void Zeta(
	[MaxRecords(1)] [NodeID("Mid")] EmptyNodeOutput m,
	[MaxRecords(1)] [NodeID("Beta")] EmptyNodeOutput b,
	[MaxRecords(1)] [NodeArraySize(4)] [AllowSparseNodes] [NodeID("Arr")] EmptyNodeOutputArray a)
{
	m.ThreadIncrementOutputCount(1);
	b.ThreadIncrementOutputCount(1);
	a[1].ThreadIncrementOutputCount(1);
	a[3].ThreadIncrementOutputCount(1);
}

[Shader("node")]
[NodeLaunch("thread")]
void Alpha()
{
	uint o;
	InterlockedAdd(RWBuf[0], 1, o);
}

[Shader("node")]
[NodeLaunch("thread")]
void Omega(
	[MaxRecords(1)] [NodeID("Lone")] EmptyNodeOutput l,
	[MaxRecords(1)] [NodeID("Pi")] EmptyNodeOutput p)
{
	l.ThreadIncrementOutputCount(1);
	p.ThreadIncrementOutputCount(1);
}

[Shader("node")]
[NodeLaunch("thread")]
void Beta([MaxRecords(1)] [NodeID("Alpha")] EmptyNodeOutput a)
{
	a.ThreadIncrementOutputCount(1);
}

[Shader("node")]
[NodeLaunch("thread")]
void Mid(
	[MaxRecords(1)] [NodeID("Kid")] EmptyNodeOutput k,
	[MaxRecords(1)] [NodeID("Lone")] EmptyNodeOutput l)
{
	k.ThreadIncrementOutputCount(1);
	l.ThreadIncrementOutputCount(1);
}

[Shader("node")]
[NodeLaunch("thread")]
void Kid()
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeID("Arr", 3)]
void Arr3()
{
	uint o;
	InterlockedAdd(RWBuf[2], 1, o);
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeID("Arr", 1)]
void Arr1()
{
	uint o;
	InterlockedAdd(RWBuf[3], 1, o);
}

[Shader("node")]
[NodeLaunch("thread")]
void Lone()
{
	uint o;
	InterlockedAdd(RWBuf[4], 1, o);
}

[Shader("node")]
[NodeLaunch("thread")]
void Pi()
{
	uint o;
	InterlockedAdd(RWBuf[5], 1, o);
}
