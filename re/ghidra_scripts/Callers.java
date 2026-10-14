// List the callers (code references to the entry point) of functions matching a regex, recursively.
// Args: <outFile> <maxDepth> <regex> [regex...]
// @category RON
import java.io.PrintWriter;
import java.util.*;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class Callers extends GhidraScript {
	private PrintWriter out;
	private int maxDepth;

	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		maxDepth = Integer.parseInt(a[1]);
		try (PrintWriter w = new PrintWriter(a[0], "UTF-8")) {
			out = w;
			for (int i = 2; i < a.length; i++) {
				Pattern p = Pattern.compile(a[i]);
				for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
					if (p.matcher(f.getName(true)).matches()) {
						walk(f, 0, new HashSet<>());
						out.println();
					}
				}
			}
		}
	}

	private void walk(Function f, int depth, Set<Function> path) {
		out.println("  ".repeat(depth) + f.getName(true) + " @" + f.getEntryPoint());
		if (depth >= maxDepth || !path.add(f)) return;
		Set<Function> seen = new LinkedHashSet<>();
		for (Reference r : getReferencesTo(f.getEntryPoint())) {
			Function c = getFunctionContaining(r.getFromAddress());
			if (c == null) {
				out.println("  ".repeat(depth + 1) + "<" + r.getReferenceType() + " from " + r.getFromAddress() + ">");
			} else if (seen.add(c)) {
				walk(c, depth + 1, path);
			}
		}
		path.remove(f);
	}
}
