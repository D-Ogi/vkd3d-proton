RWStructuredBuffer<uint> RWBuf : register(u0);

// An entry point with an empty input record: each record of the input is one launch.
// RWBuf[0] and RWBuf[1] are the low and the high half of a 64-bit launch count, so that more than
// 2^32 - 1 launches are counted correctly.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("thread")]
void EmptyEntry()
{
	uint launches = WaveActiveCountBits(true);
	uint o;

	if (WaveIsFirstLane())
	{
		InterlockedAdd(RWBuf[0], launches, o);
		if (o + launches < o)
			InterlockedAdd(RWBuf[1], 1, o);
	}
}
