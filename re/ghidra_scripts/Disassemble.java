// Dump the disassembly of functions whose fully qualified name matches a regex, one file each.
// Useful where the decompiler loses x87/SSE register arguments (e.g. TexArray::add(u, v)).
// Args: <outDir> <regex> [regex...]   Files are named <Class__method>@<address>.asm
// Each line: address, instruction, referenced symbols, and the float value of 4-byte data refs.
// @category RON
import java.io.File;
import java.io.PrintWriter;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class Disassemble extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		File outDir = new File(a[0]);
		outDir.mkdirs();
		Pattern[] pats = new Pattern[a.length - 1];
		for (int i = 1; i < a.length; i++) {
			pats[i - 1] = Pattern.compile(a[i]);
		}
		Memory mem = currentProgram.getMemory();
		int count = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			monitor.checkCancelled();
			String name = f.getName(true);
			boolean match = false;
			for (Pattern p : pats) {
				if (p.matcher(name).matches()) {
					match = true;
					break;
				}
			}
			if (!match || f.isThunk()) {
				continue;
			}
			String fileName = name.replaceAll("[^A-Za-z0-9_]+", "_") + "@" + f.getEntryPoint() + ".asm";
			try (PrintWriter w = new PrintWriter(new File(outDir, fileName), "UTF-8")) {
				w.println("; " + name + " @ " + f.getEntryPoint() + "  " + f.getSignature());
				for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
					StringBuilder line = new StringBuilder(ins.getAddress() + "  " + ins);
					for (Reference ref : ins.getReferencesFrom()) {
						Address to = ref.getToAddress();
						Symbol s = getSymbolAt(to);
						if (s != null) line.append("    ; ").append(s.getName(true));
						if (to.isMemoryAddress() && !ref.getReferenceType().isFlow()) {
							try {
								int bits = mem.getInt(to);
								float fv = Float.intBitsToFloat(bits);
								if (Float.isFinite(fv) && fv != 0f && Math.abs(fv) < 1e9 && Math.abs(fv) > 1e-9) {
									line.append("  {f=").append(fv).append("}");
								}
								long lb = mem.getLong(to);
								double dv = Double.longBitsToDouble(lb);
								if (Double.isFinite(dv) && dv != 0 && Math.abs(dv) < 1e9 && Math.abs(dv) > 1e-9) {
									line.append("  {d=").append(dv).append("}");
								}
							} catch (Exception e) {
								// unreadable
							}
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
