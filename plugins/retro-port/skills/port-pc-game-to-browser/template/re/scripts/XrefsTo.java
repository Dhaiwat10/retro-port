// Prints every reference to the given addresses (with the containing function).
// Usage (headless): -postScript XrefsTo.java <hexAddr> [hexAddr...]
//@category Port

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class XrefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        for (String s : getScriptArgs()) {
            Address a = sp.getAddress(Long.parseLong(s, 16));
            for (Reference r : getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                println("XREF " + a + " <- " + r.getFromAddress() + " " + r.getReferenceType() + " in "
                        + (f == null ? "?" : f.getName(true) + " @ " + f.getEntryPoint()));
            }
        }
    }
}
