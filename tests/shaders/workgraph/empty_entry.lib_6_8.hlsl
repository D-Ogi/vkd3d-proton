RWStructuredBuffer<uint> RWBuf : register(u0);

// An entry point with an empty input record: each record of the input is one launch.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
void EmptyEntry()
{
	uint o;
	InterlockedAdd(RWBuf[0], 1, o);
}
