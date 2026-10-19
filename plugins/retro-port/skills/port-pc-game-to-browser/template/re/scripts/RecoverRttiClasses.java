// Recovers C++ classes from MSVC 7.1 RTTI, which Ghidra's RTTI analyzer fails to link in this binary.
// For each RTTI Complete Object Locator (COL): creates the class namespace, labels its vftable,
// creates/names virtual functions (vfunc_N) and records base classes from the hierarchy descriptor.
// Usage (headless): -postScript RecoverRttiClasses.java <out.csv>
//@category Port

import ghidra.app.script.GhidraScript;
import ghidra.app.util.NamespaceUtils;
import ghidra.app.util.demangler.DemangledObject;
import ghidra.app.util.demangler.DemanglerUtil;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.util.*;

public class RecoverRttiClasses extends GhidraScript {
    private Memory mem;
    private AddressSpace space;
    private MemoryBlock text;

    private Address addr(long v) { return space.getAddress(v & 0xffffffffL); }
    private long u32(long va) throws Exception { return mem.getInt(addr(va)) & 0xffffffffL; }
    private boolean inText(long va) { return text.contains(addr(va)); }
    private boolean inData(long va) {
        MemoryBlock b = mem.getBlock(addr(va));
        return b != null && b.isInitialized() && b != text;
    }

    @Override
    public void run() throws Exception {
        mem = currentProgram.getMemory();
        space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        text = mem.getBlock(".text");

        // 1. Index every aligned dword in non-code initialized blocks: value -> locations.
        Map<Long, List<Long>> ptrs = new HashMap<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || b == text) continue;
            long start = b.getStart().getOffset(), end = b.getEnd().getOffset();
            for (long va = start; va + 3 <= end; va += 4) {
                ptrs.computeIfAbsent(u32(va), k -> new ArrayList<>()).add(va);
            }
        }

        // 2. Type descriptors: mangled ".?AV"/".?AU" name at TD+8.
        Map<Long, String> tds = new HashMap<>();
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || b == text) continue;
            byte[] buf = new byte[(int) b.getSize()];
            b.getBytes(b.getStart(), buf);
            for (int i = 0; i + 4 < buf.length; i++) {
                if (buf[i] == '.' && buf[i + 1] == '?' && buf[i + 2] == 'A' && (buf[i + 3] == 'V' || buf[i + 3] == 'U')) {
                    int j = i;
                    while (j < buf.length && buf[j] != 0) j++;
                    tds.put(b.getStart().getOffset() + i - 8, new String(buf, i, j - i, "latin1"));
                }
            }
        }

        // 3. COLs: {sig=0, offset, cdOffset, pTypeDescriptor, pClassHierarchyDescriptor}.
        //    vftable = (location of a pointer to the COL) + 4.
        List<long[]> cols = new ArrayList<>();  // {col, td, vtable, offset}
        Set<Long> colSet = new HashSet<>();
        for (Map.Entry<Long, String> e : tds.entrySet()) {
            for (long ref : ptrs.getOrDefault(e.getKey(), Collections.emptyList())) {
                long col = ref - 12;
                if (u32(col) != 0 || u32(col + 4) > 0x10000 || u32(col + 8) > 0x10000) continue;
                if (!inData(u32(col + 16))) continue;
                for (long vref : ptrs.getOrDefault(col, Collections.emptyList())) {
                    cols.add(new long[] { col, e.getKey(), vref + 4, u32(col + 4) });
                    colSet.add(vref);
                }
            }
        }

        // 4. vtable extents: consecutive pointers into .text, stopping at the next COL slot.
        Map<Long, Integer> vtLen = new HashMap<>();
        Map<Long, String> fnOwner = new HashMap<>();
        Map<Long, Integer> fnOwnerLen = new HashMap<>();
        Map<Long, Integer> fnSlot = new HashMap<>();
        for (long[] c : cols) {
            int n = 0;
            for (long va = c[2]; ; va += 4, n++) {
                if (n > 0 && colSet.contains(va)) break;
                long t = u32(va);
                if (!inText(t) || n > 2000) break;
            }
            vtLen.put(c[2], n);
        }

        int classes = 0, vfuncsNamed = 0, fnsCreated = 0;
        Map<Long, Namespace> nsByTd = new HashMap<>();
        Map<String, Namespace> nsByName = new HashMap<>();
        File out = new File(getScriptArgs().length > 0 ? getScriptArgs()[0] : "rtti_classes.csv");
        try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
            w.println("class,mangled,vftable,offset,slots,bases");
            for (long[] c : cols) {
                String mangled = tds.get(c[1]);
                String name = demangle(mangled);
                Namespace ns = nsByTd.get(c[1]);
                if (ns == null) {
                    ns = NamespaceUtils.createNamespaceHierarchy(name, null, currentProgram, SourceType.ANALYSIS);
                    if (!(ns instanceof GhidraClass)) ns = NamespaceUtils.convertNamespaceToClass(ns);
                    nsByTd.put(c[1], ns);
                    nsByName.put(name, ns);
                    classes++;
                }
                String label = c[3] == 0 ? "vftable" : "vftable_" + Long.toHexString(c[3]);
                currentProgram.getSymbolTable().createLabel(addr(c[2]), label, ns, SourceType.ANALYSIS);
                currentProgram.getSymbolTable().createLabel(addr(c[0]), "RTTI_Complete_Object_Locator", ns, SourceType.ANALYSIS);

                int n = vtLen.get(c[2]);
                for (int i = 0; i < n; i++) {
                    long t = u32(c[2] + 4L * i);
                    // Most-base owner wins: the shortest vtable containing the function.
                    if (!fnOwnerLen.containsKey(t) || n < fnOwnerLen.get(t)) {
                        fnOwner.put(t, name);
                        fnOwnerLen.put(t, n);
                        fnSlot.put(t, i);
                    }
                }
                w.println(String.format("\"%s\",%s,%s,%d,%d,\"%s\"", name, mangled,
                        Long.toHexString(c[2]), c[3], n, String.join(";", bases(c[0], tds))));
            }
        }

        // 5. Create/name virtual functions in their owning class namespace.
        FunctionManager fm = currentProgram.getFunctionManager();
        for (Map.Entry<Long, String> e : fnOwner.entrySet()) {
            Address a = addr(e.getKey());
            Function f = fm.getFunctionAt(a);
            if (f == null) {
                disassemble(a);
                f = createFunction(a, null);
                if (f == null) continue;
                fnsCreated++;
            }
            if (f.getSymbol().getSource() != SourceType.DEFAULT) continue;
            Namespace ns = nsByName.get(e.getValue());
            if (ns == null) continue;
            f.setParentNamespace(ns);
            f.setName("vfunc_" + fnSlot.get(e.getKey()), SourceType.ANALYSIS);
            vfuncsNamed++;
        }

        println(String.format("RTTI_RECOVERY type_descriptors=%d cols=%d classes=%d vftables=%d vfuncs_named=%d functions_created=%d",
                tds.size(), new HashSet<>(cols.stream().map(x -> x[0]).toList()).size(), classes, cols.size(), vfuncsNamed, fnsCreated));
    }

    private List<String> bases(long col, Map<Long, String> tds) throws Exception {
        List<String> out = new ArrayList<>();
        long chd = u32(col + 16);
        long num = u32(chd + 8), bca = u32(chd + 12);
        if (num > 64 || !inData(bca)) return out;
        for (int i = 1; i < num; i++) {  // entry 0 is the class itself
            long bcd = u32(bca + 4L * i);
            if (!inData(bcd)) continue;
            String m = tds.get(u32(bcd));
            if (m != null) out.add(demangle(m));
        }
        return out;
    }

    private String demangle(String mangled) {
        try {
            List<DemangledObject> l = DemanglerUtil.demangle(currentProgram, mangled, null);
            if (l != null && !l.isEmpty()) {
                String s = l.get(0).getNamespaceString();
                if (s != null && !s.isEmpty()) return s;
            }
        } catch (Throwable t) { /* fall through */ }
        // Fallback for simple names: ".?AVC@B@A@@" -> "A::B::C"
        String body = mangled.substring(4).replaceAll("@@$", "");
        if (body.contains("?$")) return body.replaceAll("[^A-Za-z0-9_]", "_");
        List<String> parts = new ArrayList<>(Arrays.asList(body.split("@")));
        Collections.reverse(parts);
        return String.join("::", parts);
    }
}
