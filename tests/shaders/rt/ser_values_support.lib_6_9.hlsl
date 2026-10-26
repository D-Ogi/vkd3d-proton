struct [raypayload] Payload { uint value : read(miss, closesthit, caller) : write(miss, closesthit, caller); };
cbuffer Local : register(b1) { uint Sentinel; };
[shader("miss")] void ValueMiss0(inout Payload p) { p.value += 11; }
[shader("miss")] void ValueMiss1(inout Payload p) { p.value += 23; }
[shader("closesthit")] void ValueHit(inout Payload p, BuiltInTriangleIntersectionAttributes a) { p.value += 11; }
[shader("callable")] void ValueCallable(inout Payload p) { p.value += 7; }
