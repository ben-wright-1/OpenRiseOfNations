// Dump global data symbols and find scalar uses. One query per argument line:
//   sym:<regex>[:<dwords>]  data symbols whose name matches: address, type, referencing functions, and the
//                           first <dwords> 32-bit values (default: the data type length, max 64). A value that
//                           points at a C string is printed with that string.
//   scalar:<hex>            functions with an instruction whose operand is that scalar (e.g. a string-table offset)
// Args: <outFile> <query> [query...]
// @category RON
import java.io.PrintWriter;
import java.util.*;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataType;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class DumpSymbols extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		try (PrintWriter out = new PrintWriter(a[0], "UTF-8")) {
			for (int i = 1; i < a.length; i++) {
				String q = a[i];
				if (q.startsWith("scalar:")) scalar(out, Long.parseLong(q.substring(7).replace("0x", ""), 16));
				else if (q.startsWith("sym:")) sym(out, q.substring(4));
				else if (q.startsWith("addr:")) addr(out, q.substring(5));
				out.println();
			}
		}
	}

	private void scalar(PrintWriter out, long v) throws Exception {
		out.printf("scalar 0x%x%n", v);
		Set<String> seen = new LinkedHashSet<>();
		for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
			monitor.checkCancelled();
			for (int op = 0; op < ins.getNumOperands(); op++) {
				for (Object o : ins.getOpObjects(op)) {
					if (o instanceof Scalar s && s.getUnsignedValue() == v) {
						Function f = getFunctionContaining(ins.getAddress());
						seen.add((f == null ? "?" : f.getName(true)) + " @" + ins.getAddress() + "  " + ins);
					}
				}
			}
		}
		for (String s : seen) out.println("  " + s);
	}

	private void addr(PrintWriter out, String spec) throws Exception {
		String[] p = spec.split(":");
		Address ad = toAddr(Long.parseLong(p[0].replace("0x", ""), 16));
		Symbol s = getSymbolAt(ad);
		dumpAt(out, s == null ? "addr" : s.getName(true), ad, p.length > 1 ? Integer.parseInt(p[1]) : 16);
	}

	private void sym(PrintWriter out, String spec) throws Exception {
		int dw = -1;
		int c = spec.lastIndexOf(':');
		if (c > 0 && spec.substring(c + 1).matches("\\d+")) {
			dw = Integer.parseInt(spec.substring(c + 1));
			spec = spec.substring(0, c);
		}
		Pattern p = Pattern.compile(spec);
		SymbolIterator it = currentProgram.getSymbolTable().getAllSymbols(true);
		while (it.hasNext()) {
			Symbol s = it.next();
			if (s.getSymbolType() != SymbolType.LABEL || !p.matcher(s.getName(true)).matches()) continue;
			Data d = getDataAt(s.getAddress());
			DataType t = d == null ? null : d.getDataType();
			int n = dw > 0 ? dw : Math.min(64, Math.max(1, (t == null ? 4 : t.getLength()) / 4));
			dumpAt(out, s.getName(true) + " type=" + (t == null ? "?" : t.getName()), s.getAddress(), n);
		}
	}

	private void dumpAt(PrintWriter out, String label, Address ad, int n) throws Exception {
		Memory mem = currentProgram.getMemory();
		out.printf("sym %s @%s%n", label, ad);
		Set<String> refs = new TreeSet<>();
		for (Reference r : getReferencesTo(ad)) {
			Function f = getFunctionContaining(r.getFromAddress());
			refs.add(f == null ? r.getFromAddress().toString() : f.getName(true));
		}
		out.println("  refs: " + String.join(", ", refs));
		for (int k = 0; k < n; k++) {
			Address x = ad.add(4L * k);
			if (!mem.contains(x)) break;
			long v = mem.getInt(x) & 0xffffffffL;
			String extra = "";
			try {
				Address pa = ad.getNewAddress(v);
				if (mem.contains(pa)) {
					StringBuilder sb = new StringBuilder();
					for (int j = 0; j < 80; j++) {
						byte b = mem.getByte(pa.add(j));
						if (b == 0) break;
						if (b < 0x20 || b > 0x7e) { sb.setLength(0); break; }
						sb.append((char) b);
					}
					if (sb.length() > 1) extra = "  \"" + sb + "\"";
					Symbol ps = getSymbolAt(pa);
					if (ps != null) extra += "  <" + ps.getName(true) + ">";
				}
			} catch (Exception e) { /* not an address */ }
			out.printf("  [%2d] 0x%08x %d%s%n", k, v, (int) v, extra);
		}
	}
}
