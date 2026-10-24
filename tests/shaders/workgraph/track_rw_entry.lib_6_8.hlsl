RWStructuredBuffer<uint> RWBuf : register(u0);

struct [NodeTrackRWInputSharing] Record
{
	uint value;
};

// An entry point whose record carries [NodeTrackRWInputSharing]. The record of the application is 4 bytes. The
// shader does not call FinishedCrossGroupSharing(), which an entry point must not call.
[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(32, 1, 1)]
void TrackEntry(RWDispatchNodeInputRecord<Record> rec, uint tid : SV_GroupIndex)
{
	uint o;
	if (tid == 0)
		InterlockedAdd(RWBuf[rec.Get().value & 1023], 1, o);
}
