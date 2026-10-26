"""Evaluate generated integer GetDimensions SPIR-V; this is not GPU execution."""
import argparse
import json
from pathlib import Path

def check(path):
    ins, sets, array_strides, stores = {}, {}, {}, []
    for line in path.read_text().splitlines():
        t = line.split(';', 1)[0].split()
        if not t:
            continue
        if len(t) > 2 and t[1] == '=':
            ins[t[0]] = (t[2], t[3:])
        elif t[0] == 'OpDecorate' and t[2] == 'DescriptorSet':
            sets[t[1]] = int(t[3])
        elif t[0] == 'OpDecorate' and t[2] == 'ArrayStride':
            array_strides[t[1]] = int(t[3])
        elif t[0] == 'OpStore':
            stores.append(t[1:3])
    strides = [2, 6, 10, 14, 32, 0]
    results = []
    for mode in [0, 1, 4, 5, 6, 7, 100, 'near4GiB']:
        counts = [(0xfffffffc // s if mode == 'near4GiB' else mode) if s else 4096 for s in strides]
        cache = {}
        def value(id):
            if id in cache:
                return cache[id]
            op, a = ins[id]
            if op == 'OpConstant':
                v = int(a[1], 0)
            elif op == 'OpVariable':
                v = (id, [])
            elif op == 'OpAccessChain':
                root, ix = value(a[1]); v = (root, ix + [value(i) for i in a[2:]])
            elif op == 'OpLoad':
                root, ix = value(a[1])
                if root == '%registers':
                    assert ix == [0]; v = 0
                elif sets.get(root) == 2:
                    assert ix[0] == 0 and ix[2:] == [0, 13]
                    d = ix[1] % 6; v = counts[d] * strides[d]
                else:
                    raise ValueError((op, root, ix))
            elif op == 'OpArrayLength':
                root, ix = value(a[1]); assert sets[root] == 0 and len(ix) == 1
                pointer_type = ins[a[1]][1][0]
                structure = ins[pointer_type][1][1]
                array_type = ins[structure][1][0]
                unit = array_strides[array_type]
                d = ix[0] % 6; v = ((counts[d] * strides[d] + 3) & ~3) // unit
            elif op == 'OpCopyObject':
                v = value(a[1])
            elif op in ('OpIAdd', 'OpIMul', 'OpUDiv', 'OpUMod'):
                x, y = value(a[1]), value(a[2])
                v = {'OpIAdd': lambda: x+y, 'OpIMul': lambda: x*y,
                     'OpUDiv': lambda: x//y, 'OpUMod': lambda: x%y}[op]() & 0xffffffff
            else:
                raise ValueError((id, op, a))
            cache[id] = v
            return v
        for ptr, val in stores:
            root, ix = value(ptr)
            assert sets[root] == 0 and ix[0] == 11 and ix[1] == 0
            offset = ix[2] - 440
            d, field = divmod(offset, 4)
            expected = strides[d] if field & 1 else counts[d]
            actual = value(val)
            results.append(dict(mode=mode, stride=strides[d], field=field,
                                expected=expected, actual=actual, passed=actual == expected))
    return dict(file=str(path), cases=results, failures=sum(not x['passed'] for x in results))

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('old', type=Path); p.add_argument('fixed', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    results = [check(a.old), check(a.fixed)]
    a.output.write_text(json.dumps(results, indent=2))
    for r in results:
        print(r['file'], 'failures', r['failures'], 'cases', len(r['cases']))
    assert results[0]['failures'] > 0, 'Negative control did not reproduce'
    assert results[1]['failures'] == 0, 'Generated GetDimensions arithmetic is wrong'
