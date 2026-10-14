// Grep every instruction's text (as Ghidra prints it) with a regex; prints address, function and instruction.
// Args: <outFile> <regex> [regex...]
// @category RON
import java.io.PrintWriter;
import java.util.regex.Pattern;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;

public class InsnGrep extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] a = ArgFiles.expand(getScriptArgs());
		Pattern[] p = new Pattern[a.length - 1];
		for (int i = 1; i < a.length; i++) p[i - 1] = Pattern.compile(a[i]);
		try (PrintWriter w = new PrintWriter(a[0], "UTF-8")) {
			InstructionIterator it = currentProgram.getListing().getInstructions(true);
			while (it.hasNext()) {
				Instruction ins = it.next();
				String s = ins.toString();
				for (Pattern q : p) if (q.matcher(s).find()) {
					Function f = getFunctionContaining(ins.getAddress());
					w.println(ins.getAddress() + "  " + (f == null ? "?" : f.getName(true)) + "  " + s);
					break;
				}
			}
		}
	}
}
