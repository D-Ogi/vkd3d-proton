RWStructuredBuffer<uint> RWBuf : register(u0);

// Shared readers whose owner is in another subgroup-sized chunk of the distributor (test_workgraph_drain_retention).
// The engine gives the nodes slots in the order of their node IDs: A_Entry 0, A_Owner 1, B_Reader 2, M[0] to M[69] 3
// to 72, Sink 73, Y_Owner 74, Z_Reader 75. Z_Reader shares the input of A_Owner (owner slot 1, reader slot 75), and
// B_Reader shares the input of Y_Owner (reader slot 2, owner slot 74). With a subgroup of 32 or 64 lanes, the owner and
// the reader of each pair are in different chunks. A_Entry declares M[0] to M[69], but writes no records into them.
//   level 0: A_Entry (grid 4) writes one record for A_Owner and one for Y_Owner for each group;
//   level 1: A_Owner, Z_Reader, Y_Owner and B_Reader (grid 64, 2 threads) write 2 Sink records of 256 bytes for each
//     group;
//   level 2: Sink (thread launch, no outputs).
// A record of level 1 has a demand of 256 Sink records on pool 2 (both readers), 128 times W of pool 2, so pool 2 sets
// how many records a round admits, and level 1 keeps records across rounds.
// RWBuf[0] counts the Sink records, RWBuf[1] the groups of M, RWBuf[2] records with wrong contents, RWBuf[3] records
// that arrive more than once, RWBuf[4 + n] the groups of reader n (0 A_Owner, 1 Z_Reader, 2 Y_Owner, 3 B_Reader).
// RWBuf[64 + 16 n + i] has one bit for each Sink id of reader n (512 ids).
struct OwnerRecord
{
	uint id;
	uint check;
};

struct FillRecord
{
	uint id;
};

struct SinkRecord
{
	uint id;
	uint src;
	uint words[61];
	uint check;
};

uint pattern(uint id, uint i)
{
	return id * 0x9e3779b1u + (i + 1u) * 0x85ebca77u;
}

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
[NodeDispatchGrid(4, 1, 1)]
[NumThreads(1, 1, 1)]
void A_Entry(uint gid : SV_GroupID, [MaxRecords(1)] NodeOutput<OwnerRecord> A_Owner,
		[MaxRecords(1)] NodeOutput<OwnerRecord> Y_Owner, [MaxRecords(1)] [NodeArraySize(70)] NodeOutputArray<FillRecord> M)
{
	GroupNodeOutputRecords<OwnerRecord> a = A_Owner.GetGroupNodeOutputRecords(1);
	a.Get().id = gid;
	a.Get().check = pattern(gid, 0);
	a.OutputComplete();
	GroupNodeOutputRecords<OwnerRecord> y = Y_Owner.GetGroupNodeOutputRecords(1);
	y.Get().id = gid;
	y.Get().check = pattern(gid, 1);
	y.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(64, 1, 1)]
[NumThreads(2, 1, 1)]
void A_Owner(DispatchNodeInputRecord<OwnerRecord> input, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(2)] NodeOutput<SinkRecord> Sink)
{
	OwnerRecord r = input.Get();
	uint o, id = r.id * 128 + gid * 2 + tid;

	if (tid == 0 && (r.id >= 4u || r.check != pattern(r.id, 0)))
		bad_record();
	if (tid == 0)
		InterlockedAdd(RWBuf[4], 1, o);
	GroupNodeOutputRecords<SinkRecord> s = Sink.GetGroupNodeOutputRecords(2);
	s[tid].id = id;
	s[tid].src = 0;
	for (uint w = 0; w < 61; w++)
		s[tid].words[w] = pattern(id + 0, w);
	s[tid].check = pattern(id + 0, 61);
	s.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(64, 1, 1)]
[NumThreads(2, 1, 1)]
[NodeShareInputOf("A_Owner")]
void Z_Reader(DispatchNodeInputRecord<OwnerRecord> input, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(2)] NodeOutput<SinkRecord> Sink)
{
	OwnerRecord r = input.Get();
	uint o, id = r.id * 128 + gid * 2 + tid;

	if (tid == 0 && (r.id >= 4u || r.check != pattern(r.id, 0)))
		bad_record();
	if (tid == 0)
		InterlockedAdd(RWBuf[5], 1, o);
	GroupNodeOutputRecords<SinkRecord> s = Sink.GetGroupNodeOutputRecords(2);
	s[tid].id = id;
	s[tid].src = 1;
	for (uint w = 0; w < 61; w++)
		s[tid].words[w] = pattern(id + 512, w);
	s[tid].check = pattern(id + 512, 61);
	s.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(64, 1, 1)]
[NumThreads(2, 1, 1)]
void Y_Owner(DispatchNodeInputRecord<OwnerRecord> input, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(2)] NodeOutput<SinkRecord> Sink)
{
	OwnerRecord r = input.Get();
	uint o, id = r.id * 128 + gid * 2 + tid;

	if (tid == 0 && (r.id >= 4u || r.check != pattern(r.id, 1)))
		bad_record();
	if (tid == 0)
		InterlockedAdd(RWBuf[6], 1, o);
	GroupNodeOutputRecords<SinkRecord> s = Sink.GetGroupNodeOutputRecords(2);
	s[tid].id = id;
	s[tid].src = 2;
	for (uint w = 0; w < 61; w++)
		s[tid].words[w] = pattern(id + 1024, w);
	s[tid].check = pattern(id + 1024, 61);
	s.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(64, 1, 1)]
[NumThreads(2, 1, 1)]
[NodeShareInputOf("Y_Owner")]
void B_Reader(DispatchNodeInputRecord<OwnerRecord> input, uint gid : SV_GroupID, uint tid : SV_GroupIndex,
		[MaxRecords(2)] NodeOutput<SinkRecord> Sink)
{
	OwnerRecord r = input.Get();
	uint o, id = r.id * 128 + gid * 2 + tid;

	if (tid == 0 && (r.id >= 4u || r.check != pattern(r.id, 1)))
		bad_record();
	if (tid == 0)
		InterlockedAdd(RWBuf[7], 1, o);
	GroupNodeOutputRecords<SinkRecord> s = Sink.GetGroupNodeOutputRecords(2);
	s[tid].id = id;
	s[tid].src = 3;
	for (uint w = 0; w < 61; w++)
		s[tid].words[w] = pattern(id + 1536, w);
	s[tid].check = pattern(id + 1536, 61);
	s.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void Sink(ThreadNodeInputRecord<SinkRecord> input)
{
	SinkRecord r = input.Get();
	bool good = r.src < 4u && r.id < 512u && r.check == pattern(r.id + 512 * r.src, 61);
	uint o;

	for (uint w = 0; w < 61; w++)
		good = good && r.words[w] == pattern(r.id + 512 * r.src, w);
	if (!good)
	{
		bad_record();
		return;
	}
	mark(64 + r.src * 16 + r.id / 32, r.id & 31u);
	InterlockedAdd(RWBuf[0], 1, o);
}

[Shader("node")]
[NodeID("M", 0)]
[NodeLaunch("thread")]
void M00(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 1)]
[NodeLaunch("thread")]
void M01(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 2)]
[NodeLaunch("thread")]
void M02(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 3)]
[NodeLaunch("thread")]
void M03(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 4)]
[NodeLaunch("thread")]
void M04(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 5)]
[NodeLaunch("thread")]
void M05(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 6)]
[NodeLaunch("thread")]
void M06(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 7)]
[NodeLaunch("thread")]
void M07(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 8)]
[NodeLaunch("thread")]
void M08(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 9)]
[NodeLaunch("thread")]
void M09(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 10)]
[NodeLaunch("thread")]
void M10(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 11)]
[NodeLaunch("thread")]
void M11(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 12)]
[NodeLaunch("thread")]
void M12(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 13)]
[NodeLaunch("thread")]
void M13(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 14)]
[NodeLaunch("thread")]
void M14(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 15)]
[NodeLaunch("thread")]
void M15(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 16)]
[NodeLaunch("thread")]
void M16(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 17)]
[NodeLaunch("thread")]
void M17(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 18)]
[NodeLaunch("thread")]
void M18(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 19)]
[NodeLaunch("thread")]
void M19(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 20)]
[NodeLaunch("thread")]
void M20(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 21)]
[NodeLaunch("thread")]
void M21(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 22)]
[NodeLaunch("thread")]
void M22(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 23)]
[NodeLaunch("thread")]
void M23(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 24)]
[NodeLaunch("thread")]
void M24(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 25)]
[NodeLaunch("thread")]
void M25(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 26)]
[NodeLaunch("thread")]
void M26(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 27)]
[NodeLaunch("thread")]
void M27(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 28)]
[NodeLaunch("thread")]
void M28(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 29)]
[NodeLaunch("thread")]
void M29(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 30)]
[NodeLaunch("thread")]
void M30(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 31)]
[NodeLaunch("thread")]
void M31(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 32)]
[NodeLaunch("thread")]
void M32(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 33)]
[NodeLaunch("thread")]
void M33(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 34)]
[NodeLaunch("thread")]
void M34(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 35)]
[NodeLaunch("thread")]
void M35(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 36)]
[NodeLaunch("thread")]
void M36(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 37)]
[NodeLaunch("thread")]
void M37(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 38)]
[NodeLaunch("thread")]
void M38(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 39)]
[NodeLaunch("thread")]
void M39(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 40)]
[NodeLaunch("thread")]
void M40(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 41)]
[NodeLaunch("thread")]
void M41(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 42)]
[NodeLaunch("thread")]
void M42(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 43)]
[NodeLaunch("thread")]
void M43(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 44)]
[NodeLaunch("thread")]
void M44(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 45)]
[NodeLaunch("thread")]
void M45(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 46)]
[NodeLaunch("thread")]
void M46(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 47)]
[NodeLaunch("thread")]
void M47(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 48)]
[NodeLaunch("thread")]
void M48(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 49)]
[NodeLaunch("thread")]
void M49(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 50)]
[NodeLaunch("thread")]
void M50(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 51)]
[NodeLaunch("thread")]
void M51(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 52)]
[NodeLaunch("thread")]
void M52(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 53)]
[NodeLaunch("thread")]
void M53(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 54)]
[NodeLaunch("thread")]
void M54(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 55)]
[NodeLaunch("thread")]
void M55(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 56)]
[NodeLaunch("thread")]
void M56(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 57)]
[NodeLaunch("thread")]
void M57(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 58)]
[NodeLaunch("thread")]
void M58(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 59)]
[NodeLaunch("thread")]
void M59(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 60)]
[NodeLaunch("thread")]
void M60(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 61)]
[NodeLaunch("thread")]
void M61(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 62)]
[NodeLaunch("thread")]
void M62(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 63)]
[NodeLaunch("thread")]
void M63(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 64)]
[NodeLaunch("thread")]
void M64(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 65)]
[NodeLaunch("thread")]
void M65(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 66)]
[NodeLaunch("thread")]
void M66(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 67)]
[NodeLaunch("thread")]
void M67(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 68)]
[NodeLaunch("thread")]
void M68(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}

[Shader("node")]
[NodeID("M", 69)]
[NodeLaunch("thread")]
void M69(ThreadNodeInputRecord<FillRecord> input)
{
	uint o;
	InterlockedAdd(RWBuf[1], 1, o);
}
