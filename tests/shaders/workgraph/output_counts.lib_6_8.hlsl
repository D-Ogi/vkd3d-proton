RWStructuredBuffer<uint> RWBuf : register(u0);

// The input record of the entry point: the thread groups that add empty records (the first groups of the grid), the
// count that each of them adds with GroupIncrementOutputCount(), and the count that each of their threads adds with
// ThreadIncrementOutputCount(). Empty records do not count against the MaxRecords limit of a thread group
// (WorkGraphs.md, Node output limits), so the declared counts are large.
struct Params
{
	uint groups;
	uint group_count;
	uint thread_count;
};

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(64, 1, 1)]
[NumThreads(64, 1, 1)]
void Producer(DispatchNodeInputRecord<Params> params, uint gid : SV_GroupID,
		[MaxRecords(2097152)] EmptyNodeOutput GroupChild,
		[MaxRecords(2097152)] EmptyNodeOutput ThreadChild)
{
	if (gid < params.Get().groups)
	{
		GroupChild.GroupIncrementOutputCount(params.Get().group_count);
		ThreadChild.ThreadIncrementOutputCount(params.Get().thread_count);
	}
}

// RWBuf[0] counts the launches of GroupChild and RWBuf[1] the launches of ThreadChild.
void count_launch(uint index)
{
	uint launches = WaveActiveCountBits(true);
	uint o;

	if (WaveIsFirstLane())
		InterlockedAdd(RWBuf[index], launches, o);
}

[Shader("node")]
[NodeLaunch("thread")]
void GroupChild()
{
	count_launch(0);
}

[Shader("node")]
[NodeLaunch("thread")]
void ThreadChild()
{
	count_launch(1);
}
