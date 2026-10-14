// Synthetic processor smoke check; no game files required.
import ghidra.app.script.GhidraScript;

public class SmokeCheck extends GhidraScript {
    public void run() throws Exception {
        var entry = currentProgram.getMinAddress();
        if (!disassemble(entry) || getInstructionAt(entry) == null) {
            throw new IllegalStateException("Synthetic disassembly failed");
        }
        println("LAB_SMOKE_OK " + currentProgram.getLanguageID() + " " + getInstructionAt(entry));
    }
}
