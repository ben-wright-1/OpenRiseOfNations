// List the functions that reference a symbol (data or code), with the referencing instruction.
// Args: <outFile> <symbolName> [symbolName...]   (names as Ghidra shows them, e.g. camera_scale or Camera::def_scroll_speeds)
// @category RON
import java.io.PrintWriter;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class DataXrefs extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		try (PrintWriter w = new PrintWriter(a[0], "UTF-8")) {
			for (int i = 1; i < a.length; i++) {
				SymbolIterator it = currentProgram.getSymbolTable().getSymbols(a[i].contains("::") ? a[i].substring(a[i].lastIndexOf("::") + 2) : a[i]);
				while (it.hasNext()) {
					Symbol s = it.next();
					if (!s.getName(true).equals(a[i]) && !s.getName().equals(a[i])) continue;
					w.println(s.getName(true) + " @" + s.getAddress());
					for (Reference r : getReferencesTo(s.getAddress())) {
						Function f = getFunctionContaining(r.getFromAddress());
						Instruction ins = getInstructionAt(r.getFromAddress());
						w.println("  " + r.getFromAddress() + "  " + (f == null ? "?" : f.getName(true)) + "  " + r.getReferenceType() + "  " + (ins == null ? "" : ins.toString()));
					}
					w.println();
				}
			}
		}
	}
}
