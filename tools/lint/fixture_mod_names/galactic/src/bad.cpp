// modc t04 lint fixture: every finding in this file is expected by check_mod_names.py --selftest (line numbers matter).
#include <string>
// A comment naming Empire / "Rebel" is not code.
const char* a = "Empire";
const char* b = "Rebel_Infantry_Squad";
const char* c = "BasicRebel";
const char* d = R"(<Faction Name="Hutts">)";
int e = 1;
const char* f = "1125571106";
// [name-ok: retail name, docs/re/EXAMPLE.md 0x140000000]
const char* g = "Underworld";
const char* h = "Pirates";

// [name-ok: ]
const char* i = "Neutral";
// [name-ok-end]
const char* j = "Hostile";
