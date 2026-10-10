import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.TreeMap;

/** Report imported-function callers and argument evidence from an analyzed ELF. */
public class I386ImportAudit extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2)
            throw new IllegalArgumentException("report-path [--assembly] import-name ...");
        boolean assembly = Arrays.asList(args).contains("--assembly");
        Set<String> names = new LinkedHashSet<>();
        for (int i = 1; i < args.length; ++i)
            if (!args[i].equals("--assembly")) names.add(args[i]);
        if (names.isEmpty()) throw new IllegalArgumentException("Need imported-function names");
        TreeMap<Address, Function> targets = new TreeMap<>();
        var functions = currentProgram.getFunctionManager().getFunctions(true);
        while (functions.hasNext()) {
            Function function = functions.next();
            Function target = function.isThunk() ? function.getThunkedFunction(true) : function;
            if (names.contains(function.getName()) || target != null && names.contains(target.getName()))
                targets.put(function.getEntryPoint(), function);
        }
        DecompInterface decompiler = new DecompInterface();
        if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException("Cannot open decompiler");
        try (PrintWriter out = new PrintWriter(args[0])) {
            out.println("module=" + currentProgram.getName() + " sha256=" + currentProgram.getExecutableSHA256());
            out.println("imageBase=" + currentProgram.getImageBase());
            TreeMap<Address, Function> callers = new TreeMap<>();
            for (Function target : targets.values()) {
                out.println("import=" + target.getName() + " entry=" + target.getEntryPoint());
                var references = currentProgram.getReferenceManager().getReferencesTo(target.getEntryPoint());
                while (references.hasNext()) {
                    Reference reference = references.next();
                    Address from = reference.getFromAddress();
                    Function caller = currentProgram.getFunctionManager().getFunctionContaining(from);
                    out.println("reference=" + from + " type=" + reference.getReferenceType() +
                                " caller=" + (caller == null ? "data/undefined" : caller.getName()));
                    if (caller != null && !targets.containsKey(caller.getEntryPoint()))
                        callers.put(caller.getEntryPoint(), caller);
                }
            }
            if (targets.isEmpty()) out.println("No matching imported functions in the analyzed listing");
            for (Function caller : callers.values()) {
                monitor.checkCancelled();
                out.println("=== caller=" + caller.getName() + " entry=" + caller.getEntryPoint() + " ===");
                DecompileResults result = decompiler.decompileFunction(caller, 120, monitor);
                if (!result.decompileCompleted())
                    throw new IllegalStateException("Cannot decompile " + caller.getName() + ": " + result.getErrorMessage());
                String[] lines = result.getDecompiledFunction().getC().split("\n");
                boolean[] selected = new boolean[lines.length];
                boolean matched = false;
                for (int line = 0; line < lines.length; ++line) {
                    for (String name : names) {
                        if (!lines[line].contains(name)) continue;
                        matched = true;
                        for (int nearby = Math.max(0, line - 6); nearby <= Math.min(lines.length - 1, line + 3); ++nearby)
                            selected[nearby] = true;
                    }
                }
                for (int line = 0; line < lines.length; ++line)
                    if (selected[line]) out.println(lines[line]);
                if (!matched) out.println("No imported-call expression recovered; inspect reference instructions");
                if (assembly) {
                    var instructions = currentProgram.getListing().getInstructions(caller.getBody(), true);
                    while (instructions.hasNext()) {
                        Instruction instruction = instructions.next();
                        for (Reference reference : instruction.getReferencesFrom()) {
                            if (!targets.containsKey(reference.getToAddress())) continue;
                            Instruction first = instruction;
                            for (int i = 0; i < 6 && first.getPrevious() != null &&
                                    caller.getBody().contains(first.getPrevious().getAddress()); ++i)
                                first = first.getPrevious();
                            while (first != null && first.getAddress().compareTo(instruction.getAddress()) <= 0) {
                                out.println(first.getAddress() + " " + first);
                                first = first.getNext();
                            }
                        }
                    }
                }
            }
        } finally {
            decompiler.dispose();
        }
    }
}
