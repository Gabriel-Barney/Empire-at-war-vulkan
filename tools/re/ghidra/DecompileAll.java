// Decompiles every non-thunk function in parallel and writes C grouped by owning
// namespace (class) into <outDir>/<namespace>.c, plus an index CSV.
// Usage (headless postScript): DecompileAll.java <outDir> [timeoutSecs]
// @category EAW
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.decompiler.parallel.*;
import ghidra.program.model.listing.*;
import ghidra.util.task.TaskMonitor;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;

public class DecompileAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String out = a.length > 0 ? a[0] : "/home/claude/work/ghidra/decomp";
        int timeout = a.length > 1 ? Integer.parseInt(a[1]) : 60;
        new File(out).mkdirs();
        long base = currentProgram.getImageBase().getOffset();
        List<Function> funcs = new ArrayList<>();
        for (Function f : currentProgram.getFunctionManager().getFunctions(true))
            if (!f.isThunk() && !f.isExternal()) funcs.add(f);
        println("decompiling " + funcs.size());
        ConcurrentHashMap<String, StringBuffer> byNs = new ConcurrentHashMap<>();
        ConcurrentLinkedQueue<String> index = new ConcurrentLinkedQueue<>();
        DecompilerCallback<Void> cb = new DecompilerCallback<>(currentProgram, new DecompileConfigurer() {
            public void configure(DecompInterface d) {
                DecompileOptions o = new DecompileOptions();
                d.setOptions(o);
                d.toggleCCode(true);
                d.toggleSyntaxTree(false);
                d.setSimplificationStyle("decompile");
            }
        }) {
            public Void process(DecompileResults r, TaskMonitor m) {
                Function f = r.getFunction();
                long rva = f.getEntryPoint().getOffset() - base;
                String ns = f.getParentNamespace().getName(true);
                String code;
                boolean ok = r.decompileCompleted() && r.getDecompiledFunction() != null;
                code = ok ? r.getDecompiledFunction().getC() : ("/* decompile failed: " + r.getErrorMessage() + " */\n");
                String hdr = String.format("\n// ===== %s @ RVA 0x%x =====\n", f.getName(true), rva);
                byNs.computeIfAbsent(ns, k -> new StringBuffer()).append(hdr).append(code);
                index.add(String.format("0x%x,\"%s\",\"%s\",%s", rva, f.getName(), ns, ok));
                return null;
            }
        };
        cb.setTimeout(timeout);
        try {
            ParallelDecompiler.decompileFunctions(cb, funcs, monitor);
        } finally {
            cb.dispose();
        }
        for (Map.Entry<String, StringBuffer> e : byNs.entrySet()) {
            String fn = e.getKey().replaceAll("[^A-Za-z0-9_.-]", "_");
            if (fn.length() > 150) fn = fn.substring(0, 150);
            try (Writer w = new FileWriter(out + "/" + fn + ".c")) { w.write(e.getValue().toString()); }
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(out + "/_index.csv"))) {
            w.println("rva,name,namespace,ok");
            for (String s : index) w.println(s);
        }
        println("DecompileAll done: " + index.size() + " functions, " + byNs.size() + " files");
    }
}
