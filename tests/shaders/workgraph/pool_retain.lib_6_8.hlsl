RWStructuredBuffer<uint> RWBuf : register(u0);

// Retention of whole records across the rounds of the draining scheduler (test_workgraph_drain_retention). Four
// levels:
//   level 0: Entry (grid 4) writes one Mid record of 256 bytes for each group;
//   level 1: Mid (grid 8, 32 threads) writes 32 Leaf records of 256 bytes and one empty record for Done for each group,
//     and groups 0 and 1 write one Side record each. Tap shares the input of Mid (NodeShareInputOf) and writes one Fan
//     record;
//   level 2: Leaf (thread launch, no outputs), Done (coalescing empty input, 8 records a group) writes one Join record
//     for each empty record, Side (broadcasting, grid 64, 2 threads) writes 2 Join records for each group, Fan
//     (coalescing, 4 records a group) writes one Join record for each input record;
//   level 3: Join (thread launch, no outputs).
// Side and Fan are the two paths of a diamond into Join. A Mid record is a record of two readers. Pool 3 holds empty
// and nonempty records of the slots of level 2. One Side record has a demand of 128 Join records on pool 3, 16 times W
// of pool 3, so pool 3 sets how many Side records a round admits, and level 2 keeps records across rounds.
// RWBuf[0] counts the Leaf records, RWBuf[1] the Join records, RWBuf[2] records with wrong contents, RWBuf[3]
// records that arrive more than once, RWBuf[4] the empty records of Done, RWBuf[5] the Side records. RWBuf[8 + i] has
// one bit for each Leaf record id (1024 ids), RWBuf[40] one bit for each Fan id (4), RWBuf[41] counts the Join
// records from Done, RWBuf[64 + i] has one bit for each Join id from Side (1024 ids).
struct MidRecord
{
	uint id;
	uint words[62];
	uint check;
};

struct LeafRecord
{
	uint id;
	uint words[62];
	uint check;
};

struct SideRecord
{
	uint id;
	uint check;
};

struct FanRecord
{
	uint id;
	uint check;
};

struct JoinRecord
{
	uint id;
	uint src; // 0 Side, 1 Fan, 2 Done.
};

uint pattern(uint id, uint i)
{
	return id * 0x9e3779b1u + (i + 1u) * 0x85ebca77u;
}

void bad_record()
{
	uint o;
	InterlockedAdd(RWBuf[2], 1, o);
}

void mark(uint word, uint bit)
{
	uint o, b = 1u << bit;
	InterlockedOr(RWBuf[word], b, o);
	if (o & b)
		InterlockedAdd(RWBuf[3], 1, o);
}

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(4, 1, 1)]
[NumThreads(1, 1, 1)]
void Entry(uint gid : SV_GroupID, [MaxRecords(1)] NodeOutput<MidRecord> Mid)
{
	GroupNodeOutputRecords<MidRecord> m = Mid.GetGroupNodeOutputRecords(1);
	m.Get().id = gid;
	for (uint w = 0; w < 62; w++)
		m.Get().words[w] = pattern(gid, w);
	m.Get().check = pattern(gid, 62);
	m.OutputComplete();
}

bool good_mid(MidRecord r)
{
	bool good = r.id < 4u && r.check == pattern(r.id, 62);
	for (uint w = 0; w < 62; w++)
		good = good && r.words[w] == pattern(r.id, w);
	return good;
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(8, 1, 1)]
[NumThreads(32, 1, 1)]
void Mid(DispatchNodeInputRecord<MidRecord> input, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(32)] NodeOutput<LeafRecord> Leaf, [MaxRecords(1)] EmptyNodeOutput Done,
		[MaxRecords(1)] NodeOutput<SideRecord> Side)
{
	MidRecord r = input.Get();
	bool good = good_mid(r);
	if (!good && tid == 0)
		bad_record();

	GroupNodeOutputRecords<LeafRecord> l = Leaf.GetGroupNodeOutputRecords(32);
	uint id = (r.id * 8 + gid) * 32 + tid;
	l[tid].id = id;
	for (uint w = 0; w < 62; w++)
		l[tid].words[w] = pattern(id, w);
	l[tid].check = pattern(id, 62);
	l.OutputComplete();

	Done.GroupIncrementOutputCount(1);

	GroupNodeOutputRecords<SideRecord> s = Side.GetGroupNodeOutputRecords(gid < 2 ? 1 : 0);
	if (gid < 2 && tid == 0)
	{
		s.Get().id = r.id * 2 + gid;
		s.Get().check = pattern(r.id * 2 + gid, 0);
	}
	s.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeShareInputOf("Mid")]
void Tap(DispatchNodeInputRecord<MidRecord> input, [MaxRecords(1)] NodeOutput<FanRecord> Fan)
{
	MidRecord r = input.Get();
	if (!good_mid(r))
		bad_record();
	GroupNodeOutputRecords<FanRecord> f = Fan.GetGroupNodeOutputRecords(1);
	f.Get().id = r.id;
	f.Get().check = pattern(r.id, 1);
	f.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Leaf(ThreadNodeInputRecord<LeafRecord> input)
{
	LeafRecord r = input.Get();
	bool good = r.id < 1024u && r.check == pattern(r.id, 62);
	uint o;

	for (uint w = 0; w < 62; w++)
		good = good && r.words[w] == pattern(r.id, w);
	if (!good)
	{
		bad_record();
		return;
	}
	mark(8 + r.id / 32, r.id & 31u);
	InterlockedAdd(RWBuf[0], 1, o);
}

[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(8, 1, 1)]
void Done([MaxRecords(8)] EmptyNodeInput input, uint tid : SV_GroupIndex, [MaxRecords(8)] NodeOutput<JoinRecord> Join)
{
	uint count = input.Count(), o;
	GroupNodeOutputRecords<JoinRecord> j = Join.GetGroupNodeOutputRecords(count);
	if (tid < count)
	{
		j[tid].id = 0;
		j[tid].src = 2;
	}
	j.OutputComplete();
	if (tid == 0)
		InterlockedAdd(RWBuf[4], count, o);
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(64, 1, 1)]
[NumThreads(2, 1, 1)]
void Side(DispatchNodeInputRecord<SideRecord> input, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(2)] NodeOutput<JoinRecord> Join)
{
	SideRecord r = input.Get();
	uint o;

	if (tid == 0 && (r.id >= 8u || r.check != pattern(r.id, 0)))
		bad_record();
	if (tid == 0 && gid == 0)
		InterlockedAdd(RWBuf[5], 1, o);
	GroupNodeOutputRecords<JoinRecord> j = Join.GetGroupNodeOutputRecords(2);
	j[tid].id = r.id * 128 + gid * 2 + tid;
	j[tid].src = 0;
	j.OutputComplete();
}

[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(4, 1, 1)]
void Fan([MaxRecords(4)] GroupNodeInputRecords<FanRecord> input, uint tid : SV_GroupIndex,
		[MaxRecords(4)] NodeOutput<JoinRecord> Join)
{
	uint count = input.Count();
	GroupNodeOutputRecords<JoinRecord> j = Join.GetGroupNodeOutputRecords(count);
	if (tid < count)
	{
		FanRecord r = input[tid];
		if (r.id >= 4u || r.check != pattern(r.id, 1))
			bad_record();
		j[tid].id = r.id;
		j[tid].src = 1;
	}
	j.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Join(ThreadNodeInputRecord<JoinRecord> input)
{
	JoinRecord r = input.Get();
	uint o;

	if (r.src == 0 && r.id < 1024u)
		mark(64 + r.id / 32, r.id & 31u);
	else if (r.src == 1 && r.id < 4u)
		mark(40, r.id);
	else if (r.src == 2 && r.id == 0)
		InterlockedAdd(RWBuf[41], 1, o);
	else
	{
		bad_record();
		return;
	}
	InterlockedAdd(RWBuf[1], 1, o);
}
