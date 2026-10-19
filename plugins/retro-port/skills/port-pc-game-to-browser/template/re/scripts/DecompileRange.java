// Decompiles every function whose entry lies in [start, end) and writes the C to a file.
// Usage (headless): -postScript DecompileRange.java <startHex> <endHex> <out.c>
//@category Port

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;

import java.io.*;

public class DecompileRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        Address start = sp.getAddress(Long.parseLong(a[0], 16)), end = sp.getAddress(Long.parseLong(a[1], 16));
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        int n = 0;
        try (PrintWriter w = new PrintWriter(new FileWriter(a[2]))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(start, true)) {
                if (f.getEntryPoint().compareTo(end) >= 0) break;
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                w.println("// ==== " + f.getName(true) + " @ " + f.getEntryPoint());
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// FAILED: " + r.getErrorMessage());
                n++;
            }
        }
        di.dispose();
        println("DECOMPILED_RANGE " + n + " functions -> " + a[2]);
    }
}
