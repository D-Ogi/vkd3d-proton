RWStructuredBuffer<uint> RWBuf : register(u0);

struct Record
{
	uint value;
};

// An entry point that writes to its own input record. Each thread must see its own write to a separate record.
// RWBuf[value] counts the threads that read value and then read back value + 100. A thread that sees another
// value adds 0x10000 instead, so that a shared or changed record shows in the result.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
void RWEntry(RWThreadNodeInputRecord<Record> rec)
{
	uint v = rec.Get().value;
	rec.Get().value = v + 100;
	uint w = rec.Get().value;
	uint o;
	InterlockedAdd(RWBuf[v & 1023], w == v + 100 ? 1 : 0x10000, o);
}
