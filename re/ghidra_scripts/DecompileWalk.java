// Decompile functions (like DecompileFunctions) after giving DataWalk a typed vtable, so that the visitor's virtual
// calls show their arguments: dw->vt->walk_function(dw, start, end) and dw->vt->walk_test(dw, label).
// Without this the PDB leaves DataWalk's vfptr as an int ("_padding_") and the decompiler drops every argument.
// The change is made in memory only (the project is opened read-only).
// Args: <outDir> <regex> [regex...]   (regexes over fully qualified names)
// @category RON
import java.io.File;
import java.io.PrintWriter;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class DecompileWalk extends GhidraScript {
	private DataType find(DataTypeManager dtm, String name) {
		java.util.List<DataType> l = new java.util.ArrayList<>();
		dtm.findDataTypes(name, l);
		for (DataType d : l) if (d.getPathName().equals("/rise.pdb/" + name)) return d;
		return l.isEmpty() ? null : l.get(0);
	}

	private void fixDataWalk() throws Exception {
		DataTypeManager dtm = currentProgram.getDataTypeManager();
		Structure dw = (Structure) find(dtm, "DataWalk");
		DataType str = find(dtm, "String");
		CategoryPath cp = new CategoryPath("/ron_walk");
		Pointer pdw = new PointerDataType(dw, dtm);
		FunctionDefinitionDataType wf = new FunctionDefinitionDataType(cp, "DW_walk_function", dtm);
		wf.setReturnType(VoidDataType.dataType);
		wf.setArguments(new ParameterDefinition[] {
			new ParameterDefinitionImpl("dw", pdw, null),
			new ParameterDefinitionImpl("start", new PointerDataType(VoidDataType.dataType, dtm), null),
			new ParameterDefinitionImpl("end", new PointerDataType(VoidDataType.dataType, dtm), null) });
		wf.setCallingConvention("__thiscall");
		FunctionDefinitionDataType wt = new FunctionDefinitionDataType(cp, "DW_walk_test", dtm);
		wt.setReturnType(VoidDataType.dataType);
		wt.setArguments(new ParameterDefinition[] {
			new ParameterDefinitionImpl("dw", pdw, null),
			new ParameterDefinitionImpl("label", new PointerDataType(str, dtm), null) });
		wt.setCallingConvention("__thiscall");
		StructureDataType vt = new StructureDataType(cp, "DataWalkVtbl", 0, dtm);
		vt.add(new PointerDataType(wf, dtm), 4, "walk_function", null);
		vt.add(new PointerDataType(wt, dtm), 4, "walk_test", null);
		DataType vtr = dtm.addDataType(vt, DataTypeConflictHandler.REPLACE_HANDLER);
		dw.replaceAtOffset(0, new PointerDataType(vtr, dtm), 4, "vt", null);
		println("DataWalk vtable typed: " + dw.toString().replace('\n', ' '));
	}

	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		File outDir = new File(a[0]);
		outDir.mkdirs();
		Pattern[] pats = new Pattern[a.length - 1];
		for (int i = 1; i < a.length; i++) pats[i - 1] = Pattern.compile(a[i]);
		fixDataWalk();

		DecompInterface decomp = new DecompInterface();
		decomp.setOptions(new DecompileOptions());
		decomp.openProgram(currentProgram);
		int count = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			monitor.checkCancelled();
			String name = f.getName(true);
			boolean match = false;
			for (Pattern p : pats) if (p.matcher(name).matches()) { match = true; break; }
			if (!match || f.isThunk()) continue;
			DecompileResults res = decomp.decompileFunction(f, 120, monitor);
			String fileName = name.replaceAll("[^A-Za-z0-9_]+", "_") + "@" + f.getEntryPoint() + ".c";
			try (PrintWriter w = new PrintWriter(new File(outDir, fileName), "UTF-8")) {
				w.println("// " + name + " @ " + f.getEntryPoint());
				if (res.decompileCompleted()) {
					w.println(res.getDecompiledFunction().getC());
				} else {
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
