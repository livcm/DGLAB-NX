// Decompile the function at each address, creating it first when the analyzer
// never did.
//
// The IPC command cases of a service dispatch are reached through a byte table
// plus a branch table, so the analyzer sees them as unreferenced code and does
// not turn them into functions. That is exactly where the per-command payload
// shape lives (what the firmware reads out of the request), so the cases have to
// be decompiled one address at a time.
//
// usage:
//   analyzeHeadless <projdir> <proj> -process module.elf -noanalysis \
//     -postScript DecompileForce.java 0x1efd0 0x1f420
import java.io.BufferedWriter;
import java.io.FileWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileForce extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length < 1) {
            println("usage: DecompileForce.java <address> [address...]");
            return;
        }

        // The first argument may be an output file; addresses are the rest.
        boolean toFile = !arguments[0].startsWith("0x");
        BufferedWriter writer = null;
        int first = 0;

        if (toFile) {
            writer = new BufferedWriter(new FileWriter(arguments[0]));
            first = 1;
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(false);
        decompiler.openProgram(currentProgram);

        try {
            for (int i = first; i < arguments.length; i++) {
                Address address = toAddr(Long.decode(arguments[i]));

                disassemble(address);

                Function function = getFunctionAt(address);
                if (function == null)
                    function = createFunction(address, null);
                if (function == null)
                    function = getFunctionContaining(address);
                if (function == null) {
                    emit(writer, "=== " + address + ": no function\n");
                    continue;
                }

                emit(writer, "// ---- " + function.getName() + " @ "
                        + function.getEntryPoint() + " (asked " + address + ") ----\n");

                DecompileResults results =
                        decompiler.decompileFunction(function, 120, monitor);
                if (results == null || !results.decompileCompleted())
                    emit(writer, "// decompilation failed\n\n");
                else {
                    emit(writer, results.getDecompiledFunction().getC());
                    emit(writer, "\n");
                }
            }
        }
        finally {
            decompiler.dispose();
            if (writer != null)
                writer.close();
        }
    }

    private void emit(BufferedWriter writer, String text) throws Exception {
        if (writer == null)
            println(text);
        else
            writer.write(text);
    }
}
