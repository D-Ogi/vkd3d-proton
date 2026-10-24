RWStructuredBuffer<uint> RWBuf : register(u0);

// The input record of both entry points: the record count of each allocation, and a record index that thread 0
// writes in addition to the records of the count (0: none). An index at or past the count is not valid: such a write
// must stay in the payload memory of the work graph and its sinks.
struct Params
{
	uint count;
	uint bad_index;
};

struct SmallRecord
{
	uint id;
};

// 256 bytes.
struct LargeRecord
{
	uint id;
	uint words[63];
};

// RWBuf[0] counts the small records that arrive, RWBuf[1] the large records, RWBuf[2] records with an id that is not
// valid, and RWBuf[3] records that arrive more than once. RWBuf[16 + id / 32] has one bit for each small record id.
void mark(uint id)
{
	uint o;
	if (id >= 64u * 256u)
	{
		InterlockedAdd(RWBuf[2], 1, o);
		return;
	}
	uint bit = 1u << (id & 31u);
	InterlockedOr(RWBuf[16 + id / 32], bit, o);
	if (o & bit)
		InterlockedAdd(RWBuf[3], 1, o);
	InterlockedAdd(RWBuf[0], 1, o);
}

// 64 thread groups of 64 threads. Each group allocates count 4-byte records with GetGroupNodeOutputRecords() and writes
// the first min(count, 256) of them, with the id group * 256 + index.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(64, 1, 1)]
[NumThreads(64, 1, 1)]
void GroupProducer(DispatchNodeInputRecord<Params> params, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(256)] NodeOutput<SmallRecord> SmallChild)
{
	uint count = params.Get().count;
	uint bad_index = params.Get().bad_index;
	uint written = min(count, 256u);
	uint i;

	GroupNodeOutputRecords<SmallRecord> g = SmallChild.GetGroupNodeOutputRecords(count);
	for (i = tid; i < written; i += 64)
		g[i].id = gid * 256 + i;
	if (bad_index != 0 && tid == 0)
		g[bad_index].id = 0xbad00000u | gid;
	g.OutputComplete();
}

// One thread group of 64 threads. Each thread allocates count 256-byte records with GetThreadNodeOutputRecords() (one
// allocation per wave) and writes the first. Thread 63 also writes record bad_index: with the lane offset of the
// wave, this is the write that ends farthest after the allocation.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
void ThreadProducer(DispatchNodeInputRecord<Params> params, uint tid : SV_GroupIndex,
		[MaxRecords(64)] NodeOutput<LargeRecord> LargeChild)
{
	uint count = params.Get().count;
	uint bad_index = params.Get().bad_index;
	uint w;

	ThreadNodeOutputRecords<LargeRecord> t = LargeChild.GetThreadNodeOutputRecords(count);
	if (count != 0)
	{
		t[0].id = tid;
		for (w = 0; w < 63; w++)
			t[0].words[w] = tid;
	}
	if (bad_index != 0 && tid == 63)
	{
		t[bad_index].id = 0xbad00000u;
		for (w = 0; w < 63; w++)
			t[bad_index].words[w] = 0xbad00000u;
	}
	t.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void SmallChild(ThreadNodeInputRecord<SmallRecord> input)
{
	mark(input.Get().id);
}

[Shader("node")]
[NodeLaunch("thread")]
void LargeChild(ThreadNodeInputRecord<LargeRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}
