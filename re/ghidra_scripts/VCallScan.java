// Find virtual calls made through a global object pointer: in every function that reads <global>, track
// (linearly, per function) registers loaded from the global, then registers loaded from [objReg] (the vtable),
// then CALL [vtReg + disp]. Prints function, call address and the vtable displacement for the requested ones.
// Args: <outFile> <globalSymbol or 0xADDRESS> <hexDisp|*> [hexDisp...]
// @category RON
import java.io.PrintWriter;
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class VCallScan extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		Set<Long> want = new HashSet<>();
		boolean all = false;
		for (int i = 2; i < a.length; i++) {
			if (a[i].equals("*")) all = true; else want.add(Long.parseLong(a[i].replace("0x", ""), 16));
		}
		Address g = null;
		if (a[1].startsWith("0x")) g = toAddr(a[1]);
		else {
			SymbolIterator it = currentProgram.getSymbolTable().getSymbols(a[1]);
			while (it.hasNext()) { g = it.next().getAddress(); break; }
		}
		if (g == null) { println("no symbol " + a[1]); return; }
		Set<Function> funcs = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
		for (Reference r : getReferencesTo(g)) {
			Function f = getFunctionContaining(r.getFromAddress());
			if (f != null) funcs.add(f);
		}
		try (PrintWriter w = new PrintWriter(a[0], "UTF-8")) {
			for (Function f : funcs) {
				Map<String, Integer> state = new HashMap<>(); // reg -> 1 obj, 2 vtable
				InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
				while (ii.hasNext()) {
					Instruction ins = ii.next();
					String m = ins.getMnemonicString();
					if (m.equals("MOV") && ins.getNumOperands() == 2) {
						Object[] o0 = ins.getOpObjects(0);
						Object[] o1 = ins.getOpObjects(1);
						if (o0.length == 1 && o0[0] instanceof Register) {
							String dst = ((Register) o0[0]).getName();
							boolean fromG = false;
							for (Reference r : ins.getReferencesFrom()) if (r.getToAddress().equals(g)) fromG = true;
							if (fromG) { state.put(dst, 1); continue; }
							if (o1.length == 1 && o1[0] instanceof Register && ins.toString().contains("[")) {
								String src = ((Register) o1[0]).getName();
								if (Integer.valueOf(1).equals(state.get(src))) { state.put(dst, 2); continue; }
							}
							if (o1.length == 1 && o1[0] instanceof Register && !ins.toString().contains("[")) {
								Integer s = state.get(((Register) o1[0]).getName());
								if (s != null) { state.put(dst, s); continue; }
							}
							state.remove(dst);
						}
					} else if (m.equals("CALL")) {
						Object[] o = ins.getOpObjects(0);
						if (o.length == 2 && o[0] instanceof Register && o[1] instanceof Scalar) {
							String reg = ((Register) o[0]).getName();
							long d = ((Scalar) o[1]).getUnsignedValue();
							if (Integer.valueOf(2).equals(state.get(reg)) && (all || want.contains(d)))
								w.printf("%s  %s  +0x%x%n", f.getName(true), ins.getAddress(), d);
						}
						state.remove("EAX"); state.remove("ECX"); state.remove("EDX");
					}
				}
			}
		}
	}
}
