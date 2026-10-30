// Exports function table, class/vtable info, string xrefs, call graph and imports
// from an analyzed program. Usage (headless postScript): ExportCore.java <outDir>
// @category EAW
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import java.io.*;
import java.util.*;

public class ExportCore extends GhidraScript {
    static String esc(String s) {
        if (s == null) return "";
        s = s.replace("\"", "\"\"").replace("\n", "\\n").replace("\r", "");
        return "\"" + s + "\"";
    }

    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "/home/claude/work/ghidra/out";
        new File(out).mkdirs();
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        SymbolTable st = currentProgram.getSymbolTable();
        long base = currentProgram.getImageBase().getOffset();

        // functions + call graph
        try (PrintWriter fw = new PrintWriter(new FileWriter(out + "/functions.csv"));
             PrintWriter cw = new PrintWriter(new FileWriter(out + "/callgraph.csv"))) {
            fw.println("rva,name,namespace,size,callers,callees,thunk,calling_convention,signature");
            cw.println("caller_rva,callee_rva");
            int n = 0;
            for (Function f : fm.getFunctions(true)) {
                if (monitor.isCancelled()) break;
                Set<Function> callees = f.getCalledFunctions(monitor);
                Set<Function> callers = f.getCallingFunctions(monitor);
                long rva = f.getEntryPoint().getOffset() - base;
                fw.println(String.format("0x%x,%s,%s,%d,%d,%d,%s,%s,%s", rva, esc(f.getName()),
                        esc(f.getParentNamespace().getName(true)), f.getBody().getNumAddresses(),
                        callers.size(), callees.size(), f.isThunk(), esc(f.getCallingConventionName()),
                        esc(f.getSignature().getPrototypeString())));
                for (Function c : callees)
                    cw.println(String.format("0x%x,0x%x", rva, c.getEntryPoint().getOffset() - base));
                n++;
            }
            println("functions: " + n);
        }

        // classes: namespaces that are GhidraClass, with vftable symbols and methods
        try (PrintWriter w = new PrintWriter(new FileWriter(out + "/classes.csv"))) {
            w.println("class,methods,vftables,vftable_rvas");
            SymbolIterator it = st.getDefinedSymbols();
            Map<String, int[]> cls = new TreeMap<>();
            Map<String, List<String>> vt = new HashMap<>();
            for (Namespace ns : iterNamespaces(st)) {
                if (!(ns instanceof GhidraClass)) continue;
                String name = ns.getName(true);
                int methods = 0;
                List<String> vts = new ArrayList<>();
                for (Symbol s : st.getSymbols(ns)) {
                    if (s.getSymbolType() == SymbolType.FUNCTION) methods++;
                    if (s.getName().startsWith("vftable") || s.getName().contains("vftable"))
                        vts.add(String.format("0x%x", s.getAddress().getOffset() - base));
                }
                w.println(esc(name) + "," + methods + "," + vts.size() + "," + esc(String.join(" ", vts)));
            }
        }

        // strings with referencing functions
        try (PrintWriter w = new PrintWriter(new FileWriter(out + "/strings_xrefs.csv"))) {
            w.println("string_rva,value,ref_function_rvas,ref_function_names");
            DataIterator di = listing.getDefinedData(true);
            int n = 0;
            while (di.hasNext() && !monitor.isCancelled()) {
                Data d = di.next();
                if (!(d.getDataType() instanceof AbstractStringDataType) && !d.hasStringValue()) continue;
                Object v = d.getValue();
                if (v == null) continue;
                String sv = v.toString();
                if (sv.length() < 3) continue;
                LinkedHashSet<String> fr = new LinkedHashSet<>(), fn = new LinkedHashSet<>();
                for (Reference r : rm.getReferencesTo(d.getAddress())) {
                    Function f = fm.getFunctionContaining(r.getFromAddress());
                    if (f != null) {
                        fr.add(String.format("0x%x", f.getEntryPoint().getOffset() - base));
                        fn.add(f.getName(true));
                    }
                }
                w.println(String.format("0x%x,%s,%s,%s", d.getAddress().getOffset() - base, esc(sv),
                        esc(String.join(" ", fr)), esc(String.join(" | ", fn))));
                n++;
            }
            println("strings: " + n);
        }

        // imports
        try (PrintWriter w = new PrintWriter(new FileWriter(out + "/imports.csv"))) {
            w.println("library,name,callers");
            for (Symbol s : st.getExternalSymbols()) {
                ExternalLocation el = currentProgram.getExternalManager().getExternalLocation(s);
                int refs = 0;
                for (Symbol t : st.getSymbols(s.getName())) refs += t.getReferenceCount();
                w.println(esc(el != null ? el.getLibraryName() : "") + "," + esc(s.getName()) + "," + s.getReferenceCount());
            }
        }
        println("ExportCore done -> " + out);
    }

    List<Namespace> iterNamespaces(SymbolTable st) {
        List<Namespace> l = new ArrayList<>();
        for (Symbol s : st.getDefinedSymbols()) {
            if (s.getSymbolType() == SymbolType.CLASS || s.getSymbolType() == SymbolType.NAMESPACE) {
                Object o = s.getObject();
                if (o instanceof Namespace) l.add((Namespace) o);
            }
        }
        // classes declared only via SymbolType.CLASS from getClassNamespaces
        Iterator<GhidraClass> ci = st.getClassNamespaces();
        Set<Namespace> seen = new HashSet<>(l);
        while (ci.hasNext()) { GhidraClass c = ci.next(); if (seen.add(c)) l.add(c); }
        return l;
    }
}
