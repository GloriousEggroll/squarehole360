// Export recovered pseudocode with an explicit per-function status ledger.
// @category GOW3
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.io.BufferedWriter;

public class ExportDecompilation extends GhidraScript {
    private static String clean(String text) {
        return text == null ? "" : text.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException("Usage: output_directory expected_xex_sha256 [limit]");
        }
        if (!args[1].equalsIgnoreCase(currentProgram.getExecutableSHA256())) {
            throw new IllegalStateException("Ghidra program SHA-256 does not match the extracted XEX: "
                + currentProgram.getExecutableSHA256());
        }
        Path out = Path.of(args[0]);
        Path functions = out.resolve("functions");
        Files.createDirectories(functions);
        int limit = args.length > 2 ? Integer.parseInt(args[2]) : Integer.MAX_VALUE;
        int total = currentProgram.getFunctionManager().getFunctionCount();
        Files.writeString(out.resolve("program.txt"),
            "program=" + currentProgram.getName() + "\nsha256=" + currentProgram.getExecutableSHA256()
            + "\nlanguage=" + currentProgram.getLanguageID() + "\nrecognized_functions=" + total
            + "\nstatus=Unverified decompiler pseudocode; not recovered original source.\n",
            StandardCharsets.UTF_8);

        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(false);
        if (!decompiler.openProgram(currentProgram)) {
            throw new IllegalStateException(decompiler.getLastMessage());
        }
        int attempted = 0, completed = 0, failed = 0, warnings = 0;
        try (BufferedWriter ledger = Files.newBufferedWriter(out.resolve("functions.tsv"), StandardCharsets.UTF_8)) {
            ledger.write("address\tname\tbody_bytes\tstatus\tdiagnostic\n");
            FunctionIterator iterator = currentProgram.getFunctionManager().getFunctions(true);
            while (iterator.hasNext() && attempted < limit) {
                monitor.checkCancelled();
                Function function = iterator.next();
                if (function.isExternal()) continue;
                attempted++;
                DecompileResults result = decompiler.decompileFunction(function, 15, monitor);
                String status = "failed";
                String diagnostic = result.getErrorMessage();
                if (result.decompileCompleted() && result.getDecompiledFunction() != null) {
                    String c = result.getDecompiledFunction().getC();
                    boolean warned = c.contains("WARNING:") || !clean(diagnostic).isEmpty();
                    status = warned ? "pseudocode_with_warnings" : "pseudocode";
                    if (warned) warnings++;
                    Files.writeString(functions.resolve(function.getEntryPoint() + ".c"),
                        "/* Unverified Ghidra pseudocode. Address: " + function.getEntryPoint()
                        + ". Not original source or a compilable replacement. */\n" + c,
                        StandardCharsets.UTF_8);
                    completed++;
                } else {
                    failed++;
                }
                ledger.write(function.getEntryPoint() + "\t" + clean(function.getName()) + "\t"
                    + function.getBody().getNumAddresses() + "\t" + status + "\t" + clean(diagnostic) + "\n");
                if (attempted % 500 == 0) {
                    ledger.flush();
                    println("EXPORT_PROGRESS attempted=" + attempted + " completed=" + completed + " failed=" + failed);
                }
            }
        } finally {
            decompiler.dispose();
        }
        String summary = "recognized_functions=" + total + "\nattempted=" + attempted
            + "\npseudocode_emitted=" + completed + "\nfailed=" + failed
            + "\nwith_warnings=" + warnings + "\nvalidated_against_original=0\n";
        Files.writeString(out.resolve("summary.txt"), summary, StandardCharsets.UTF_8);
        println(summary);
    }
}
