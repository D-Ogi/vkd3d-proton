RWStructuredBuffer<uint> RWBuf : register(u0);

// A 256-byte record. The first and the last dword are read, so that a record that is cut short is seen.
struct Record
{
	uint value;
	uint padding[62];
	uint tail;
};

// One launch per record. RWBuf[base + 0] counts the launches, RWBuf[base + 1] sums the first dword and
// RWBuf[base + 2] sums the last dword of each record.
void count_record(uint base, Record record)
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

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
void EntryA(ThreadNodeInputRecord<Record> input)
{
	count_record(0, input.Get());
}

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
void EntryB(ThreadNodeInputRecord<Record> input)
{
	count_record(4, input.Get());
}
