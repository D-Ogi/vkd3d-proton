RWStructuredBuffer<uint> RWBuf : register(u0);

struct InputRecord
{
	uint3 grid : SV_DispatchGrid;
};

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(2, 2, 2)]
[NumThreads(2, 3, 4)]
void EntryA(DispatchNodeInputRecord<InputRecord> input, uint3 thr : SV_DispatchThreadID)
{
	uint idx = thr.z * 100 + thr.y * 10 + thr.x;
	uint o;
	InterlockedAdd(RWBuf[idx], 1, o);
}

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(2, 2, 2)]
[NumThreads(2, 3, 4)]
void EntryB(DispatchNodeInputRecord<InputRecord> input, uint3 thr : SV_DispatchThreadID)
{
	uint idx = thr.z * 100 + thr.y * 10 + thr.x;
	uint o;
	InterlockedAdd(RWBuf[idx], 0x10000, o);
}
