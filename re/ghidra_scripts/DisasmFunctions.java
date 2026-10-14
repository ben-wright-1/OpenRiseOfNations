// Disassemble functions whose fully qualified name matches a regex, one file each (.asm).
// Data references are annotated with the symbol and, for 4/8-byte reads, the float/double value.
// Args: <outDir> <regex> [regex...]
// @category RON
import java.io.File;
import java.io.PrintWriter;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class DisasmFunctions extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		File outDir = new File(a[0]);
		outDir.mkdirs();
		Pattern[] pats = new Pattern[a.length - 1];
		for (int i = 1; i < a.length; i++) pats[i - 1] = Pattern.compile(a[i]);
		int count = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			String name = f.getName(true);
			boolean match = false;
			for (Pattern p : pats) if (p.matcher(name).matches()) { match = true; break; }
			if (!match || f.isThunk()) continue;
			String fileName = name.replaceAll("[^A-Za-z0-9_]+", "_") + "@" + f.getEntryPoint() + ".asm";
			try (PrintWriter w = new PrintWriter(new File(outDir, fileName), "UTF-8")) {
				w.println("; " + name + " @ " + f.getEntryPoint());
				for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
					StringBuilder line = new StringBuilder(ins.getAddress() + "  " + ins);
					for (Reference ref : ins.getReferencesFrom()) {
						Address to = ref.getToAddress();
						if (!to.isMemoryAddress()) continue;
						Symbol s = getSymbolAt(to);
						line.append("    ; ").append(to);
						if (s != null) line.append(" ").append(s.getName(true));
						if (ref.getReferenceType().isData()) {
							try {
								int bits = getInt(to);
								line.append(" f=").append(Float.intBitsToFloat(bits));
								long lb = getLong(to);
								double d = Double.longBitsToDouble(lb);
								if (Math.abs(d) > 1e-12 && Math.abs(d) < 1e12) line.append(" d=").append(d);
							} catch (Exception e) { }
						}
					}
					w.println(line);
				}
			}
			count++;
		}
		println("disassembled " + count + " functions into " + outDir);
	}
}
