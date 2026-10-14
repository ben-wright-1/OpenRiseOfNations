// Print instructions in address ranges (whether or not a function is defined there).
// Args: <outFile> <startHex>-<endHex> [...]
// @category RON
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class DisasmRange extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		try (PrintWriter w = new PrintWriter(a[0], "UTF-8")) {
			for (int i = 1; i < a.length; i++) {
				String[] r = a[i].split("-");
				Address s = toAddr(Long.parseLong(r[0], 16)), e = toAddr(Long.parseLong(r[1], 16));
				w.println("== " + a[i] + "  (function: " + (getFunctionContaining(s) == null ? "none" : getFunctionContaining(s).getName(true)) + ")");
				Instruction ins = getInstructionAt(s);
				if (ins == null) ins = getInstructionAfter(s);
				while (ins != null && ins.getAddress().compareTo(e) <= 0) {
					StringBuilder line = new StringBuilder(ins.getAddress() + "  " + ins);
					for (Reference ref : ins.getReferencesFrom()) {
						Symbol sym = getSymbolAt(ref.getToAddress());
						if (sym != null) line.append("    ; ").append(sym.getName(true));
						else if (ref.getReferenceType().isData()) {
							try { int v = getInt(ref.getToAddress()); line.append(String.format("    ; [%s]=%g", ref.getToAddress(), Float.intBitsToFloat(v))); } catch (Exception x) { }
						}
					}
					w.println(line);
					ins = ins.getNext();
				}
			}
		}
	}
}
