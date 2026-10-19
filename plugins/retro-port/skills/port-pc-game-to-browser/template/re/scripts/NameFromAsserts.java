// Names unnamed functions after the source file/line in the assert strings they reference,
// e.g. a function using ".\src\fielding\fieldteam.cpp(412)" becomes fieldteam_cpp_412.
// Also reports RTTI-derived class/vtable counts. Writes a CSV of every rename.
//@category Port

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;

import java.io.*;
import java.util.*;
import java.util.regex.*;

public class NameFromAsserts extends GhidraScript {
    private static final Pattern SRC = Pattern.compile("([A-Za-z0-9_]+)\\.(cpp|c|h|hpp)\\((\\d+)\\)");

    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        Map<Function, String> best = new HashMap<>();
        Map<Function, Integer> bestLine = new HashMap<>();
        int strings = 0;

        for (Data d : listing.getDefinedData(true)) {
            if (monitor.isCancelled()) break;
            if (!d.hasStringValue()) continue;
            String s = StringDataInstance.getStringDataInstance(d).getStringValue();
            if (s == null) continue;
            Matcher m = SRC.matcher(s);
            if (!m.find()) continue;
            strings++;
            String base = m.group(1) + "_" + m.group(2);
            int line = Integer.parseInt(m.group(3));
            for (Reference r : rm.getReferencesTo(d.getAddress())) {
                Function f = fm.getFunctionContaining(r.getFromAddress());
                if (f == null) continue;
                Integer prev = bestLine.get(f);
                if (prev == null || line < prev) {
                    best.put(f, base + "_" + line);
                    bestLine.put(f, line);
                }
            }
        }

        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : "assert_names.csv");
        int renamed = 0;
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            w.println("address,old_name,new_name");
            for (Map.Entry<Function, String> e : best.entrySet()) {
                Function f = e.getKey();
                if (f.getSymbol().getSource() != SourceType.DEFAULT) continue;  // keep real names
                String old = f.getName();
                f.setName(e.getValue(), SourceType.ANALYSIS);
                w.println(f.getEntryPoint() + "," + old + "," + e.getValue());
                renamed++;
            }
        }

        int total = fm.getFunctionCount();
        int named = 0;
        for (Function f : fm.getFunctions(true)) {
            if (f.getSymbol().getSource() != SourceType.DEFAULT) named++;
        }
        int vtables = 0;
        SymbolIterator it = currentProgram.getSymbolTable().getSymbolIterator("vftable", true);
        while (it.hasNext()) { it.next(); vtables++; }

        println(String.format("SUMMARY assert_strings=%d renamed_from_asserts=%d functions=%d named=%d vftables=%d",
                strings, renamed, total, named, vtables));
    }
}
