RWStructuredBuffer<uint> RWBuf : register(u0);

// The charge of a group that is not full (test_workgraph_drain_retention). Entry (grid 1) writes 17 records into A and
// 17 into B. A and B (coalescing, 4 records a group, 128 threads) each write 128 Sink records of 256 bytes for each
// group, whatever the input count: a group takes the whole W of pool 2 (32 KiB, more than half the slack that the
// alignment of Min can give), and a group with 1 input record takes as much as a group with 4. The test
// runs the graph at a backing size at which pool 2 holds 2 groups but not 3. A charge of k / u groups for a group that
// is not full lets 3 groups into the pool, and the Sink records of the third group are lost. The records of A and B
// are 8 bytes: a block of an odd count of them is not a multiple of 16 bytes.
// RWBuf[0] counts the Sink records of input records, RWBuf[1] the Sink records that fill a group, RWBuf[2] records
// with wrong contents, RWBuf[3] records that arrive more than once. RWBuf[8] has one bit for each record id of A (17
// ids), RWBuf[9] one bit for each record id of B (17 ids).
static const uint FILL = 0xffffu;

struct Item
{
	uint id;
	uint check;
};

struct SinkRecord
{
	uint id;
	uint check;
	uint words[62];
};

uint pattern(uint id)
{
	return id * 0x9e3779b1u + 0x85ebca77u;
}

void bad_record()
{
	uint o;
	InterlockedAdd(RWBuf[2], 1, o);
}

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(32, 1, 1)]
void Entry(uint tid : SV_GroupIndex, [MaxRecords(17)] NodeOutput<Item> A, [MaxRecords(17)] NodeOutput<Item> B)
{
	GroupNodeOutputRecords<Item> a = A.GetGroupNodeOutputRecords(17);
	if (tid < 17)
	{
		a[tid].id = tid;
		a[tid].check = pattern(tid);
	}
	a.OutputComplete();

	GroupNodeOutputRecords<Item> b = B.GetGroupNodeOutputRecords(17);
	if (tid < 17)
	{
		b[tid].id = 32 + tid;
		b[tid].check = pattern(32 + tid);
	}
	b.OutputComplete();
}

[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(128, 1, 1)]
void A([MaxRecords(4)] GroupNodeInputRecords<Item> input, uint tid : SV_GroupIndex,
		[MaxRecords(128)] NodeOutput<SinkRecord> Sink)
{
	uint count = input.Count();
	GroupNodeOutputRecords<SinkRecord> s = Sink.GetGroupNodeOutputRecords(128);
	if (tid < count)
	{
		Item r = input[tid];
		if (r.id >= 17u || r.check != pattern(r.id))
			bad_record();
		s[tid].id = r.id;
		s[tid].check = r.check;
	}
	else
	{
		s[tid].id = FILL;
		s[tid].check = 0;
	}
	s.OutputComplete();
}

[Shader("node")]
[NodeLaunch("coalescing")]
[NumThreads(128, 1, 1)]
void B([MaxRecords(4)] GroupNodeInputRecords<Item> input, uint tid : SV_GroupIndex,
		[MaxRecords(128)] NodeOutput<SinkRecord> Sink)
{
	uint count = input.Count();
	GroupNodeOutputRecords<SinkRecord> s = Sink.GetGroupNodeOutputRecords(128);
	if (tid < count)
	{
		Item r = input[tid];
		if (r.id < 32u || r.id >= 49u || r.check != pattern(r.id))
			bad_record();
		s[tid].id = r.id;
		s[tid].check = r.check;
	}
	else
	{
		s[tid].id = FILL;
		s[tid].check = 0;
	}
	s.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Sink(ThreadNodeInputRecord<SinkRecord> input)
{
	SinkRecord r = input.Get();
	uint o, b;

	if (r.id == FILL)
	{
		InterlockedAdd(RWBuf[1], 1, o);
		return;
	}
	if (r.id >= 64u || r.check != pattern(r.id))
	{
		bad_record();
		return;
	}
	b = 1u << (r.id & 31u);
	InterlockedOr(RWBuf[8 + r.id / 32], b, o);
	if (o & b)
		InterlockedAdd(RWBuf[3], 1, o);
	InterlockedAdd(RWBuf[0], 1, o);
}
