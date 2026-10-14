// Per-root static callee closure plus the named globals each reached function references.
// Args: <outFile> <maxDepth> <root> [root...]
//   root is a hex address (e.g. 9e1890 or 0x009e1890) or a fully qualified name (Class::method).
// Unlike CallTree, every root gets its own visited set, so each root's closure is complete to maxDepth.
// Library helpers (std::, __*, _*) and hub classes listed in STOP are recorded but not expanded.
// Output, tab separated, one line per (root, item):
//   <rootAddr> F <depth> <function name> <addr>      reached function (depth 0 = root)
//   <rootAddr> D <depth> <global symbol name> <addr> named non-function symbol read or written by a function at <depth>
// @category RON
import java.io.PrintWriter;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class CalleeClosure extends GhidraScript {
	private static final String[] STOP = { "String::", "Error::", "RunTimeEnv::", "ObjectArray", "PtrArray", "ArrayBase",
		"Recycler", "Stack<", "StringGuts", "operator_new", "operator_delete", "malloc", "free" };

	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		int maxDepth = Integer.parseInt(a[1]);
		try (PrintWriter out = new PrintWriter(a[0], "UTF-8")) {
			for (int i = 2; i < a.length; i++) {
				Function root = resolve(a[i]);
				if (root == null) {
					out.println(a[i] + "\t!\t0\tnot found\t");
					continue;
				}
				walk(out, root, maxDepth);
			}
		}
	}

	private Function resolve(String s) {
		if (s.matches("(0x)?[0-9a-fA-F]+")) {
			Address ad = toAddr(Long.parseLong(s.replaceFirst("^0x", ""), 16));
			Function f = getFunctionAt(ad);
			return f != null ? f : getFunctionContaining(ad);
		}
		String[] parts = s.split("::");
		SymbolIterator it = currentProgram.getSymbolTable().getSymbols(parts[parts.length - 1]);
		while (it.hasNext()) {
			Symbol sym = it.next();
			Function f = getFunctionAt(sym.getAddress());
			if (f != null && f.getName(true).equals(s)) return f;
		}
		return null;
	}

	private static boolean stop(String name) {
		if (name.startsWith("std::") || name.startsWith("__") || name.startsWith("_")) return true;
		for (String p : STOP) if (name.startsWith(p) || name.contains(p)) return true;
		return false;
	}

	private void walk(PrintWriter out, Function root, int maxDepth) throws Exception {
		String r = root.getEntryPoint().toString();
		Map<Function, Integer> depth = new LinkedHashMap<>();
		ArrayDeque<Function> q = new ArrayDeque<>();
		depth.put(root, 0);
		q.add(root);
		Set<String> globals = new HashSet<>();
		while (!q.isEmpty()) {
			monitor.checkCancelled();
			Function f = q.poll();
			int d = depth.get(f);
			String name = f.getName(true);
			out.println(r + "\tF\t" + d + "\t" + name + "\t" + f.getEntryPoint());
			// Named globals referenced from this function's body.
			for (Address ad : f.getBody().getAddresses(true)) {
				for (Reference ref : getReferencesFrom(ad)) {
					if (!ref.isMemoryReference() || ref.getReferenceType().isFlow()) continue;
					Symbol s = getSymbolAt(ref.getToAddress());
					if (s == null || s.getSource() == SourceType.DEFAULT || getFunctionAt(ref.getToAddress()) != null) continue;
					String gn = s.getName(true);
					if (gn.startsWith("u_") || gn.startsWith("s_") || gn.startsWith("DAT_") || gn.startsWith("PTR_") || gn.startsWith("switchD")
						|| gn.startsWith("LAB_") || gn.startsWith("caseD")) continue;
					if (globals.add(gn)) out.println(r + "\tD\t" + d + "\t" + gn + "\t" + ref.getToAddress());
				}
			}
			if (d >= maxDepth || (d > 0 && stop(name))) continue;
			for (Function c : f.getCalledFunctions(monitor)) {
				Function t = c.isThunk() ? c.getThunkedFunction(true) : c;
				if (t != null && !depth.containsKey(t)) {
					depth.put(t, d + 1);
					q.add(t);
				}
			}
		}
	}
}
