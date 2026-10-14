// List every reference to symbols with the given names (globals or functions), with the referencing
// function and instruction, plus the symbol's current 4-byte value as int and float.
// Args: <outFile> <symbolName> [symbolName...]
// @category RON
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class XrefsTo extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		try (PrintWriter w = new PrintWriter(a[0], "UTF-8")) {
			for (int i = 1; i < a.length; i++) {
				SymbolIterator it = currentProgram.getSymbolTable().getSymbols(a[i]);
				while (it.hasNext()) {
					Symbol s = it.next();
					String val = "";
					try {
						int v = getInt(s.getAddress());
						val = String.format(" value=0x%08x (%g)", v, Float.intBitsToFloat(v));
					} catch (Exception e) { }
					w.println("== " + s.getName(true) + " @" + s.getAddress() + val);
					for (Reference r : getReferencesTo(s.getAddress())) {
						Function f = getFunctionContaining(r.getFromAddress());
						Instruction ins = getInstructionAt(r.getFromAddress());
						w.println("  " + r.getFromAddress() + "  " + (f == null ? "?" : f.getName(true)) + "  " + r.getReferenceType()
								+ "  " + (ins == null ? "" : ins.toString()));
					}
				}
			}
		}
	}
}
