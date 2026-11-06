#!/usr/bin/env python3
"""selftest_modc.py - offline check of the modc loader probe kit: builds the mod from extracted/foc_data_text.zip
(--source-zip, no text file) into a temp folder, checks the files the questions depend on (BOM bytes, digit-led root
elements, list entries) and the verdict logic of modc_run.py. With --mod-check <alamo_mod_check.exe> --game <FoC dir>
it also loads the built mod in the port and prints what the port does with it (the port's own answer to each question).

  selftest_modc.py [--zip extracted\\foc_data_text.zip] [--mod-check EXE --game DIR]

Ends with MODC SELFTEST OK.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
import build_modc_mod  # noqa: E402
import modc_run  # noqa: E402


def check(cond, what):
    if not cond:
        raise SystemExit('MODC SELFTEST FAILED: ' + what)
    print('  ok  ' + what)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--zip', default=os.path.join(ROOT, 'extracted', 'foc_data_text.zip'))
    ap.add_argument('--mod-check')
    ap.add_argument('--game')
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix='modc_selftest_')
    try:
        out = os.path.join(tmp, build_modc_mod.MOD_NAME)
        build_modc_mod.build(None, out, a.zip, no_text=True)

        def rd(*p):
            return open(os.path.join(out, *p), 'rb').read()
        story = ('Data', 'Scripts', 'Story')
        check(rd(*story, 'Modc_Probe_Bom.lua').startswith(build_modc_mod.BOM), 'BOM script starts with EF BB BF')
        check(not rd(*story, 'Modc_Probe_Plain.lua').startswith(build_modc_mod.BOM), 'control script has no BOM')
        check(b'@@' not in rd(*story, 'Modc_Probe_Bom.lua') + rd(*story, 'Modc_Probe_Plain.lua'), 'placeholders replaced')
        check(b'\n<1Modc_Probe_Campaigns>' in rd('Data', 'XML', 'Modc_Probe_Digit_Campaigns.xml'), 'digit-root campaign file')
        check(b'\n<80s_Modc_Probe_Units>' in rd('Data', 'XML', 'Modc_Probe_Units_Digit.xml'), 'digit-root unit file')
        bd = rd('Data', 'XML', 'Modc_Probe_Units_Bom_Digit.xml')
        check(bd.startswith(build_modc_mod.BOM) and b'\n<181st_Modc_Probe_Units>' in bd, 'BOM + digit-root unit file')
        cf = rd('Data', 'XML', 'CampaignFiles.xml')
        check(b'Modc_Probe_Campaigns.xml' in cf and b'Modc_Probe_Digit_Campaigns.xml' in cf, 'CampaignFiles.xml lists both')
        gof = rd('Data', 'XML', 'GameObjectFiles.xml')
        check(all(f.encode() in gof for f in build_modc_mod.UNIT_FILES), 'GameObjectFiles.xml lists the unit files')
        check(all(os.path.exists(os.path.join(out, c)) for c in build_modc_mod.CANARIES), 'canaries installed')
        check(os.path.exists(os.path.join(out, build_modc_mod.MARKER)), 'marker installed')

        all_present = {c: 'present' for c in build_modc_mod.CANARIES}
        v = modc_run.verdicts(all_present, 'unknown')
        check(not v['control_script_ran'] and v['t13_bom_skipped'].startswith('inconclusive'), 'verdict: nothing ran')
        st = dict(all_present, **{'canary_plain_script.txt': 'deleted', 'canary_unit_plain_root.txt': 'deleted'})
        v = modc_run.verdicts(st, 'no')
        check(v['t13_bom_skipped'] == 'no' and v['t14_digit_root_unit'] == 'rejected', 'verdict: stock behaviour')
        st = {c: 'deleted' for c in build_modc_mod.CANARIES}
        v = modc_run.verdicts(st, 'yes')
        check(v['t13_bom_skipped'] == 'yes' and v['t14_bom_digit_root_unit'] == 'accepted', 'verdict: lenient loader')

        if a.mod_check:
            if not a.game:
                raise SystemExit('--mod-check needs --game')
            rep = os.path.join(tmp, 'mod_check.txt')
            subprocess.run([a.mod_check, '--game', a.game, 'MODPATH=' + out, '--verbose', '--out', rep],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            text = open(rep, encoding='utf-8', errors='replace').read()
            print('port (alamo_mod_check) on the probe mod:')
            bom_err = 'MODC_PROBE_BOM.LUA' in text.upper() and 'unexpected symbol' in text
            print('  t13 BOM script compiles in the port: %s' % ('no' if bom_err else 'yes'))
            digit_warn = '<80s_Modc_Probe_Units>' in text or "Modc_Probe_Units_Digit.xml(2): warning: '<'" in text
            print('  t14 digit-root files parse cleanly in the port: %s' % ('no' if digit_warn else 'yes'))
            for line in text.splitlines():
                if line.startswith('campaigns ') or line.startswith('== result'):
                    print('  ' + line.strip())
        print('MODC SELFTEST OK')
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
