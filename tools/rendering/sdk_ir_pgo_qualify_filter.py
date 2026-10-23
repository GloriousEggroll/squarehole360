#!/usr/bin/env python3
"""Explicit CPU compiler-only gate: emitted IR counters, never a game/GPU run."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import sdk_ir_pgo as pgo

EXPECTED = {
    "_ZN3rex8graphics16CommandProcessor13ExecutePacketEj",
    "_ZN3rex8graphics6vulkan22VulkanCommandProcessor9IssueDrawEj",
    "_ZN3rex6memory8BaseHeap16QueryRangeAccessEjj",
    "_ZN3rex6memory10RingBuffer4ReadEj",
    "_ZN3rex7runtime18FunctionDispatcher7ExecuteEj",
    "_ZN3rex7runtime23ResolveIndirectFunctionEj",
}
FORBIDDEN_PARTS = ("UpdateWritePointer", "AccessViolationCallback", "PhysicalSnapshotTracker",
                   "MMIOHandler", "signal_handler", "CommonTemplate", "UnrelatedGuestBody",
                   "ExceptionCleanupWitness", "CleanupWitnessD2Ev")
LOCAL = "_ZZN3rex8graphics16CommandProcessor13ExecutePacketEjENK3$_0clEj"

def counters(ir):
    return {a or b for a,b in re.findall(r'@(?:"__profc_([^"\n]+)"|__profc_([^\s=]+))\s*=', ir)}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root",required=True)
    parser.add_argument("--compiler",required=True)
    parser.add_argument("--profdata",required=True)
    parser.add_argument("--output",required=True)
    a=parser.parse_args()
    root=Path(a.root).resolve(); out=Path(a.output).resolve()
    pgo.need(not out.exists(),"filter proof output must be fresh")
    out.mkdir(parents=True)
    (out/"raw").mkdir()
    identity=pgo.compiler_identity(a.compiler)
    policy=root/pgo.POLICY
    fixture=root/"tools/rendering/sdk_ir_pgo_policy_test.cpp"
    driver=root/"tools/rendering/sdk_ir_pgo_policy_driver.cpp"
    expected_names=EXPECTED | {str(fixture)+";"+LOCAL, pgo.TERMINATE_HELPER}
    expected_ir=EXPECTED | {re.sub(r"[/;-]", "_", str(fixture)+";"+LOCAL),
                            pgo.TERMINATE_HELPER+".742261418966908927"}
    profdata=Path(a.profdata).resolve(strict=True)
    tool_version=pgo.run([profdata,"--version"])
    pgo.need("LLVM version "+identity["release"] in tool_version,"compiler/profile-tool release mismatch")
    base=[identity["path"],"--driver-mode=g++","-std=c++23","-O3","-fPIC","-mcmodel=large","-msse4.1",
          "-ffp-model=strict","-fno-strict-aliasing","-fno-char8_t"]
    generate=["-fprofile-update=atomic","-fprofile-generate="+str(out/"raw"),"-fprofile-list="+str(policy)]
    command=base+generate+["-S","-emit-llvm",str(fixture),"-o",str(out/"fixture.ll")]
    completed=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (out/"compile.log").write_text(completed.stdout)
    result={"schema":"sdk-ir-pgo-filter-proof-v1","passed":False,"compiler":identity,
            "policy_sha256":pgo.digest(policy),"fixture_sha256":pgo.digest(fixture),"command":command,
            "returncode":completed.returncode}
    if completed.returncode==0:
        ir=(out/"fixture.ll").read_text(); observed=counters(ir)
        definitions=set(re.findall(r'^define\b[^\n]*?@(?:"([^"]+)"|([^\s(]+))\(',ir,re.MULTILINE))
        definitions={x or y for x,y in definitions}
        missing_forbidden=[name for name in FORBIDDEN_PARTS if not any(name in n for n in definitions)]
        result.update(observed_counters=sorted(observed),expected_counters=sorted(expected_ir),
                      defined_functions=sorted(definitions),missing_forbidden_witnesses=missing_forbidden)
        # Positive and negative witnesses both need to exist. Absence of all
        # counters is a failure, as is any escaped callback/template counter.
        result["passed"]=observed==expected_ir and not missing_forbidden
    if result["passed"]:
        # Actual DSO-only runtime/flush proof. The driver has no instrumentation;
        # only the shared-library compile/link gets GENERATE. USE reuses exactly
        # the same driver bytes and strict mismatch/missing-function diagnostics.
        result["passed"]=False
        result["commands"]=[]
        outputs={}
        try:
            # Qualify exactly the unavoidable compiler-generated termination
            # helper. Never call the fatal path; accepted merged counts must0.
            cmd=base+generate+["-c",str(fixture),"-o",str(out/"fixture.o")]
            result["commands"].append(cmd)
            (out/"object-build.log").write_text(pgo.run(cmd))
            pgo.need(set(pgo.profile_symbols(out/"fixture.o","nm"))==expected_names,
                     "local/exception fixture object counter scope mismatch")
            certificate=pgo.terminate_helper_certificate(out/"fixture.o")
            body=re.search(r"^define[^\n]*@__clang_call_terminate\([^\n]*\{\n(.*?)^\}",ir,re.MULTILINE|re.DOTALL)
            pgo.need(body and re.findall(r"(?:tail )?call [^\n]*?@([^ (]+)",body.group(1))==
                     ["__cxa_begin_catch","_ZSt9terminatev"] and
                     body.group(1).count("atomicrmw add")==1 and
                     body.group(1).rstrip().endswith("unreachable"),"unexpected termination helper IR")
            for mode in ("off","generate","use"):
                folder=out/mode;folder.mkdir()
                options=[] if mode=="off" else generate if mode=="generate" else [
                    "-fprofile-use="+str(out/"fixture.profdata"),"-fprofile-list="+str(policy),
                    "-Werror=profile-instr-out-of-date","-Werror=profile-instr-unprofiled"]
                cmd=base+options+["-shared",str(fixture),"-o",str(folder/"libpolicy.so")]
                result["commands"].append(cmd)
                completed=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
                (folder/"build.log").write_text(completed.stdout)
                pgo.need(completed.returncode==0,"DSO "+mode+" build failed")
                if mode=="off":
                    cmd=base+[str(driver),"-L"+str(folder),"-lpolicy","-Wl,-rpath,$ORIGIN","-o",str(folder/"driver")]
                    result["commands"].append(cmd)
                    completed=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
                    (folder/"driver-build.log").write_text(completed.stdout)
                    pgo.need(completed.returncode==0,"uninstrumented driver build failed")
                else:
                    shutil.copyfile(out/"off/driver",folder/"driver")
                    (folder/"driver").chmod(0o755)
                pgo.need(not pgo.profile_symbols(folder/"driver","nm"),"driver accidentally instrumented")
                observed=pgo.profile_symbols(folder/"libpolicy.so","nm")
                pgo.need(set(observed)==expected_names if mode=="generate" else not observed,"DSO emitted-counter scope mismatch")
                env=dict(os.environ);env["LLVM_PROFILE_FILE"]=str(out/"raw/fixture_%m_%p.profraw")
                completed=subprocess.run([str(folder/"driver")],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
                (folder/"run.log").write_text(completed.stdout)
                pgo.need(completed.returncode==0,"DSO "+mode+" driver failed")
                outputs[mode]=completed.stdout
                if mode=="generate":
                    files=sorted((out/"raw").glob("*.profraw"));pgo.need(files,"DSO-only profile did not flush")
                    cmd=[str(profdata),"merge","--failure-mode=any","-o",str(out/"fixture.profdata")]+[str(p) for p in files]
                    result["commands"].append(cmd)
                    (out/"merge.log").write_text(pgo.run(cmd))
                    counts=pgo.run([profdata,"show","--all-functions","--counts",out/"fixture.profdata"])
                    (out/"counts.txt").write_text(counts)
                    parsed=pgo.profile_counts(counts)
                    pgo.need(set(parsed)==expected_names and parsed[pgo.TERMINATE_HELPER]==0 and
                             parsed[str(fixture)+";"+LOCAL]>0,"local/exception merged profile evidence mismatch")
            pgo.need(len(set(outputs.values()))==1,"OFF/GENERATE/USE fixture output changed")
            result.update(passed=True,dso_only_flush=True,use_strict_warnings_passed=True,
                          local_profile_names_qualified=True,compiler_terminate_helper_qualified=certificate,
                          outputs=outputs,driver_sha256=pgo.digest(out/"off/driver"),
                          profdata={"path":str(profdata),"sha256":pgo.digest(profdata),"version":tool_version})
        except (RuntimeError,OSError,subprocess.SubprocessError) as e:
            result["error"]=str(e)
    pgo.save(out/"result.json",result)
    pgo.need(result["passed"],"Clang filter/DSO flush/USE qualification failed")
    print("PASS compiler filter: six allowed methods, authenticated local lambda, pinned zero-execution termination helper, nine exclusions, DSO-only flush and strict USE parity; no game/GPU run")

if __name__=="__main__":
    try: main()
    except (RuntimeError,OSError,ValueError,subprocess.CalledProcessError) as e:
        print("FAIL filter proof: "+str(e),file=sys.stderr);sys.exit(1)
