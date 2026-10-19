// Decompiles the function containing each given address and prints it.
// Usage (headless): -postScript DecompileContaining.java <hexAddr> [hexAddr...]
//@category Port

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;

public class DecompileContaining extends GhidraScript {
    @Override
    public void run() throws Exception {
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String s : getScriptArgs()) {
            Address a = sp.getAddress(Long.parseLong(s, 16));
            Function f = currentProgram.getFunctionManager().getFunctionContaining(a);
            if (f == null) { println("==== no function contains " + a); continue; }
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            println("==== " + f.getName(true) + " @ " + f.getEntryPoint() + " (contains " + a + ")\n"
                    + (r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// FAILED: " + r.getErrorMessage()));
        }
        di.dispose();
    }
}
