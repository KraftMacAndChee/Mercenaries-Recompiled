/* Pre-analysis script for an Xbox XBE raw import.
 *
 * Runs BEFORE auto-analysis (headless -preScript) and forces ON the analyzers
 * most useful for recovering meaningful names from a stripped, statically
 * linked MSVC/XDK binary:
 *   - Function ID  (matches against bundled vsOlder/vs20xx x86 FidDb files ->
 *                   names statically linked CRT/library functions)
 *   - Library Identification
 *   - Demangler Microsoft (decodes MSVC-mangled C++ symbols if any survive)
 *   - Windows x86 PE RTTI Analyzer (recovers class names + vftables)
 *   - Decompiler Parameter ID (better signatures via decompiler)
 *   - Decompiler Switch Analysis
 *   - Scalar Operand References (helps resolve cross-section pointers)
 *   - Aggressive Instruction Finder (squeeze more code out of the flat image)
 *
 * Any analyzer name not present in this Ghidra build is silently skipped.
 *
 * @category XboxRecomp
 */
import java.util.Map;

import ghidra.app.script.GhidraScript;

public class SetAnalysisOptions extends GhidraScript {

	private void enable(Map<String, String> opts, String name) {
		if (opts.containsKey(name)) {
			setAnalysisOption(currentProgram, name, "true");
			println("[pre] enabled analyzer: " + name);
		} else {
			println("[pre] analyzer not present (skipped): " + name);
		}
	}

	private void setOpt(Map<String, String> opts, String name, String value) {
		if (opts.containsKey(name)) {
			setAnalysisOption(currentProgram, name, value);
			println("[pre] set " + name + " = " + value);
		}
	}

	@Override
	protected void run() throws Exception {
		Map<String, String> opts = getCurrentAnalysisOptionsAndValues(currentProgram);

		// Naming-critical analyzers.
		enable(opts, "Function ID");
		enable(opts, "Library Identification");
		enable(opts, "Demangler Microsoft");

		// RTTI / vftable recovery (analyzer name varies across versions; try all).
		enable(opts, "Windows x86 PE RTTI Analyzer");
		enable(opts, "RTTI Analyzer");
		enable(opts, "Windows x86 PE Exception Handling");

		// Decompiler-dependent analyzers for better signatures.
		enable(opts, "Decompiler Parameter ID");
		enable(opts, "Decompiler Switch Analysis");
		setOpt(opts, "Decompiler Parameter ID.Analysis Decompiler Timeout (sec)", "60");

		// Reference / code discovery helpers for a flat raw image.
		enable(opts, "Scalar Operand References");
		enable(opts, "Aggressive Instruction Finder");
		enable(opts, "Reference");
		enable(opts, "Shared Return Calls");

		println("[pre] analysis option setup complete.");
	}
}
