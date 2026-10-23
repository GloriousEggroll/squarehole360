// Export references to campaign-loading strings from the analysis database.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;
import java.nio.charset.Charset;
import java.nio.file.Files;
import java.nio.file.Path;
import ghidra.program.model.mem.MemoryBlock;

public class FindLoadingReferences extends GhidraScript {
  public void run() throws Exception {
    String[] terms = {"PrepareMapChange", "CommitMapChange", "LoadMap", "UpdateLevelStreaming",
        "SP_Nightmare", "GearGame_P", "FlushLevelStreaming", "RequestLevel",
        "Failed to load", "Couldn't find file", "StreamingLevels"};
    try (PrintWriter out = new PrintWriter(getScriptArgs()[0])) {
      for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
        out.printf("block %s %s %x%n", block.getName(), block.getStart(), block.getSize());
        if (block.isInitialized()) {
          byte[] data = new byte[(int)block.getSize()];
          block.getBytes(block.getStart(), data);
          Files.write(Path.of(getScriptArgs()[0] + "." + block.getStart() + ".bin"), data);
        }
      }
      for (String term : terms) {
        for (String encoding : new String[]{"UTF-8", "UTF-16BE"}) {
          byte[] bytes = term.getBytes(Charset.forName(encoding));
          Address pos = currentProgram.getMinAddress();
          while (pos != null) {
            Address found = currentProgram.getMemory().findBytes(pos, bytes, null, true, monitor);
            if (found == null) break;
            out.printf("%s %s %s%n", term, encoding, found);
            int unit = encoding.equals("UTF-16BE") ? 2 : 1;
            Address start = found;
            for (int back = 0; back < 256; back++) {
              Address previous = start.subtract(unit);
              if ((unit == 1 ? currentProgram.getMemory().getByte(previous) : currentProgram.getMemory().getShort(previous)) == 0) break;
              start = previous;
            }
            byte[] preview = new byte[256];
            currentProgram.getMemory().getBytes(start, preview);
            out.printf("  start %s %s%n", start, new String(preview, Charset.forName(encoding)).split("\u0000")[0]);
            for (Reference ref : currentProgram.getReferenceManager().getReferencesTo(found)) {
              Function fn = currentProgram.getFunctionManager().getFunctionContaining(ref.getFromAddress());
              out.printf("  from %s function %s%n", ref.getFromAddress(), fn == null ? "?" : fn.getEntryPoint());
            }
            pos = found.add(1);
          }
        }
      }
    }
  }
}
