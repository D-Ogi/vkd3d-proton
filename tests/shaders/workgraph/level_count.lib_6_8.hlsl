RWStructuredBuffer<uint> RWBuf : register(u0);

// A 256-byte input record. The first and the last dword are read, so that a record that is cut short is seen.
struct Record
{
	uint value;
	uint padding[62];
	uint tail;
};

// The record of the second level: 128 bytes, the largest output of a thread launch node.
struct ChildRecord
{
	uint value;
	uint padding[30];
	uint tail;
};

// One launch per record. RWBuf[base + 0] counts the launches, RWBuf[base + 1] sums the first dword and
// RWBuf[base + 2] sums the last dword of each record.
void count_record(uint base, ChildRecord record)
{
	uint launches = WaveActiveCountBits(true);
	uint value = WaveActiveSum(record.value);
	uint tail = WaveActiveSum(record.tail);
	uint o;

	if (WaveIsFirstLane())
	{
		InterlockedAdd(RWBuf[base + 0], launches, o);
		InterlockedAdd(RWBuf[base + 1], value, o);
		InterlockedAdd(RWBuf[base + 2], tail, o);
	}
}

// Each entry point sends the first and the last dword of its record to a node of the second level, which counts
// them. The records of the second level go through the payload memory of the work graph execution.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
void EntryA(ThreadNodeInputRecord<Record> input, [MaxRecords(1)] NodeOutput<ChildRecord> ChildA)
{
	ThreadNodeOutputRecords<ChildRecord> output = ChildA.GetThreadNodeOutputRecords(1);
	output.Get().value = input.Get().value;
	output.Get().tail = input.Get().tail;
	output.OutputComplete();
}

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
void EntryB(ThreadNodeInputRecord<Record> input, [MaxRecords(1)] NodeOutput<ChildRecord> ChildB)
{
	ThreadNodeOutputRecords<ChildRecord> output = ChildB.GetThreadNodeOutputRecords(1);
	output.Get().value = input.Get().value;
	output.Get().tail = input.Get().tail;
	output.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void ChildA(ThreadNodeInputRecord<ChildRecord> input)
{
	count_record(0, input.Get());
}

[Shader("node")]
[NodeLaunch("thread")]
void ChildB(ThreadNodeInputRecord<ChildRecord> input)
{
	count_record(4, input.Get());
}
