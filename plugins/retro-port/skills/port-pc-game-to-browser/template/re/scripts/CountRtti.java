// Reports how much C++ RTTI structure Ghidra recovered (type descriptors, vftables, class namespaces).
//@category Port

import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;

public class CountRtti extends GhidraScript {
    @Override
    public void run() throws Exception {
        int typeDesc = 0, colocator = 0, vftable = 0, classes = 0;
        for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
            String n = s.getName();
            if (n.contains("RTTI_Type_Descriptor")) typeDesc++;
            else if (n.contains("RTTI_Complete_Object_Locator")) colocator++;
            else if (n.equals("vftable") || n.startsWith("vftable")) vftable++;
            if (s.getSymbolType() == SymbolType.CLASS) classes++;
        }
        println(String.format("RTTI type_descriptors=%d col=%d vftables=%d class_namespaces=%d",
                typeDesc, colocator, vftable, classes));
    }
}
