// Headless Ghidra post-script (docs/phone-mirroring.md 7.8):
//   analyzeHeadless <proj_dir> <proj> -import <elf> -scriptPath tools \
//     -postScript DumpDecomp.java <out.c> [0xADDR ...]
// Decompile every function of the current program into <arg0>; the
// optional addresses become functions first (code reached only through
// data tables, e.g. mp4_parser's box handlers).
// SH4 PIC: literal-pool constants (DAT_xxx) are replaced by their value,
// "(code *)(DAT_x + 0xNNNN)" calls (bsrf/braf: target = const + pc) by the
// target function's name, and GOT-relative data ("(int)&DAT_a + DAT_o +
// r12-copy") by the C string there, or PTR_<addr>.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import java.io.*;
import java.util.regex.*;

public class DumpDecomp extends GhidraScript {
	private Long poolValue(Address a, int size) {
		try {
			Memory m = currentProgram.getMemory();
			if (size == 2)
				return (long) m.getShort(a);
			return (long) m.getInt(a);
		} catch (Exception e) {
			return null;
		}
	}

	private int poolSize(Address a) {
		Data d = getDataAt(a);
		if (d != null && d.getLength() == 2)
			return 2;
		if (d != null && d.getLength() == 4)
			return 4;
		return (a.getOffset() & 3) != 0 ? 2 : 4;
	}

	private String fix(String c) {
		AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
		Pattern call = Pattern.compile("\\(\\*\\(code \\*\\)\\(DAT_([0-9a-f]{8}) \\+ 0x([0-9a-f]+)\\)\\)");
		Matcher m = call.matcher(c);
		StringBuffer sb = new StringBuffer();
		while (m.find()) {
			Address a = sp.getAddress(Long.parseLong(m.group(1), 16));
			Long v = poolValue(a, 4);
			String rep = m.group(0);
			if (v != null) {
				long t = (v + Long.parseLong(m.group(2), 16)) & 0xffffffffL;
				Function f = getFunctionAt(sp.getAddress(t));
				rep = f != null ? f.getName() : String.format("FUN_%08x", t);
			}
			m.appendReplacement(sb, Matcher.quoteReplacement(rep));
		}
		m.appendTail(sb);
		c = sb.toString();

		// GOT-relative data: (int)&DAT_a + DAT_o + iVarN, where iVarN was
		// loaded from DAT_a (r12 = &DAT_a + *DAT_a = GOT).
		Pattern gotref = Pattern.compile("\\(int\\)&DAT_([0-9a-f]{8}) \\+ DAT_([0-9a-f]{8}) \\+ [a-zA-Z_][a-zA-Z0-9_]*");
		m = gotref.matcher(c);
		sb = new StringBuffer();
		while (m.find()) {
			Address a = sp.getAddress(Long.parseLong(m.group(1), 16));
			Address o = sp.getAddress(Long.parseLong(m.group(2), 16));
			Long base = poolValue(a, 4);
			int osz = poolSize(o);
			Long off = poolValue(o, osz);
			String rep = m.group(0);
			if (base != null && off != null) {
				long t = (a.getOffset() + base + off) & 0xffffffffL;
				rep = String.format("PTR_%08x", t);
				try {
					byte[] buf = new byte[96];
					int n = currentProgram.getMemory().getBytes(sp.getAddress(t), buf);
					int len = 0;
					while (len < n && buf[len] >= 0x20 && buf[len] < 0x7f)
						len++;
					if (len >= 2 && len < n && buf[len] == 0)
						rep = "\"" + new String(buf, 0, len).replace("\\", "\\\\") + "\"";
				} catch (Exception e) {
				}
			}
			m.appendReplacement(sb, Matcher.quoteReplacement(rep));
		}
		m.appendTail(sb);
		c = sb.toString();

		Pattern dat = Pattern.compile("DAT_([0-9a-f]{8})");
		m = dat.matcher(c);
		sb = new StringBuffer();
		while (m.find()) {
			Address a = sp.getAddress(Long.parseLong(m.group(1), 16));
			MemoryBlock b = currentProgram.getMemory().getBlock(a);
			String rep = m.group(0);
			if (b != null && b.isExecute()) {
				int sz = poolSize(a);
				Long v = poolValue(a, sz);
				if (v != null)
					rep = String.format("0x%x", sz == 2 ? v & 0xffff : v & 0xffffffffL);
				if (sz == 2 && v != null && v < 0)
					rep = String.format("-0x%x", -v);
			}
			m.appendReplacement(sb, Matcher.quoteReplacement(rep));
		}
		m.appendTail(sb);
		return sb.toString();
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		String out = args[0];
		// Optional: hex addresses of functions reached only through data.
		for (int i = 1; i < args.length; i++) {
			Address a = toAddr(Long.parseLong(args[i].replace("0x", ""), 16));
			if (getFunctionAt(a) == null) {
				disassemble(a);
				createFunction(a, null);
			}
		}
		DecompInterface d = new DecompInterface();
		d.openProgram(currentProgram);
		try (PrintWriter w = new PrintWriter(new FileWriter(out))) {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				if (f.isThunk() || f.isExternal())
					continue;
				DecompileResults r = d.decompileFunction(f, 120, monitor);
				w.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
				if (r != null && r.decompileCompleted())
					w.println(fix(r.getDecompiledFunction().getC()));
				else
					w.println("// decompile failed: " + (r == null ? "?" : r.getErrorMessage()));
			}
		}
	}
}
