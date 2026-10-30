// Apply a rename plan CSV (rva,namespace,name,source,confidence,function,note) to the program:
// creates missing functions, moves them into class namespaces, renames (USER_DEFINED) and adds a
// comment with source/confidence. Usage: ApplyRenames.java <renames.csv>
// @category EAW
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.app.util.NamespaceUtils;
import java.io.*;
import java.util.*;

public class ApplyRenames extends GhidraScript {
    static List<String> parseCsv(String line) {
        List<String> out = new ArrayList<>(); StringBuilder cur = new StringBuilder(); boolean q = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (q) { if (c == '"') { if (i + 1 < line.length() && line.charAt(i + 1) == '"') { cur.append('"'); i++; } else q = false; } else cur.append(c); }
            else if (c == '"') q = true; else if (c == ',') { out.add(cur.toString()); cur.setLength(0); } else cur.append(c);
        }
        out.add(cur.toString()); return out;
    }
    @Override
    public void run() throws Exception {
        String csv = getScriptArgs()[0];
        long base = currentProgram.getImageBase().getOffset();
        SymbolTable st = currentProgram.getSymbolTable();
        Map<String, Namespace> byName = new HashMap<>();
        SymbolIterator it = st.getDefinedSymbols();
        while (it.hasNext()) {
            Symbol s = it.next();
            if (s.getSymbolType() == SymbolType.CLASS || s.getSymbolType() == SymbolType.NAMESPACE) {
                Object o = s.getObject();
                if (o instanceof Namespace) byName.put(((Namespace) o).getName(true), (Namespace) o);
            }
        }
        int created = 0, renamed = 0, failed = 0, nsCreated = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(csv))) {
            String line = r.readLine();
            while ((line = r.readLine()) != null) {
                if (monitor.isCancelled()) break;
                List<String> f = parseCsv(line);
                long rva = Long.decode(f.get(0));
                String nsName = f.get(1), name = f.get(2), src = f.get(3), conf = f.get(4), note = f.size() > 6 ? f.get(6) : "";
                Address a = toAddr(base + rva);
                try {
                    Function fn = getFunctionAt(a);
                    if (fn == null) {
                        if (getInstructionAt(a) == null) new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
                        fn = createFunction(a, null);
                        if (fn == null) { failed++; continue; }
                        created++;
                    }
                    Namespace ns = currentProgram.getGlobalNamespace();
                    if (!nsName.isEmpty()) {
                        ns = byName.get(nsName);
                        if (ns == null) {
                            ns = NamespaceUtils.createNamespaceHierarchy(nsName, null, currentProgram, SourceType.USER_DEFINED);
                            byName.put(nsName, ns); nsCreated++;
                        }
                    }
                    fn.setParentNamespace(ns);
                    String nm = name; int k = 2;
                    while (true) {
                        try { fn.setName(nm, SourceType.USER_DEFINED); break; }
                        catch (ghidra.util.exception.DuplicateNameException e) { nm = name + "_" + (k++); if (k > 50) throw e; }
                    }
                    String c = "[" + src + "/" + conf + "]" + (note.isEmpty() ? "" : " " + note);
                    fn.setComment(fn.getComment() == null ? c : fn.getComment() + "\n" + c);
                    renamed++;
                } catch (Exception e) {
                    failed++; if (failed < 30) println("fail " + f.get(0) + " " + nsName + "::" + name + " : " + e);
                }
            }
        }
        println("ApplyRenames: renamed=" + renamed + " created=" + created + " failed=" + failed + " namespaces_created=" + nsCreated);
    }
}
