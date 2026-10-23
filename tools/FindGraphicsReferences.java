// Read-only graphics entry-point survey of the verified retail executable.
// @category GOW3
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashSet;
import java.util.Set;

public class FindGraphicsReferences extends GhidraScript {
  private final Set<Function> functions = new LinkedHashSet<>();

  private void references(PrintWriter out, String label, Address address) {
    for (Reference ref : currentProgram.getReferenceManager().getReferencesTo(address)) {
      Function fn = getFunctionContaining(ref.getFromAddress());
      out.printf("reference\t%s\t%s\t%s\t%s\t%s%n", label, address,
          ref.getFromAddress(), fn == null ? "unknown" : fn.getEntryPoint(), ref.getReferenceType());
      if (fn != null) functions.add(fn);
    }
  }

  @Override public void run() throws Exception {
    if (!currentProgram.getExecutableSHA256().equalsIgnoreCase(
        "4e5d00b8f584f42951344b26a2ae8292bb30c7634b1a75cec0c70a80b3f8f790")) {
      throw new IllegalStateException("Unexpected executable SHA-256");
    }
    Path output = Path.of(getScriptArgs()[0]);
    Files.createDirectories(output);
    try (PrintWriter out = new PrintWriter(output.resolve("references.tsv").toFile())) {
      for (String term : new String[]{"XeD3DCommands.cpp", "SetRenderTarget",
          "Unknown RHI vertex element", "FSetCompressedShaderCache", "FRemoveCompressedShaderCache",
          "TMotionBlurGatherPixelShader", "MainGatherMotionBlur", "MotionBlurMaskScaleAndBias"}) {
        for (var encoding : new java.nio.charset.Charset[]{StandardCharsets.UTF_8, StandardCharsets.UTF_16BE}) {
          Address pos = currentProgram.getMinAddress();
          while (pos != null) {
            monitor.checkCancelled();
            Address found = currentProgram.getMemory().findBytes(pos, term.getBytes(encoding), null, true, monitor);
            if (found == null) break;
            out.printf("string\t%s\t%s\t%s%n", term, encoding, found);
            references(out, term, found);
            int unit = encoding == StandardCharsets.UTF_16BE ? 2 : 1;
            Address start = found;
            for (int i = 0; i < 512 && currentProgram.getMemory().contains(start.subtract(unit)); i++) {
              Address prev = start.subtract(unit);
              if ((unit == 1 ? getByte(prev) : getShort(prev)) == 0) break;
              start = prev;
            }
            if (!start.equals(found)) references(out, term + "-start", start);
            pos = found.add(1);
          }
        }
      }
      // Known kernel graphics imports and call sites, from static recompilation.
      for (long addr : new long[]{0x82fc8fbcL, 0x82fc8f8cL, 0x82fc90bcL, 0x82af6e28L, 0x82af65d8L}) {
        Address a = toAddr(addr);
        references(out, "graphics-call", a);
        Function fn = getFunctionContaining(a);
        if (fn != null) functions.add(fn);
      }
      // One level of callers identifies the engine/library boundary.
      for (Function fn : functions.toArray(new Function[0])) {
        references(out, "caller", fn.getEntryPoint());
      }
      for (Function fn : functions) {
        out.printf("function\t%s\t%s\t%d%n", fn.getEntryPoint(), fn.getName(), fn.getBody().getNumAddresses());
        try (PrintWriter asm = new PrintWriter(output.resolve(fn.getEntryPoint() + ".asm").toFile())) {
          for (Instruction ins : currentProgram.getListing().getInstructions(fn.getBody(), true)) {
            asm.printf("%s\t%s%n", ins.getAddress(), ins);
          }
        }
      }
      out.printf("count\t%d%n", functions.size());
    }
    println("Graphics references exported to " + output);
  }
}
