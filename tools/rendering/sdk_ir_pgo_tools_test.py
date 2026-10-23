#!/usr/bin/env python3
"""CPU-only admission/refusal tests using independent miniature Ninja graphs."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import sdk_ir_pgo as pgo

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",required=True)
    a=parser.parse_args(); cases=[]
    with tempfile.TemporaryDirectory(prefix="sdk-pgo-controls-") as temporary:
        root=Path(temporary)/"root with spaces"; build=root/"build"; build.mkdir(parents=True)
        mandatory=(pgo.POLICY,"CMakeLists.txt","cmake/native_pgo.cmake","cmake/sdk_ir_pgo.cmake",
                   "tools/rendering/sdk_ir_pgo.py","tools/rendering/verify-native-boot.py",
                   "tools/rendering/sdk_ir_pgo_desktop.py","tools/rendering/sdk_ir_pgo_elf.py",
                   "tools/rendering/sdk_ir_pgo_qualify_filter.py","tools/rendering/sdk_ir_pgo_policy_test.cpp",
                   "tools/rendering/sdk_ir_pgo_policy_driver.cpp",
                   "generated/rexglue.cmake","vendor/rexglue-sdk/CMakeLists.txt")
        for name in mandatory+tuple(pgo.SOURCES):
            path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text("// fixture\n")
        identity={"release":"22.1.8","sha256":"a"*64}
        config={"mode":"GENERATE","root":str(root),"build":str(build),"compiler":identity,
                "source":pgo.source_identity(root),"profile_path":str(root/"raw")}
        pgo.save(build/"sdk-ir-pgo/config.json",config)
        def graph(mode,change=None):
            lines=[]
            for index,(source,target) in enumerate(pgo.SOURCES.items()):
                obj=f"sdk/CMakeFiles/{target}.dir/source{index}.cpp.o"
                flags="-O3 -fPIC -mcmodel=large -msse4.1 -ffp-model=strict -fno-strict-aliasing"
                if mode=="GENERATE":
                    flags+=' -fprofile-list="'+str(root/pgo.POLICY)+'" -fprofile-generate="'+str(root/"raw")+'" -fprofile-update=atomic'
                lines += [f"build {obj}: CXX_COMPILER__{target}_unscanned_Release "+str(root/source).replace(" ","$ ")+" || object-order",
                          "  FLAGS = "+flags,"  DEFINES = -DKEEP=1","  INCLUDES = -I\""+str(root/"include space")+"\"",""]
            lines += ["build bin/gow3: CXX_EXECUTABLE_LINKER__gow3_Release object.o",
                      "  FLAGS = -O3","  LINK_FLAGS = -Wl,--no-relax","",
                      "build bin/librexruntime.so: CXX_SHARED_LIBRARY_LINKER__rexruntime_Release object.o",
                      "  LINK_FLAGS = -shared -pthread"+((' -fprofile-generate="'+str(root/"raw")+'"') if mode=="GENERATE" else ""),""]
            text="\n"+"\n".join(lines)
            (build/"build.ninja").write_text(change(text) if change else text)
        def check(name,mode="GENERATE",change=None,accept=False,baseline=True):
            graph(mode,change)
            args=SimpleNamespace(root=str(root),build=str(build),mode=mode,output=str(root/(name+".json")),
                                 baseline=str(root/"off.json") if baseline else None,observe=False,manifest=None,nm="nm")
            error=None
            try:
                with contextlib.redirect_stdout(io.StringIO()):pgo.coverage(args)
            except RuntimeError as e:error=str(e)
            pgo.need((error is None)==accept,"unexpected qualification result: "+name)
            cases.append({"name":name,"expected_acceptance":accept,"reason":error})
        check("off","OFF",accept=True,baseline=False)
        check("generate",accept=True)
        check("wrong_owner",change=lambda s:s.replace("CMakeFiles/rexcore.dir/","CMakeFiles/rexruntime.dir/"))
        check("missing_atomic",change=lambda s:s.replace(" -fprofile-update=atomic","",1))
        check("changed_fp",change=lambda s:s.replace("-ffp-model=strict","-ffp-model=fast",1))
        check("changed_define",change=lambda s:s.replace("-DKEEP=1","-DKEEP=2",1))
        check("changed_include",change=lambda s:s.replace("include space","other include",1))
        check("escaped_pch",change=lambda s:s.replace("-O3 -fPIC","-O3 -include-pch wrong.pch -fPIC",1))
        check("wrong_policy",change=lambda s:s.replace("sdk_ir_pgo.profile-list","other.profile-list",1))
        check("changed_link",change=lambda s:s.replace("-Wl,--no-relax","-Wl,--as-needed"))
        check("main_instrumented",change=lambda s:s.replace("-Wl,--no-relax","-Wl,--no-relax -fprofile-generate=/wrong"))
        check("duplicate_source",change=lambda s:s+s[s.index("build sdk/"):s.index("build sdk/",s.index("build sdk/")+1)])
        check("off_rejects_profile","OFF",change=lambda s:s.replace("-O3 -fPIC","-O3 -fprofile-use=/wrong -fPIC",1))
        (root/next(iter(pgo.SOURCES))).write_text("// changed after configuration\n")
        check("source_changed")
        (root/pgo.POLICY).write_text("[llvm]\ndefault:forbid\nfunction:_ZZN3rex8graphics16CommandProcessor13ExecutePacket*=allow\n")
        owner="vendor/rexglue-sdk/src/graphics/command_processor.cpp"
        local="_ZZN3rex8graphics16CommandProcessor13ExecutePacketEjENK3$_0clEj"
        full=str(root/owner)+";"+local
        def name_case(name,symbol,source,expected):
            pgo.need(pgo.policy_allows(root,symbol,source)==expected,"unexpected local-name admission: "+name)
            cases.append({"name":name,"expected_acceptance":expected})
        name_case("local_exact_owner",full,owner,True)
        name_case("local_missing_owner",full,None,False)
        name_case("local_wrong_selected_owner",full,"vendor/rexglue-sdk/src/graphics/vulkan/command_processor.cpp",False)
        name_case("local_basename_prefix",Path(owner).name+";"+local,owner,False)
        name_case("local_relative_prefix",owner+";"+local,owner,False)
        name_case("local_unreviewed_prefix",str(root/"other.cpp")+";"+local,owner,False)
        name_case("local_multiple_prefixes",str(root/owner)+";"+full,owner,False)
        name_case("local_forbidden_suffix",full.replace("ExecutePacket","UpdateWritePointer"),owner,False)
        name_case("compiler_helper_not_source_allowance",pgo.TERMINATE_HELPER,owner,False)
        sample="Counters:\n  "+full+":\n    Hash: 0x123\n    Counters: 2\n    Block counts: [31, 9]\n  __clang_call_terminate:\n    Block counts: [0]\nInstrumentation level: IR\n"
        pgo.need(pgo.profile_counts(sample)=={full:40,pgo.TERMINATE_HELPER:0},"space-containing profile counts changed")
        cases.append({"name":"space_containing_profile_identity","expected_acceptance":True})
    pgo.save(a.output,{"passed":True,"cases":cases,"compiler_invocations":0,"game_runs":0})
    print("PASS SDK PGO controls: %d graph/identity acceptance and refusal cases; no compilation"%len(cases))

if __name__=="__main__":main()
