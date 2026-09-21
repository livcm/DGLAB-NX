// Decompile the function that contains each address given as a script argument.
//
// usage:
//   analyzeHeadless <projdir> <proj> -import module.elf -postScript DecompileAt.java 0x1b5dc 0x41040
//
// Also prints who calls each function, which is how the IPC handler table gets
// traced back to the service registration in docs/ble-re.md.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

public class DecompileAt extends GhidraScript {
    @Override
    protected void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(false);
        decompiler.openProgram(currentProgram);

        FunctionManager functions = currentProgram.getFunctionManager();
        ReferenceManager references = currentProgram.getReferenceManager();

        for (String argument : getScriptArgs()) {
            Address address = toAddr(Long.decode(argument));
            Function function = functions.getFunctionContaining(address);
            if (function == null) {
                println("no function contains " + argument);
                continue;
            }

            println("=== " + argument + " is in " + function.getName()
                    + " @ " + function.getEntryPoint());

            ReferenceIterator callers = references.getReferencesTo(function.getEntryPoint());
            while (callers.hasNext()) {
                Reference reference = callers.next();
                Function caller = functions.getFunctionContaining(reference.getFromAddress());
                println("    caller: " + (caller != null
                        ? caller.getName() + " @ " + caller.getEntryPoint()
                        : reference.getFromAddress().toString()));
            }

            DecompileResults results = decompiler.decompileFunction(function, 120, monitor);
            if (results == null || !results.decompileCompleted()) {
                println("    decompilation failed");
                continue;
            }
            println(results.getDecompiledFunction().getC());
        }

        decompiler.dispose();
    }
}
