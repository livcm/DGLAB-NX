// Write the decompiled C of every function in the program to one text file.
//
// usage:
//   analyzeHeadless <projdir> <proj> -process module.elf -postScript ExportDecompiled.java /tmp/ble-re/decompiled.c
//
// Working on the whole listing at once is what makes it practical to find the
// IPC command dispatch: the exported C can be searched for the command table
// shape instead of stepping through disassembly.
import java.io.BufferedWriter;
import java.io.FileWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class ExportDecompiled extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 1) {
            println("usage: ExportDecompiled.java <output.c>");
            return;
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(false);
        decompiler.openProgram(currentProgram);

        int total = 0;
        int failed = 0;

        try (BufferedWriter writer = new BufferedWriter(new FileWriter(arguments[0]))) {
            FunctionIterator functions =
                    currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext() && !monitor.isCancelled()) {
                Function function = functions.next();
                writer.write("// ---- " + function.getName() + " @ "
                        + function.getEntryPoint() + " ----\n");

                DecompileResults results =
                        decompiler.decompileFunction(function, 120, monitor);
                if (results == null || !results.decompileCompleted()) {
                    writer.write("// decompilation failed\n\n");
                    failed++;
                } else {
                    writer.write(results.getDecompiledFunction().getC());
                    writer.write("\n");
                }
                total++;
            }
        }

        decompiler.dispose();
        println("exported " + total + " functions (" + failed + " failed) to "
                + arguments[0]);
    }
}
