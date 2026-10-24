RWStructuredBuffer<uint> RWBuf : register(u0);

// One record with a large demand (test_workgraph_drain_retention). Entry (grid 1) writes one record into Wide. WideTap
// shares the input of Wide. Wide and WideTap (grid 512 each) each write 16 records of 16 bytes for each group: one
// group needs 256 bytes, one reader 128 KiB, and the record 256 KiB of the next pool. RWBuf[0] counts the Leaf records,
// RWBuf[1] the Leaf2 records, RWBuf[2] records with wrong contents.
struct WideRecord
{
	uint id;
};

struct LeafRecord
{
	uint id;
	uint group;
	uint thread;
	uint check;
};

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void Entry([MaxRecords(1)] [NodeID("Wide")] NodeOutput<WideRecord> output)
{
	GroupNodeOutputRecords<WideRecord> r = output.GetGroupNodeOutputRecords(1);
	r.Get().id = 7;
	r.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(512, 1, 1)]
[NumThreads(16, 1, 1)]
void Wide(DispatchNodeInputRecord<WideRecord> input, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(16)] NodeOutput<LeafRecord> Leaf)
{
	GroupNodeOutputRecords<LeafRecord> l = Leaf.GetGroupNodeOutputRecords(16);
	l[tid].id = input.Get().id;
	l[tid].group = gid;
	l[tid].thread = tid;
	l[tid].check = input.Get().id ^ (gid * 16 + tid);
	l.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(512, 1, 1)]
[NumThreads(16, 1, 1)]
[NodeShareInputOf("Wide")]
void WideTap(DispatchNodeInputRecord<WideRecord> input, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(16)] NodeOutput<LeafRecord> Leaf2)
{
	GroupNodeOutputRecords<LeafRecord> l = Leaf2.GetGroupNodeOutputRecords(16);
	l[tid].id = input.Get().id;
	l[tid].group = gid;
	l[tid].thread = tid;
	l[tid].check = input.Get().id ^ (gid * 16 + tid);
	l.OutputComplete();
}

void count(uint index, LeafRecord r)
{
	uint o;
	if (r.id != 7 || r.check != (r.id ^ (r.group * 16 + r.thread)))
		InterlockedAdd(RWBuf[2], 1, o);
	else
		InterlockedAdd(RWBuf[index], 1, o);
}

[Shader("node")]
[NodeLaunch("thread")]
void Leaf(ThreadNodeInputRecord<LeafRecord> input)
{
	count(0, input.Get());
}

[Shader("node")]
[NodeLaunch("thread")]
void Leaf2(ThreadNodeInputRecord<LeafRecord> input)
{
	count(1, input.Get());
}
