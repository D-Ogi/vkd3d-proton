RWStructuredBuffer<uint> RWBuf : register(u0);

// A shared reader with more than 32768 records, for test_workgraph_shared_divider. The lab flavour of the amdgpu-wddm
// engine splits the workgroups of a node into a primary dispatch of 32768 groups and a secondary dispatch
// (AMDGPU_WDDM_WG_DIVIDER=1), so each node below has records in both dispatches. Entry (grid 514, 64 threads) writes
// 32868 records into A_Pad and 32868 records into Owner. Reader shares the input of Owner. The node slots are in the
// order of the node IDs: A_Pad, Entry, Owner, Reader. The records of A_Pad come first in the linear offsets of the
// level, so a secondary dispatch of Reader that starts at a wrong offset in front of the records of Owner reads records
// of A_Pad, and does not read past the offsets.
// RWBuf[0] counts the groups of Owner, RWBuf[1] the groups of Reader, RWBuf[2] records with wrong contents, RWBuf[3]
// records that arrive more than once, RWBuf[4] the groups of A_Pad. RWBuf[16 + i] has one bit for each Owner record
// id, RWBuf[1056 + i] one bit for each Reader record id (32868 ids, 1028 words each).
static const uint RECORDS = 32868;

struct InputRecord
{
	uint id;
	uint tag; // 1 for Owner, 2 for A_Pad.
};

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
[NodeDispatchGrid(514, 1, 1)]
[NumThreads(64, 1, 1)]
void Entry(uint id : SV_DispatchThreadID, [MaxRecords(64)] NodeOutput<InputRecord> A_Pad,
		[MaxRecords(64)] NodeOutput<InputRecord> Owner)
{
	ThreadNodeOutputRecords<InputRecord> p = A_Pad.GetThreadNodeOutputRecords(id < RECORDS ? 1 : 0);
	if (id < RECORDS)
	{
		p.Get().id = id;
		p.Get().tag = 2;
	}
	p.OutputComplete();
	ThreadNodeOutputRecords<InputRecord> o = Owner.GetThreadNodeOutputRecords(id < RECORDS ? 1 : 0);
	if (id < RECORDS)
	{
		o.Get().id = id;
		o.Get().tag = 1;
	}
	o.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void A_Pad(DispatchNodeInputRecord<InputRecord> input)
{
	uint o;
	if (input.Get().tag != 2u || input.Get().id >= RECORDS)
		bad_record();
	InterlockedAdd(RWBuf[4], 1, o);
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void Owner(DispatchNodeInputRecord<InputRecord> input)
{
	InputRecord r = input.Get();
	uint o;

	InterlockedAdd(RWBuf[0], 1, o);
	if (r.tag != 1u || r.id >= RECORDS)
	{
		bad_record();
		return;
	}
	mark(16 + r.id / 32, r.id & 31u);
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeShareInputOf("Owner")]
void Reader(DispatchNodeInputRecord<InputRecord> input)
{
	InputRecord r = input.Get();
	uint o;

	InterlockedAdd(RWBuf[1], 1, o);
	if (r.tag != 1u || r.id >= RECORDS)
	{
		bad_record();
		return;
	}
	mark(1056 + r.id / 32, r.id & 31u);
}
