import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import createFaultline from "./dist/faultline-engine.mjs";

const wasm = readFileSync(new URL("./dist/faultline-engine.wasm", import.meta.url));
const engine = await createFaultline({
  instantiateWasm(imports, success) {
    WebAssembly.instantiate(wasm, imports).then(({ instance }) => success(instance));
  }
});
const call = (name, returnType = "string", argTypes = [], args = []) => engine.ccall(name, returnType, argTypes, args);
const snapshot = () => JSON.parse(call("faultline_snapshot"));

JSON.parse(call("faultline_reset"));
assert.equal(snapshot().nodes.length, 5, "five-node cluster must initialize");
const election = JSON.parse(call("faultline_campaign", "string", ["number"], [1]));
assert.equal(election.nodes.find((node) => node.role === "leader")?.id, 1, "node 1 must win deterministic campaign");
const proposal = JSON.parse(call("faultline_propose", "string", ["number", "string", "string"], [7, "account/alice", "90"]));
assert.match(proposal.event, /committed transaction 7/, "quorum proposal must commit");

const valid = JSON.parse(call("faultline_check_history", "string", ["number"], [0]));
assert.equal(valid.linearizable, true, "valid register history must have a witness");
assert.ok(valid.witness.length > 0, "valid result must expose witness order");
const stale = JSON.parse(call("faultline_check_history", "string", ["number"], [1]));
assert.equal(stale.linearizable, false, "stale read must be rejected");

console.log(JSON.stringify({ nodes: 5, leader: 1, committedTransaction: 7, witness: valid.witness, staleRejected: !stale.linearizable }));
