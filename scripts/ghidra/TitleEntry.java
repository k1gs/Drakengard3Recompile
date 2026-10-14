// Bounded exploration of the entry function; output stays under ignored work/.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import java.nio.file.Files;
import java.nio.file.Path;

public class TitleEntry extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        var start = toAddr(args[0]);
        disassemble(start);
        var fn = getFunctionAt(start);
        if (fn == null) fn = createFunction(start, "title_startup_candidate");
        if (fn == null) throw new IllegalStateException("No function at " + start);
        StringBuilder text = new StringBuilder();
        text.append("Language: ").append(currentProgram.getLanguageID()).append("\n");
        text.append("NOTE: PS3 OPD/TOC and ABI are not automatically validated by this import.\n");
        var instructions = currentProgram.getListing().getInstructions(fn.getBody(), true);
        int count = 0;
        while (instructions.hasNext() && count++ < 500) {
            var ins = instructions.next();
            text.append(ins.getAddress()).append("  ").append(ins).append("\n");
        }
        DecompInterface decompiler = new DecompInterface();
        try {
            if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException("Decompiler unavailable");
            var result = decompiler.decompileFunction(fn, 30, monitor);
            if (!result.decompileCompleted()) throw new IllegalStateException(result.getErrorMessage());
            text.append("\nDECOMPILER HYPOTHESIS (not restored source):\n");
            text.append(result.getDecompiledFunction().getC());
        } finally { decompiler.dispose(); }
        Files.writeString(Path.of(args[1]), text.toString());
        println("TITLE_ENTRY_OK " + start + " instructions=" + count);
    }
}
