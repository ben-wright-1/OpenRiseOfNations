// Find byte/dword stores to [reg + disp] where reg was just loaded from a global pointer (linear, per function).
// Typical use: who writes field <disp> of the object a global points to.
// Args: <outFile> <0xGlobalAddress> <hexDisp> [hexDisp...]
// @category RON
import java.io.PrintWriter;
import java.util.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class FieldWriteScan extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		Address g = toAddr(a[1]);
		Set<Long> want = new HashSet<>();
		for (int i = 2; i < a.length; i++) want.add(Long.parseLong(a[i].replace("0x", ""), 16));
		Set<Function> funcs = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
		for (Reference r : getReferencesTo(g)) {
			Function f = getFunctionContaining(r.getFromAddress());
			if (f != null) funcs.add(f);
		}
		try (PrintWriter w = new PrintWriter(a[0], "UTF-8")) {
			for (Function f : funcs) {
				Set<String> obj = new HashSet<>();
				InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
				while (ii.hasNext()) {
					Instruction ins = ii.next();
					String m = ins.getMnemonicString();
					if (ins.getNumOperands() < 1) continue;
					Object[] o0 = ins.getOpObjects(0);
					boolean fromG = false;
					for (Reference r : ins.getReferencesFrom()) if (r.getToAddress().equals(g)) fromG = true;
					if (m.equals("MOV") && o0.length == 1 && o0[0] instanceof Register) {
						String dst = ((Register) o0[0]).getName();
						if (fromG) obj.add(dst); else obj.remove(dst);
						continue;
					}
					if (m.equals("MOV") && o0.length == 2 && o0[0] instanceof Register && o0[1] instanceof Scalar) {
						String reg = ((Register) o0[0]).getName();
						long d = ((Scalar) o0[1]).getUnsignedValue();
						if (obj.contains(reg) && want.contains(d)) w.printf("%s  %s  %s%n", f.getName(true), ins.getAddress(), ins);
					}
					if (m.equals("CALL")) { obj.remove("EAX"); obj.remove("ECX"); obj.remove("EDX"); }
				}
			}
		}
	}
}
