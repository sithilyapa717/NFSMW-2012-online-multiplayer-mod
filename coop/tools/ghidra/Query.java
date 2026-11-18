// Answers questions about the program from a request file, so no clicking around is needed.
//   request:  <user home>\nfs_ghidra_in\request.txt   (one command per line, # = comment)
//   answer:   <user home>\nfs_ghidra_out\query_out.txt
//
// Commands (addresses are hex, no 0x needed):
//   decomp ADDR       decompile the function containing ADDR (creates one if Ghidra has none)
//   callers ADDR      list everything that references ADDR (code and data) and the function it is in
//   callees ADDR      list functions called by the function containing ADDR
//   dis ADDR [N]      disassemble N instructions (default 40) from ADDR
//   bytes ADDR [N]    hex dump N bytes (default 64)
//   cstr ADDR         read a NUL-terminated string
//   find TEXT         list every string containing TEXT, with who references it
//   findd TEXT        like find, and also decompile the referencing functions (max 6)
//   ops TEXT          list instructions whose text contains TEXT (e.g. 'MOVAPS xmmword ptr [' + '+ 0x110],'), with their function
//   ptr ADDR          find every 4-byte little-endian occurrence of the value ADDR in memory (pointer tables)
//@category NFSMW
import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

public class Query extends GhidraScript {
    private PrintWriter w;
    private DecompInterface di;
    private Memory mem;

    @Override
    public void run() throws Exception {
        File in = new File(new File(System.getProperty("user.home"), "nfs_ghidra_in"), "request.txt");
        File outDir = new File(System.getProperty("user.home"), "nfs_ghidra_out");
        outDir.mkdirs();
        w = new PrintWriter(new File(outDir, "query_out.txt"), "UTF-8");
        di = new DecompInterface();
        di.openProgram(currentProgram);
        mem = currentProgram.getMemory();
        if (!in.exists()) {
            w.println("no request file at " + in);
            w.close();
            return;
        }
        List<String> lines = Files.readAllLines(in.toPath());
        for (String raw : lines) {
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            w.println("\n######## " + line);
            try {
                String[] p = line.split("\\s+", 3);
                String cmd = p[0].toLowerCase();
                String arg = p.length > 1 ? p[1] : "";
                String rest = line.substring(Math.min(line.length(), cmd.length())).trim();
                switch (cmd) {
                    case "decomp": decomp(addr(arg)); break;
                    case "callers": callers(addr(arg)); break;
                    case "callees": callees(addr(arg)); break;
                    case "dis": dis(addr(arg), p.length > 2 ? Integer.parseInt(p[2]) : 40); break;
                    case "bytes": bytes(addr(arg), p.length > 2 ? Integer.parseInt(p[2]) : 64); break;
                    case "cstr": w.println(cstr(addr(arg))); break;
                    case "find": find(rest, false); break;
                    case "findd": find(rest, true); break;
                    case "ops": ops(rest); break;
                    case "ptr": ptr(addr(arg)); break;
                    default: w.println("unknown command " + cmd);
                }
            } catch (Exception e) {
                w.println("error: " + e);
            }
            w.flush();
        }
        w.close();
        println("Wrote " + new File(outDir, "query_out.txt"));
    }

    private Address addr(String s) {
        if (s.startsWith("0x")) s = s.substring(2);
        return toAddr(Long.parseLong(s, 16));
    }

    private Function funcAt(Address a) {
        Function f = getFunctionContaining(a);
        if (f == null) {
            disassemble(a);
            f = createFunction(a, null);
        }
        return f;
    }

    private void decomp(Address a) {
        Function f = funcAt(a);
        if (f == null) {
            w.println("no function could be made at " + a);
            return;
        }
        decompF(f);
    }

    private void decompF(Function f) {
        w.println("---- " + f.getName() + " @ " + f.getEntryPoint() + " (size " + f.getBody().getNumAddresses() + ") ----");
        DecompileResults r = di.decompileFunction(f, 90, monitor);
        if (r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
        else w.println("(decompile failed: " + r.getErrorMessage() + ")");
    }

    private void callers(Address a) {
        Reference[] refs = getReferencesTo(a);
        w.println(refs.length + " references to " + a);
        for (Reference r : refs) {
            Function f = getFunctionContaining(r.getFromAddress());
            w.println("  from " + r.getFromAddress() + " (" + r.getReferenceType() + ")"
                + (f != null ? " in " + f.getName() + " @ " + f.getEntryPoint() : " (not in a function)"));
        }
    }

    private void callees(Address a) {
        Function f = funcAt(a);
        if (f == null) { w.println("no function"); return; }
        Set<Address> seen = new HashSet<>();
        for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            for (Reference r : ins.getReferencesFrom()) {
                if (r.getReferenceType().isCall() && seen.add(r.getToAddress())) {
                    Function t = getFunctionAt(r.getToAddress());
                    w.println("  call " + r.getToAddress() + (t != null ? " " + t.getName() : "") + "   (from " + ins.getAddress() + ")");
                }
            }
        }
    }

    private void dis(Address a, int n) {
        disassemble(a);
        Instruction ins = getInstructionAt(a);
        for (int i = 0; i < n && ins != null; i++) {
            w.println("  " + ins.getAddress() + "  " + ins);
            ins = ins.getNext();
        }
    }

    private void bytes(Address a, int n) throws Exception {
        byte[] b = new byte[n];
        mem.getBytes(a, b);
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) {
            if (i % 16 == 0) sb.append(String.format("\n  %s: ", a.add(i)));
            sb.append(String.format("%02x ", b[i] & 0xff));
        }
        w.println(sb);
    }

    private String cstr(Address a) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 300; i++) {
            int c = mem.getByte(a.add(i)) & 0xff;
            if (c == 0) break;
            sb.append(c >= 32 && c < 127 ? (char) c : '.');
        }
        return a + ": \"" + sb + "\"";
    }

    private void ops(String text) {
        int shown = 0;
        for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
            if (monitor.isCancelled() || shown >= 150) break;
            if (ins.toString().contains(text)) {
                Function f = getFunctionContaining(ins.getAddress());
                w.println("  " + ins.getAddress() + "  " + ins + (f != null ? "   in " + f.getName() + " @ " + f.getEntryPoint() : ""));
                shown++;
            }
        }
        if (shown == 0) w.println("   no match");
    }

    private void ptr(Address target) throws Exception {
        long v = target.getOffset();
        byte[] pat = new byte[] {(byte) v, (byte) (v >> 8), (byte) (v >> 16), (byte) (v >> 24)};
        Address from = currentProgram.getMinAddress();
        int shown = 0;
        while (from != null && shown < 40 && !monitor.isCancelled()) {
            Address hit = mem.findBytes(from, pat, null, true, monitor);
            if (hit == null) break;
            Function f = getFunctionContaining(hit);
            w.println("  " + hit + (f != null ? "  in " + f.getName() : "  (data)"));
            shown++;
            from = hit.add(1);
        }
        if (shown == 0) w.println("   no match");
    }

    private void find(String text, boolean decompile) throws Exception {
        byte[] pat = text.getBytes("US-ASCII");
        Address from = currentProgram.getMinAddress();
        int shown = 0, decomped = 0;
        Set<Address> done = new HashSet<>();
        while (from != null && shown < 80 && !monitor.isCancelled()) {
            Address hit = mem.findBytes(from, pat, null, true, monitor);
            if (hit == null) break;
            Address s = hit;
            for (int i = 0; i < 200; i++) { // walk back to the start of the string
                Address prev = s.subtract(1);
                if (mem.getByte(prev) == 0) break;
                s = prev;
            }
            w.println(cstr(s));
            shown++;
            for (Reference r : getReferencesTo(s)) {
                Function f = getFunctionContaining(r.getFromAddress());
                w.println("    ref from " + r.getFromAddress() + (f != null ? " in " + f.getName() + " @ " + f.getEntryPoint() : " (data)"));
                if (decompile && f != null && decomped < 6 && done.add(f.getEntryPoint())) {
                    decomped++;
                    decompF(f);
                }
            }
            from = hit.add(1);
        }
        if (shown == 0) w.println("   no match");
    }
}
