"""Count the ctest entries that passed without running their real checks.

Tests print "... not set, skipped" and exit 0, so ctest reports them as Passed. This reads the JUnit file written by
`ctest --output-junit` and classifies each such test by the text it printed:
  data   - needs ALAMO_FOC_DATA / ALAMO_EAW_DATA (or a module that is not configured)
  gpu    - no Vulkan loader or device
  other  - any other skip message, or a ctest-level skip (SKIP_RETURN_CODE / SKIP_REGULAR_EXPRESSION)
Usage: ctest_skips.py <junit.xml> [--list]
Prints one line "skips: data=N gpu=N other=N (of T tests)". Exit code is always 0; the count is information.
"""
import re
import sys
import xml.etree.ElementTree as ET

DATA = re.compile(r'ALAMO_(FOC|EAW)_DATA|data not found|not configured|built without|--game not set', re.I)
GPU = re.compile(r'\bgpu\b|vulkan|VK_KHR|no graphics', re.I)
SKIP = re.compile(r'\bskip(ped)?\b|\bunset\b|\bnot set\b', re.I)


def classify(case):
    ctest_skip = case.find('skipped') is not None
    out = (case.findtext('system-out') or '')
    hits = [l for l in out.splitlines() if SKIP.search(l)]
    if not hits and not ctest_skip:
        return None
    text = '\n'.join(hits) or out
    if GPU.search(text) and not DATA.search(text):
        return 'gpu'
    if DATA.search(text):
        return 'data'
    return 'other'


def main(argv):
    path = argv[1]
    cases = ET.parse(path).getroot().iter('testcase')
    counts = {'data': 0, 'gpu': 0, 'other': 0}
    total = 0
    listing = []
    for c in cases:
        total += 1
        k = classify(c)
        if k:
            counts[k] += 1
            listing.append((k, c.get('name')))
    if '--list' in argv:
        for k, n in sorted(listing):
            print(f'  {k:5} {n}')
    print(f"skips: data={counts['data']} gpu={counts['gpu']} other={counts['other']} (of {total} tests)")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
