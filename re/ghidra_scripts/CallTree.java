// Print the static callee tree below one or more root functions.
// Args: <outFile> <maxDepth> <rootName> [rootName...]
//   rootName is a fully qualified Ghidra name, e.g. Main::main or Game::loop.
// Each function is expanded once; repeats are marked "(seen)". Library/thunk
// functions (std::, Concurrency::, __ helpers) are listed but not expanded.
// @category RON
import java.io.PrintWriter;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class CallTree extends GhidraScript {
	private final Set<Function> expanded = new HashSet<>();
	private PrintWriter out;
	private int maxDepth;

	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		maxDepth = Integer.parseInt(a[1]);
		try (PrintWriter w = new PrintWriter(a[0], "UTF-8")) {
			out = w;
			for (int i = 2; i < a.length; i++) {
				List<Function> roots = findFunctions(a[i]);
				if (roots.isEmpty()) {
					out.println("!! not found: " + a[i]);
				}
				for (Function f : roots) {
					walk(f, 0);
					out.println();
				}
			}
		}
	}

	private List<Function> findFunctions(String qualified) {
		String[] parts = qualified.split("::");
		String simple = parts[parts.length - 1];
		List<Function> found = new ArrayList<>();
		SymbolIterator it = currentProgram.getSymbolTable().getSymbols(simple);
		while (it.hasNext()) {
			Symbol s = it.next();
			Function f = getFunctionAt(s.getAddress());
			if (f != null && f.getName(true).equals(qualified)) {
				found.add(f);
			}
		}
		return found;
	}

	private static boolean isLibrary(String name) {
		return name.startsWith("std::") || name.startsWith("Concurrency::") || name.startsWith("__") ||
			name.startsWith("_") || name.contains("operator_new") || name.contains("operator_delete") ||
			name.startsWith("rapidjson::") || name.startsWith("web::") || name.startsWith("utility::");
	}

	private void walk(Function f, int depth) throws Exception {
		monitor.checkCancelled();
		String name = f.getName(true);
		String indent = "  ".repeat(depth);
		boolean seen = expanded.contains(f);
		out.println(indent + name + "  @" + f.getEntryPoint() + (seen ? "  (seen)" : ""));
		if (seen || depth >= maxDepth || isLibrary(name)) {
			return;
		}
		expanded.add(f);
		// Callees in address order of their call sites, for a readable sequence.
		Set<Function> callees = f.getCalledFunctions(monitor);
		List<Function> ordered = new ArrayList<>(callees);
		ordered.sort(Comparator.comparing(c -> firstCallSite(f, c)));
		for (Function c : ordered) {
			walk(c.isThunk() ? c.getThunkedFunction(true) : c, depth + 1);
		}
	}

	private long firstCallSite(Function caller, Function callee) {
		long best = Long.MAX_VALUE;
		for (var ref : getReferencesTo(callee.getEntryPoint())) {
			if (caller.getBody().contains(ref.getFromAddress())) {
				best = Math.min(best, ref.getFromAddress().getOffset());
			}
		}
		return best;
	}
}
