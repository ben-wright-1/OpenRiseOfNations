// Dump structures whose full PATH name matches a regex, with every component (base-class components included).
// Args: <outFile> <regex> [regex...]
// @category RON
import java.io.PrintWriter;
import java.util.*;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.data.Enum;

public class DumpTypesPath extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		List<Pattern> pats = new ArrayList<>();
		for (int i = 1; i < a.length; i++) pats.add(Pattern.compile(a[i]));
		try (PrintWriter out = new PrintWriter(a[0], "UTF-8")) {
			Iterator<DataType> it = currentProgram.getDataTypeManager().getAllDataTypes();
			while (it.hasNext()) {
				DataType dt = it.next();
				boolean m = false;
				for (Pattern p : pats) if (p.matcher(dt.getPathName()).matches()) { m = true; break; }
				if (!m) continue;
				if (dt instanceof Structure s) {
					out.println("struct " + dt.getPathName() + " size=0x" + Integer.toHexString(s.getLength()));
					for (DataTypeComponent c : s.getDefinedComponents())
						out.printf("  +0x%03x %-50s %s%n", c.getOffset(), c.getDataType().getPathName(), c.getFieldName());
				} else if (dt instanceof Enum e) {
					out.println("enum " + dt.getPathName());
					for (String n : e.getNames()) out.println("  " + n + " = " + e.getValue(n));
				} else continue;
				out.println();
			}
		}
	}
}
