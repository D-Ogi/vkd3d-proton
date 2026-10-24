RWStructuredBuffer<uint> RWBuf : register(u0);

// A shared reader with outputs, for test_workgraph_drain_layout, which does not dispatch the graph. Entry writes one
// record into Reader. Tapped shares the input of Reader. Reader (grid 2) writes 2 records of 16 bytes into Leaf for
// each group; Tapped (grid 3) writes 8 records of 64 bytes into Wide for each group. W of the pool of level 2 is the W
// of Tapped, which is larger than the W of Reader. The demand of one record is the sum of both readers.
struct InputRecord
{
	uint id;
};

struct LeafRecord
{
	uint words[4];
};

struct WideRecord
{
	uint words[16];
};

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
void Entry([MaxRecords(1)] [NodeID("Reader")] NodeOutput<InputRecord> output)
{
	GroupNodeOutputRecords<InputRecord> r = output.GetGroupNodeOutputRecords(1);
	r.Get().id = 1;
	r.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(2, 1, 1)]
[NumThreads(1, 1, 1)]
void Reader(DispatchNodeInputRecord<InputRecord> input, [MaxRecords(2)] NodeOutput<LeafRecord> Leaf)
{
	GroupNodeOutputRecords<LeafRecord> r = Leaf.GetGroupNodeOutputRecords(2);
	r[0].words[0] = input.Get().id;
	r[1].words[0] = input.Get().id;
	r.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(3, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeShareInputOf("Reader")]
void Tapped(DispatchNodeInputRecord<InputRecord> input, [MaxRecords(8)] NodeOutput<WideRecord> Wide)
{
	GroupNodeOutputRecords<WideRecord> r = Wide.GetGroupNodeOutputRecords(1);
	r.Get().words[0] = input.Get().id;
	r.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Leaf(ThreadNodeInputRecord<LeafRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[0], input.Get().words[0], o);
}

[Shader("node")]
[NodeLaunch("thread")]
void Wide(ThreadNodeInputRecord<WideRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], input.Get().words[0], o);
}
