// Runs the host's own contract validator (schwung-current src/shared/param_pages/validate_contract.mjs) over
// the generated hierarchy and chain_params of both modules.  node tools/gen/validate.mjs
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const HOST = process.env.SCHWUNG_CURRENT || path.resolve(ROOT, "../schwung-current");
const { validateContract } = await import(path.join(HOST, "src/shared/param_pages/validate_contract.mjs"));
const header = fs.readFileSync(path.join(ROOT, "src/plugin/generated/mnm_ui.h"), "utf8");
let errors = 0;
for (const [v, id] of [["ONE", "monomodule-one"], ["FX", "monomodule-fx"]]) {
    const m = header.match(new RegExp(`k${v}Hierarchy\\[\\] = "(.*)";`));
    const hierarchy = JSON.parse(JSON.parse(`"${m[1]}"`));
    const mj = JSON.parse(fs.readFileSync(path.join(ROOT, "modules", id, "module.json"), "utf8"));
    const findings = validateContract({ id, hierarchy, chainParams: mj.capabilities.chain_params, capabilities: mj.capabilities });
    const list = Array.isArray(findings) ? findings : (findings.findings || []);
    console.log(`${id}: ${list.length} findings`);
    for (const f of list) {
        console.log(`  ${f.level || f.severity}  ${f.rule || f.code}  ${f.key || f.level_name || ""}  ${f.message || f.msg || ""}`);
        if ((f.level || f.severity) === "error") errors++;
    }
}
process.exit(errors ? 1 : 0);
