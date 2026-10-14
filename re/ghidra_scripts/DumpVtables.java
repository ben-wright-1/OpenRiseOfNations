// Dump virtual function tables of the given classes in slot order.
// Args: <outFile> <ClassName> [ClassName...]
// Finds every "vftable" symbol in each class namespace (one per base subobject)
// and lists consecutive pointers that land on functions.
// @category RON
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class DumpVtables extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		try (PrintWriter out = new PrintWriter(a[0], "UTF-8")) {
			for (int i = 1; i < a.length; i++) {
				dump(out, a[i]);
			}
		}
	}

	private void dump(PrintWriter out, String cls) throws Exception {
		boolean any = false;
		SymbolIterator it = currentProgram.getSymbolTable().getSymbols("vftable");
		while (it.hasNext()) {
			Symbol s = it.next();
			String ns = s.getParentNamespace().getName(true);
			if (!ns.equals(cls)) {
				continue;
			}
			any = true;
			out.println("== " + cls + " vftable @" + s.getAddress());
			Address p = s.getAddress();
			for (int slot = 0; slot < 1024; slot++) {
				Address target = toAddr(getInt(p) & 0xffffffffL);
				Function f = getFunctionAt(target);
				if (f == null) {
					break;
				}
				if (f.isThunk()) {
					f = f.getThunkedFunction(true);
				}
				out.printf("%4d  +0x%03x  %s  @%s  %s%n", slot, slot * 4, f.getName(true), target,
					f.getSignature().getPrototypeString());
				p = p.add(4);
				if (slot > 0 && getSymbolAt(p) != null && getSymbolAt(p).getName().contains("meta")) {
					break;
				}
			}
			out.println();
		}
		if (!any) {
			out.println("!! no vftable for " + cls);
		}
	}
}
