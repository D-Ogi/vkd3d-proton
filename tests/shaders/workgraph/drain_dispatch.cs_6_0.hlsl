RWStructuredBuffer<uint> RWBuf : register(u0);

// An ordinary compute dispatch after a DispatchGraph, for test_workgraph_drain_rounds. It counts its launches in
// RWBuf[8], which the work graph tests do not use.
[numthreads(1, 1, 1)]
void main()
{
	uint o;
	InterlockedAdd(RWBuf[8], 1, o);
}
