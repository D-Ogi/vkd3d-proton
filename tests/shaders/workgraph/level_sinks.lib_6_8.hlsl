RWStructuredBuffer<uint> RWBuf : register(u0);

// Three levels: Entry (level 0) sends 8 Mid records, each Mid record launches 8 thread groups (level 1), and each of
// these groups allocates 64 Leaf records of 256 bytes (16 KiB). The Leaf records are the outputs of level 1, so they
// go to the other payload ring than the outputs of level 0.
struct MidRecord
{
	uint id;
};

struct LeafRecord
{
	uint id;
	uint magic; // LEAF_MAGIC, so that a test can find Leaf records in the payload sinks.
	uint words[61];
	uint check;
};

static const uint LEAF_MAGIC = 0x51ab51abu;

uint pattern(uint id, uint i)
{
	return id * 0x9e3779b1u + (i + 1u) * 0x85ebca77u;
}

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(8, 1, 1)]
[NumThreads(1, 1, 1)]
void Entry(DispatchNodeInputRecord<MidRecord> unused, uint gid : SV_GroupID, [MaxRecords(1)] NodeOutput<MidRecord> Mid)
{
	GroupNodeOutputRecords<MidRecord> m = Mid.GetGroupNodeOutputRecords(1);
	m.Get().id = gid;
	m.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(8, 1, 1)]
[NumThreads(64, 1, 1)]
void Mid(DispatchNodeInputRecord<MidRecord> input, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(64)] NodeOutput<LeafRecord> Leaf)
{
	GroupNodeOutputRecords<LeafRecord> l = Leaf.GetGroupNodeOutputRecords(64);
	uint id = (input.Get().id * 8 + gid) * 64 + tid;
	l[tid].id = id;
	l[tid].magic = LEAF_MAGIC;
	for (uint w = 0; w < 61; w++)
		l[tid].words[w] = pattern(id, w);
	l[tid].check = pattern(id, 61);
	l.OutputComplete();
}

// RWBuf[0] counts the Leaf records that arrive, RWBuf[2] records with wrong contents and RWBuf[3] records that
// arrive more than once. RWBuf[16 + id / 32] has one bit for each record id.
[Shader("node")]
[NodeLaunch("thread")]
void Leaf(ThreadNodeInputRecord<LeafRecord> input)
{
	LeafRecord r = input.Get();
	bool good = r.magic == LEAF_MAGIC && r.check == pattern(r.id, 61) && r.id < 4096u;
	uint o;

	for (uint w = 0; w < 61; w++)
		good = good && r.words[w] == pattern(r.id, w);
	if (!good)
	{
		InterlockedAdd(RWBuf[2], 1, o);
		return;
	}
	uint bit = 1u << (r.id & 31u);
	InterlockedOr(RWBuf[16 + r.id / 32], bit, o);
	if (o & bit)
		InterlockedAdd(RWBuf[3], 1, o);
	InterlockedAdd(RWBuf[0], 1, o);
}
