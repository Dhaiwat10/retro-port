// Exports every function (entry, name, body ranges, calling convention, stack purge) and every
// indirect jump with its resolved targets (switch tables) as JSON for the recompiler.
// Usage (headless): -postScript ExportFunctions.java <out.json>
//@category Port

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

import java.io.*;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = getScriptArgs().length > 0 ? getScriptArgs()[0] : "functions.json";
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        int nf = 0, nj = 0;
        try (PrintWriter w = new PrintWriter(new BufferedWriter(new FileWriter(outPath)))) {
            w.println("{\"functions\": [");
            boolean first = true;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isThunk() && f.isExternal()) continue;
                StringBuilder ranges = new StringBuilder();
                for (AddressRange r : f.getBody()) {
                    if (ranges.length() > 0) ranges.append(",");
                    ranges.append("[").append(r.getMinAddress().getOffset()).append(",")
                          .append(r.getMaxAddress().getOffset() + 1).append("]");
                }
                w.print((first ? "" : ",\n") + String.format(
                        "{\"entry\":%d,\"name\":\"%s\",\"cc\":\"%s\",\"purge\":%d,\"ranges\":[%s]}",
                        f.getEntryPoint().getOffset(), esc(f.getName(true)), esc(f.getCallingConventionName()),
                        f.getStackPurgeSize(), ranges));
                first = false;
                nf++;
            }
            w.println("],\n\"jumptables\": [");
            first = true;
            for (Instruction ins : listing.getInstructions(true)) {
                if (!ins.getFlowType().isJump() || !ins.getFlowType().isComputed()) continue;
                StringBuilder t = new StringBuilder();
                for (Reference r : rm.getReferencesFrom(ins.getAddress())) {
                    if (!r.getReferenceType().isFlow()) continue;
                    if (t.length() > 0) t.append(",");
                    t.append(r.getToAddress().getOffset());
                }
                w.print((first ? "" : ",\n") + String.format("{\"at\":%d,\"targets\":[%s]}",
                        ins.getAddress().getOffset(), t));
                first = false;
                nj++;
            }
            w.println("]}");
        }
        println(String.format("EXPORT functions=%d indirect_jumps=%d -> %s", nf, nj, outPath));
    }

    private static String esc(String s) { return s == null ? "" : s.replace("\\", "\\\\").replace("\"", "\\\""); }
}
