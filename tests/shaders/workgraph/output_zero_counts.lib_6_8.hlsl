RWStructuredBuffer<uint> RWBuf : register(u0);

// The input record of both entry points: the number of allocations with the record count "zero" that each group or
// thread makes first, and the record count of the allocation that follows them. The counts come from the record, so
// that the compiler cannot remove the allocations. A count of 0 is valid and gives no records.
struct Params
{
	uint calls;
	uint zero;
	uint count;
};

struct SmallRecord
{
	uint id;
};

// RWBuf[0] counts the records that arrive, RWBuf[1] records with an id that is not valid, and RWBuf[2] records that
// arrive more than once. RWBuf[16 + id / 32] has one bit for each record id.
void mark(uint id)
{
	uint o;
	if (id >= 64u)
	{
		InterlockedAdd(RWBuf[1], 1, o);
		return;
	}
	uint bit = 1u << (id & 31u);
	InterlockedOr(RWBuf[16 + id / 32], bit, o);
	if (o & bit)
		InterlockedAdd(RWBuf[2], 1, o);
	InterlockedAdd(RWBuf[0], 1, o);
}

// One thread group of 64 threads. The group makes "calls" allocations of "zero" records with
// GetGroupNodeOutputRecords(), then one allocation of "count" records, and writes the records of each allocation.
// The ids are the record index over all allocations of the group.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
void GroupProducer(DispatchNodeInputRecord<Params> params, uint tid : SV_GroupIndex,
		[MaxRecords(64)] NodeOutput<SmallRecord> Child)
{
	uint calls = params.Get().calls;
	uint zero = params.Get().zero;
	uint count = params.Get().count;
	uint c;

	for (c = 0; c < calls; c++)
	{
		GroupNodeOutputRecords<SmallRecord> z = Child.GetGroupNodeOutputRecords(zero);
		if (tid < zero)
			z[tid].id = c * zero + tid;
		z.OutputComplete();
	}
	GroupNodeOutputRecords<SmallRecord> g = Child.GetGroupNodeOutputRecords(count);
	if (tid < count)
		g[tid].id = calls * zero + tid;
	g.OutputComplete();
}

// One thread group of 64 threads. Each thread makes "calls" allocations of "zero" records with
// GetThreadNodeOutputRecords(), then one allocation of "count" records (0 or 1), and writes the record with the id of
// the thread. Only "zero" = 0 is used with this entry point.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
void ThreadProducer(DispatchNodeInputRecord<Params> params, uint tid : SV_GroupIndex,
		[MaxRecords(64)] NodeOutput<SmallRecord> Child)
{
	uint calls = params.Get().calls;
	uint zero = params.Get().zero;
	uint count = params.Get().count;
	uint c;

	for (c = 0; c < calls; c++)
	{
		ThreadNodeOutputRecords<SmallRecord> z = Child.GetThreadNodeOutputRecords(zero);
		z.OutputComplete();
	}
	ThreadNodeOutputRecords<SmallRecord> t = Child.GetThreadNodeOutputRecords(count);
	if (count != 0)
		t.Get().id = tid;
	t.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Child(ThreadNodeInputRecord<SmallRecord> input)
{
	mark(input.Get().id);
}
