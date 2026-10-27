// Decompiles the functions listed in re/targets.txt into re/decomp/.
// Line formats:
//   fn  <name> <address> [depth]   decompile the function at address (+ callees to depth)
//   str <exact string> [depth]     decompile every function that references the string
// Lines starting with # are ignored. Also writes re/decomp/index.tsv (name, address, file).
//@category JWE3VR

import java.io.*;
import java.nio.file.*;
import java.util.*;

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;
import ghidra.program.util.DefinedStringIterator;

public class JWE3DumpTargets extends GhidraScript {
	private DecompInterface decomp;
	private Path outDir;
	private final Set<Address> done = new HashSet<>();
	private PrintWriter index;

	@Override
	protected void run() throws Exception {
		Path root = Paths.get(getSourceFile().getParentFile().getParentFile().getAbsolutePath());
		outDir = root.resolve("decomp");
		Files.createDirectories(outDir);
		index = new PrintWriter(new FileWriter(outDir.resolve("index.tsv").toFile(), true));
		decomp = new DecompInterface();
		decomp.toggleCCode(true);
		decomp.openProgram(currentProgram);

		List<String> lines = Files.readAllLines(root.resolve("targets.txt"));
		Map<String, List<Address>> strings = null;
		for (String raw : lines) {
			String line = raw.trim();
			if (line.isEmpty() || line.startsWith("#")) {
				continue;
			}
			if (line.startsWith("fn ")) {
				String[] p = line.split("\\s+");
				int depth = p.length > 3 ? Integer.parseInt(p[3]) : 0;
				Function f = fnAt(toAddr(p[2]));
				if (f == null) {
					println("no function at " + p[2] + " (" + p[1] + ")");
					continue;
				}
				dump(p[1], f, depth);
			}
			else if (line.startsWith("str ")) {
				String rest = line.substring(4);
				int depth = 0;
				int sp = rest.lastIndexOf(' ');
				if (sp > 0 && rest.substring(sp + 1).matches("\\d")) {
					depth = Integer.parseInt(rest.substring(sp + 1));
					rest = rest.substring(0, sp);
				}
				if (strings == null) {
					strings = collectStrings();
				}
				for (Address sa : strings.getOrDefault(rest, List.of())) {
					for (Reference r : getReferencesTo(sa)) {
						Function f = getFunctionContaining(r.getFromAddress());
						if (f != null) {
							dump("ref_" + sanitize(rest), f, depth);
						}
						else {
							println("str " + rest + " ref from non-function " + r.getFromAddress());
						}
					}
				}
			}
		}
		index.close();
		decomp.dispose();
	}

	private Function fnAt(Address a) throws Exception {
		Function f = getFunctionAt(a);
		if (f == null) {
			f = getFunctionContaining(a);
		}
		if (f == null) {
			disassemble(a);
			f = createFunction(a, null);
		}
		return f;
	}

	private Map<String, List<Address>> collectStrings() {
		Map<String, List<Address>> m = new HashMap<>();
		for (Data d : DefinedStringIterator.forProgram(currentProgram)) {
			StringDataInstance s = StringDataInstance.getStringDataInstance(d);
			String v = s.getStringValue();
			if (v != null) {
				m.computeIfAbsent(v, k -> new ArrayList<>()).add(d.getAddress());
			}
		}
		return m;
	}

	private void dump(String name, Function f, int depth) throws Exception {
		if (!done.add(f.getEntryPoint()) || monitor.isCancelled()) {
			return;
		}
		DecompileResults res = decomp.decompileFunction(f, 120, monitor);
		String file = sanitize(name) + "_" + f.getEntryPoint().toString() + ".c";
		try (PrintWriter w = new PrintWriter(new FileWriter(outDir.resolve(file).toFile()))) {
			w.println("// " + name + " @ " + f.getEntryPoint() + "  (" + f.getName() + ")");
			w.println("// callers: " + callers(f));
			if (res != null && res.decompileCompleted()) {
				w.println(res.getDecompiledFunction().getC());
			}
			else {
				w.println("// decompile failed: " + (res == null ? "null" : res.getErrorMessage()));
			}
		}
		index.println(name + "\t" + f.getEntryPoint() + "\t" + file);
		if (depth > 0) {
			for (Function c : f.getCalledFunctions(monitor)) {
				MemoryBlock b = getMemoryBlock(c.getEntryPoint());
				if (b != null && b.getName().equals(".bss") && c.getBody().getNumAddresses() > 8) {
					dump(name + "__" + c.getEntryPoint(), c, depth - 1);
				}
			}
		}
	}

	private String callers(Function f) {
		StringBuilder sb = new StringBuilder();
		int n = 0;
		for (Function c : f.getCallingFunctions(monitor)) {
			if (n++ == 12) {
				sb.append("...");
				break;
			}
			sb.append(c.getEntryPoint()).append(' ');
		}
		return sb.toString();
	}

	private static String sanitize(String s) {
		String t = s.replaceAll("[^A-Za-z0-9_]+", "_");
		return t.length() > 60 ? t.substring(0, 60) : t;
	}
}
