"""Write tests/ctest_cost_seed.txt, the starting CTestCostData.txt of a fresh build tree.

ctest with -j starts the longest tests first, but only knows how long they take from a previous run in the same build tree
(<tree>/Testing/Temporary/CTestCostData.txt). A new tree or worktree has none, so a 58 s test can start last and set the wall
time. The top-level CMakeLists.txt copies the seed into a tree that has no cost file yet; ctest overwrites it with measured
costs after the first run.

Usage: ctest_cost_seed.py <ctest_junit.xml> [<ctest_junit.xml> ...]   (the longest time seen for each test; tests under
1 s are left out, ctest treats an unknown test as cost 0). Refresh it now and then; stale names are harmless.
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

MIN_SECONDS = 1.0
OUT = Path(__file__).resolve().parents[2] / 'tests' / 'ctest_cost_seed.txt'


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    cost = {}
    for path in argv:
        for case in ET.parse(path).getroot().iter('testcase'):
            name, t = case.get('name'), float(case.get('time', 0))
            if name and t >= MIN_SECONDS:
                cost[name] = max(cost.get(name, 0.0), t)
    lines = [f'{n} 1 {t:.3f}' for n, t in sorted(cost.items(), key=lambda kv: (-kv[1], kv[0]))]
    # CTestCostData.txt format: "<test> <runs> <average seconds>" lines, then "---" and the names of tests that failed last time.
    OUT.write_text('\n'.join(lines) + '\n---\n', encoding='utf-8', newline='\n')
    print(f'{OUT}: {len(lines)} tests')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
