// Decompiles every function whose name contains the given substring and prints the C.
// Usage (headless): -postScript DecompileMatching.java <substring> [maxFunctions]
//@category Port

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;

public class DecompileMatching extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String needle = args.length > 0 ? args[0] : "fieldteam";
        int max = args.length > 1 ? Integer.parseInt(args[1]) : 3;
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        int n = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!f.getName().contains(needle)) continue;
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            if (!r.decompileCompleted()) {
                println("FAILED " + f.getName() + ": " + r.getErrorMessage());
                continue;
            }
            println("==== " + f.getName() + " @ " + f.getEntryPoint() + "\n" + r.getDecompiledFunction().getC());
            if (++n >= max) break;
        }
        di.dispose();
        println("DECOMPILED " + n);
    }
}
