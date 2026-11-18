// Finds game strings (script/property names) in the program, follows every
// reference to them, and writes the decompiled C of the functions involved.
// Output: <user home>\nfs_ghidra_out\string_refs.txt
//@category NFSMW
import java.io.File;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

public class DumpStringRefs extends GhidraScript {
    private static final String[] TARGETS = {
        "SpawnNetworkCar", "SpawnVehicle", "AI racer count", "RacerCount", "NumOpponents",
        "GetPlayerVehicleIndex", "GameModeManager", "RequestRespawnAtEventStart", "StartGameModeTimer"
    };
    private static final int MAX_FUNCS_PER_STRING = 12;

    @Override
    public void run() throws Exception {
        File out = new File(System.getProperty("user.home"), "nfs_ghidra_out");
        out.mkdirs();
        PrintWriter w = new PrintWriter(new File(out, "string_refs.txt"), "UTF-8");
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        Memory mem = currentProgram.getMemory();
        Set<Address> done = new HashSet<>();

        w.println("functions in program: " + currentProgram.getFunctionManager().getFunctionCount());
        for (String t : TARGETS) {
            w.println("\n==================================================================");
            w.println("STRING \"" + t + "\"");
            byte[] pat = new byte[t.length() + 2];
            System.arraycopy(t.getBytes("US-ASCII"), 0, pat, 1, t.length()); // 0 + text + 0
            Address from = currentProgram.getMinAddress();
            int found = 0, funcs = 0;
            while (from != null && !monitor.isCancelled()) {
                Address hit = mem.findBytes(from, pat, null, true, monitor);
                if (hit == null) break;
                Address str = hit.add(1);
                found++;
                w.println("-- string at " + str);
                Reference[] refs = getReferencesTo(str);
                if (refs.length == 0) w.println("   (no references found)");
                for (Reference r : refs) {
                    Address ra = r.getFromAddress();
                    Function f = getFunctionContaining(ra);
                    w.println("   referenced from " + ra + (f != null ? " in " + f.getName() + " @ " + f.getEntryPoint() : " (data, not in a function)"));
                    if (f != null) {
                        if (funcs < MAX_FUNCS_PER_STRING && done.add(f.getEntryPoint())) {
                            funcs++;
                            dump(di, f, w);
                        }
                    } else {
                        // probably a table: {name, handler, ...}. Show neighbouring dwords and decompile code pointers.
                        for (int k = -3; k <= 3; k++) {
                            try {
                                Address q = ra.add(k * 4L);
                                long v = mem.getInt(q) & 0xffffffffL;
                                Address tgt = toAddr(v);
                                Function tf = tgt == null ? null : getFunctionAt(tgt);
                                w.println(String.format("      [%+d] %s = 0x%08x%s", k * 4, q, v, tf != null ? "  -> function " + tf.getName() : ""));
                                if (tf != null && funcs < MAX_FUNCS_PER_STRING && done.add(tf.getEntryPoint())) {
                                    funcs++;
                                    dump(di, tf, w);
                                }
                            } catch (Exception e) { /* unreadable neighbour */ }
                        }
                    }
                }
                from = hit.add(1);
            }
            if (found == 0) w.println("   not found");
        }
        w.close();
        println("Wrote " + new File(out, "string_refs.txt"));
    }

    private void dump(DecompInterface di, Function f, PrintWriter w) {
        w.println("\n  ---- decompiled " + f.getName() + " @ " + f.getEntryPoint() + " ----");
        try {
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            if (r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
            else w.println("  (decompile failed: " + r.getErrorMessage() + ")");
        } catch (Exception e) {
            w.println("  (decompile error: " + e + ")");
        }
    }
}
