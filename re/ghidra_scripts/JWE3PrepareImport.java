// Runs before auto-analysis on a fresh JWE3 exe import.
// Drops the Denuvo VM blob so analysis only covers the real game code and its data, and turns
// off analyzers that cost hours on a binary this size without helping RE. On JWE2 the blob was a
// ~390 MB executable ".idata" and the code a 31 MB ".bss"; on JWE3 the names may differ, so any
// executable block of 128 MB or more is removed (re/tools/survey.py shows the section table).
//@category JWE3VR

import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class JWE3PrepareImport extends GhidraScript {
	@Override
	protected void run() throws Exception {
		Memory mem = currentProgram.getMemory();
		List<MemoryBlock> blobs = new ArrayList<>();
		for (MemoryBlock b : mem.getBlocks()) {
			println(String.format("block %-10s %s size 0x%x exec=%b", b.getName(), b.getStart(), b.getSize(), b.isExecute()));
			if (b.isExecute() && b.getSize() >= 0x8000000L) blobs.add(b);
		}
		for (MemoryBlock vm : blobs) {
			String name = vm.getName();
			mem.removeBlock(vm, monitor);
			println("removed Denuvo VM blob " + name);
		}
		String[] off = {
			"Function ID", "Library Identification", "Embedded Media", "Decompiler Parameter ID",
			"Aggressive Instruction Finder", "Condense Filler Bytes", "Variadic Function Signature Override",
			"DWARF", "PDB Universal", "PDB MSDIA"
		};
		for (String a : off) {
			try {
				setAnalysisOption(currentProgram, a, "false");
			}
			catch (Exception e) {
				println("no analyzer option: " + a);
			}
		}
	}
}
