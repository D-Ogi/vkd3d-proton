RWStructuredBuffer<uint> RWBuf : register(u0);

// The input record of the entry point: the records that each thread group allocates with
// GetGroupNodeOutputRecords(), and the records that each thread allocates with GetThreadNodeOutputRecords().
// A thread writes at most one record and a thread group at most 64, also when the counts are larger.
struct Params
{
	uint group_count;
	uint thread_count;
};

// 64 group records of 256 bytes are 16 KiB, one allocation of a thread group.
struct GroupRecord
{
	uint id;
	uint words[62];
	uint check;
};

struct ThreadRecord
{
	uint id;
	uint words[14];
	uint check;
};

uint pattern(uint id, uint i)
{
	return id * 0x9e3779b1u + (i + 1u) * 0x85ebca77u;
}

// RWBuf[0] and RWBuf[1] count the group and the thread records that arrive, RWBuf[2] counts records with wrong
// contents and RWBuf[3] records that arrive more than once. RWBuf[4] is the wave size of the producer.
// RWBuf[16 + kind * 128 + id / 32] has one bit for each record id.
void mark(uint kind, uint id, bool good)
{
	uint o;
	if (!good || id >= 4096u)
	{
		InterlockedAdd(RWBuf[2], 1, o);
		return;
	}
	uint bit = 1u << (id & 31u);
	InterlockedOr(RWBuf[16 + kind * 128 + id / 32], bit, o);
	if (o & bit)
		InterlockedAdd(RWBuf[3], 1, o);
	InterlockedAdd(RWBuf[kind], 1, o);
}

// 64 thread groups of 64 threads. Each thread group allocates its group records, then each thread its thread record.
// The ids are unique: group * 64 + record index, and group * 64 + thread index.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(64, 1, 1)]
[NumThreads(64, 1, 1)]
void Producer(DispatchNodeInputRecord<Params> params, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(64)] NodeOutput<GroupRecord> GroupChild,
		[MaxRecords(64)] NodeOutput<ThreadRecord> ThreadChild)
{
	uint group_count = params.Get().group_count;
	uint thread_count = params.Get().thread_count;
	uint written = min(group_count, 64u);
	uint i, w;

	GroupNodeOutputRecords<GroupRecord> g = GroupChild.GetGroupNodeOutputRecords(group_count);
	for (i = tid; i < written; i += 64)
	{
		uint id = gid * 64 + i;
		g[i].id = id;
		for (w = 0; w < 62; w++)
			g[i].words[w] = pattern(id, w);
		g[i].check = pattern(id, 62);
	}
	g.OutputComplete();

	ThreadNodeOutputRecords<ThreadRecord> t = ThreadChild.GetThreadNodeOutputRecords(thread_count);
	if (thread_count != 0)
	{
		uint id = gid * 64 + tid;
		t.Get().id = id;
		for (w = 0; w < 14; w++)
			t.Get().words[w] = pattern(id, w);
		t.Get().check = pattern(id, 14);
	}
	t.OutputComplete();

	if (gid == 0 && tid == 0)
		RWBuf[4] = WaveGetLaneCount();
}

[Shader("node")]
[NodeLaunch("thread")]
void GroupChild(ThreadNodeInputRecord<GroupRecord> input)
{
	GroupRecord r = input.Get();
	bool good = r.check == pattern(r.id, 62);
	for (uint w = 0; w < 62; w++)
		good = good && r.words[w] == pattern(r.id, w);
	mark(0, r.id, good);
}

[Shader("node")]
[NodeLaunch("thread")]
void ThreadChild(ThreadNodeInputRecord<ThreadRecord> input)
{
	ThreadRecord r = input.Get();
	bool good = r.check == pattern(r.id, 14);
	for (uint w = 0; w < 14; w++)
		good = good && r.words[w] == pattern(r.id, w);
	mark(1, r.id, good);
}
