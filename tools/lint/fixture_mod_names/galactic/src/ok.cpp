// modc t04 lint fixture: nothing in this file is a finding (check_mod_names.py --selftest).
const char* a = "Empire";  // [name-ok: a marker on the same line]
// [name-ok-begin: a region]
const char* b[] = {"Rebel", "Underworld"};
const char* c = "Hutts";
// [name-ok-end]
/* "Empire" in a block comment */ const char* d = "Data";
// [name-ok: the comment block directly above the statement]
// a second comment line
const char* e = "Sarlacc";
// [name-ok: a marker wrapped
// over two comment lines covers the whole next statement]
if (ready)
    use("Pirates");
