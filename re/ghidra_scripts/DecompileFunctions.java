// Decompile functions whose fully qualified name matches a regex, one file each.
// Args: <outDir> <regex> [regex...]
//   e.g. "C:/out" "Game::(run|loop|do_frame)" "BH3Method::.*"
// Files are named <Class__method>@<address>.c
// @category RON
import java.io.File;
import java.io.PrintWriter;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class DecompileFunctions extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		File outDir = new File(a[0]);
		outDir.mkdirs();
		Pattern[] pats = new Pattern[a.length - 1];
		for (int i = 1; i < a.length; i++) {
			pats[i - 1] = Pattern.compile(a[i]);
		}

		DecompInterface decomp = new DecompInterface();
		decomp.setOptions(new DecompileOptions());
		decomp.openProgram(currentProgram);
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
			DecompileResults res = decomp.decompileFunction(f, 120, monitor);
			String fileName = name.replaceAll("[^A-Za-z0-9_]+", "_") + "@" + f.getEntryPoint() + ".c";
			try (PrintWriter w = new PrintWriter(new File(outDir, fileName), "UTF-8")) {
				w.println("// " + name + " @ " + f.getEntryPoint());
				if (res.decompileCompleted()) {
					w.println(res.getDecompiledFunction().getC());
				} else {
					// Some PDB types trip the decompiler ("PTRSUB off of non-pointer type"); fall back to the listing.
					w.println("// decompile failed: " + res.getErrorMessage().strip().replace("\n", " "));
					w.println("// disassembly follows");
					for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
						StringBuilder line = new StringBuilder(ins.getAddress() + "  " + ins);
						for (Reference ref : ins.getReferencesFrom()) {
							Symbol s = getSymbolAt(ref.getToAddress());
							if (s != null) line.append("    ; ").append(s.getName(true));
						}
						w.println(line);
					}
				}
			}
			count++;
		}
		decomp.dispose();
		println("decompiled " + count + " functions into " + outDir);
	}
}
