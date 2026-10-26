"""Evaluate generated SPIR-V store addresses, not shader floating-point values or a GPU.

Only recursively needed address instructions are interpreted. Unknown instructions
on that path fail. Model the documented Vulkan four-byte range rounding and compare
against exact D3D12 structured element bounds. The unchanged GPU client remains the
end-to-end acceptance test.
"""
import argparse
import json
import re
from pathlib import Path

def check(path, tail=False, extended=False):
    source = path.read_text()
    instructions = {}
    accesses = []
    sets = {}
    for line in source.splitlines():
        t = line.split(';', 1)[0].split()
        if not t:
            continue
        if len(t) > 2 and t[1] == '=':
            instructions[t[0]] = (t[2], t[3:])
            if t[2] == 'OpLoad': accesses.append(t[4])
        elif t[0] == 'OpStore':
            accesses.append(t[1])
        elif t[0] == 'OpDecorate' and t[2] == 'DescriptorSet':
            sets[t[1]] = int(t[3])
    results = []
    for count in ((5, 6, 429496729, 306783377) if extended else (5, 6)):
        for thread in (sorted(set((0, 4, 5, count - 1, count, count + 1, 0xffffffff))) if extended else range(6)):
            cache = {}
            def value(id):
                if id in cache:
                    return cache[id]
                op, a = instructions[id]
                if op == 'OpConstant':
                    v = int(a[1], 0)
                elif op == 'OpConstantComposite':
                    v = [value(x) for x in a[1:]]
                elif op == 'OpVariable':
                    v = (id, [])
                elif op == 'OpAccessChain':
                    root, indices = value(a[1])
                    v = (root, indices + [value(x) for x in a[2:]])
                elif op == 'OpLoad':
                    root, ix = value(a[1])
                    if root == '%gl_GlobalInvocationID':
                        assert ix == [0]
                        v = thread
                    elif root == '%registers':
                        assert ix == [0]
                        v = 0
                    elif sets.get(root) == 1:
                        assert ix[0] == 0
                        descriptor = ix[1]
                        stride = [10,12,14,16,32,0][descriptor % 6]
                        v = [0, stride * (count if descriptor >= 6 else count - 1)]
                    else:
                        raise ValueError(('Unexpected address load', root, ix))
                elif op == 'OpGroupNonUniformBroadcastFirst':
                    v = value(a[2])
                elif op == 'OpCompositeExtract':
                    v = value(a[1])[int(a[2])]
                elif op == 'OpArrayLength':
                    root, ix = value(a[1])
                    assert sets.get(root) == 0 and len(ix) == 1 and a[2] == '0'
                    descriptor = ix[0]
                    stride = [10,12,14,16,32,0][descriptor % 6]
                    n = count if descriptor >= 6 else count - 1
                    v = ((stride * n + 3) & ~3) // 2
                elif op in ('OpIAdd', 'OpIMul', 'OpShiftRightLogical', 'OpULessThan', 'OpUDiv'):
                    x, y = value(a[1]), value(a[2])
                    def scalar(x, y):
                        if op == 'OpIAdd': return (x + y) & 0xffffffff
                        if op == 'OpIMul': return (x * y) & 0xffffffff
                        if op == 'OpUDiv': return x // y
                        if op == 'OpShiftRightLogical': return x >> y
                        return x < y
                    v = [scalar(i,j) for i,j in zip(x,y)] if isinstance(x, list) else scalar(x,y)
                elif op == 'OpSelect':
                    v = value(a[2] if value(a[1]) else a[3])
                else:
                    raise ValueError(('Unsupported address instruction', id, op, a))
                cache[id] = v
                return v
            actual = {0: [], 2: [], 6: [], 8: []}
            for pointer in accesses:
                # Resolve only the descriptor-array prefix before examining its offset.
                pointer_op, args = instructions[pointer]
                if pointer_op != 'OpAccessChain': continue
                parent = args[1]
                op, p = instructions[parent]
                if op != 'OpAccessChain':
                    continue
                root, ix = value(parent)
                if sets.get(root) != 0 or ix[0] not in actual:
                    continue
                _, full_ix = value(pointer)
                assert len(full_ix) == 3 and full_ix[1] == 0
                descriptor = full_ix[0]
                stride = 10 if descriptor % 6 == 0 else 14
                elements = count if descriptor >= 6 else count - 1
                if elements * stride > 0xfffffffc: continue
                pointer_type = args[0]
                type_op, type_args = instructions[pointer_type]
                assert type_op == 'OpTypePointer'
                value_op, value_args = instructions[type_args[1]]
                components = int(value_args[1]) if value_op == 'OpTypeVector' else 1
                offset = 2 * components * full_ix[2]
                for component in range(components):
                    component_offset = offset + 2 * component
                    if component_offset + 2 <= ((elements * stride + 3) & ~3):
                        actual[descriptor].append(component_offset)
            for descriptor, offsets in actual.items():
                stride = 10 if descriptor % 6 == 0 else 14
                elements = count if descriptor >= 6 else count - 1
                if elements * stride > 0xfffffffc: continue
                expected = list(range(thread * stride, (thread + 1) * stride, 2)) if thread < elements else []
                if tail and expected: expected = expected[-1:]
                # Descriptor addresses already include FirstElement. Check both
                # zero and the aligned nonzero base used by the GPU client.
                for base in (0, 4144 if stride == 14 else 10240):
                    results.append(dict(count=elements, thread=thread, stride=stride, base=base,
                                        access='store' if descriptor >= 6 else 'load',
                                        actual=[base + x for x in sorted(offsets)],
                                        expected=[base + x for x in expected],
                                        passed=sorted(offsets) == expected))
    return dict(file=str(path), cases=results, failures=sum(not r['passed'] for r in results))

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('old', type=Path); p.add_argument('fixed', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--tail', action='store_true')
    p.add_argument('--extended', action='store_true')
    a = p.parse_args()
    results = [check(a.old, a.tail, a.extended), check(a.fixed, a.tail, a.extended)]
    a.output.write_text(json.dumps(results, indent=2))
    for result in results:
        print(result['file'], 'failures', result['failures'], 'cases', len(result['cases']))
        for case in result['cases']:
            if not case['passed']: print(case)
    if not a.extended:
        assert results[0]['failures'] == (0 if a.tail else 8), 'Old source must reproduce unsafe loads/stores at both bases'
    assert results[1]['failures'] == 0, 'Fixed option must preserve all valid stores and drop OOB stores'
