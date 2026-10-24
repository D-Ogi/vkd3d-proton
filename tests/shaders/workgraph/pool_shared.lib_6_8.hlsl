RWStructuredBuffer<uint> RWBuf : register(u0);

// Shared output budgets for test_workgraph_drain_layout, which does not dispatch the graph. Wide (64-byte records)
// shares the budget of 16 records of Tiny (4-byte records). Big (160-byte records) shares the budget of 32 records
// of Empty, an empty node, which has no payload.
struct TinyRecord
{
	uint id;
};

struct WideRecord
{
	uint words[16];
};

struct BigRecord
{
	uint words[40];
};

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void Producer([MaxRecords(16)] NodeOutput<TinyRecord> Tiny,
		[MaxRecordsSharedWith(Tiny)] NodeOutput<WideRecord> Wide,
		[MaxRecords(32)] EmptyNodeOutput Empty,
		[MaxRecordsSharedWith(Empty)] NodeOutput<BigRecord> Big)
{
	GroupNodeOutputRecords<TinyRecord> t = Tiny.GetGroupNodeOutputRecords(1);
	t.Get().id = 1;
	t.OutputComplete();
	GroupNodeOutputRecords<WideRecord> w = Wide.GetGroupNodeOutputRecords(1);
	w.Get().words[0] = 2;
	w.OutputComplete();
	Empty.GroupIncrementOutputCount(1);
	GroupNodeOutputRecords<BigRecord> b = Big.GetGroupNodeOutputRecords(1);
	b.Get().words[0] = 3;
	b.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Tiny(ThreadNodeInputRecord<TinyRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[0], input.Get().id, o);
}

[Shader("node")]
[NodeLaunch("thread")]
void Wide(ThreadNodeInputRecord<WideRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], input.Get().words[0], o);
}

[Shader("node")]
[NodeLaunch("thread")]
void Empty()
{
	uint o;
	InterlockedAdd(RWBuf[2], 1, o);
}

[Shader("node")]
[NodeLaunch("thread")]
void Big(ThreadNodeInputRecord<BigRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[3], input.Get().words[0], o);
}
