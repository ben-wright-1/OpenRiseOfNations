// Pre-analysis script: point the PDB analyzer at an explicit .pdb file.
// Usage (analyzeHeadless): -preScript SetPdb.java <path-to.pdb>
// @category RON
import java.io.File;

import ghidra.app.plugin.core.analysis.PdbUniversalAnalyzer;
import ghidra.app.script.GhidraScript;

public class SetPdb extends GhidraScript {
	@Override
	public void run() throws Exception {
		File pdb = new File(getScriptArgs()[0]);
		if (!pdb.isFile()) {
			throw new IllegalArgumentException("PDB not found: " + pdb);
		}
		PdbUniversalAnalyzer.setPdbFileOption(currentProgram, pdb);
		println("PDB set to " + pdb);
	}
}
