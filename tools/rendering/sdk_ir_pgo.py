#!/usr/bin/env python3
"""Closed SDK IR-PGO identity, object coverage, bounded training, and merge tools.

No command installs tools, edits settings, configures/builds CMake, or starts a
game implicitly. Only the explicit train subcommand executes its supplied copied
user-data harness command. Qualification and performance remain separate.
"""
import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import sdk_ir_pgo_elf

SOURCES = {
    "vendor/rexglue-sdk/src/graphics/command_processor.cpp": "rexgraphics",
    "vendor/rexglue-sdk/src/graphics/vulkan/command_processor.cpp": "rexgraphics",
    "vendor/rexglue-sdk/src/system/xmemory.cpp": "rexruntime",
    "vendor/rexglue-sdk/src/system/function_dispatcher.cpp": "rexruntime",
    "vendor/rexglue-sdk/src/core/ring_buffer.cpp": "rexcore",
}
POLICY = "cmake/sdk_ir_pgo.profile-list"
TERMINATE_HELPER = "__clang_call_terminate"
REQUIRED = {
    "base_heap": "_ZN3rex6memory8BaseHeap16QueryRangeAccessEjj",
    "packet": "_ZN3rex8graphics16CommandProcessor13ExecutePacket*",
    "native_draw": "_ZN3rex8graphics6vulkan22VulkanCommandProcessor9IssueDraw*",
    "ring": "_ZN3rex6memory10RingBuffer*",
}

def need(value, reason):
    if not value:
        raise RuntimeError(reason)

def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def save(path, data, fresh=True):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x" if fresh else "w") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")

def read(path):
    return json.loads(Path(path).read_text())

def run(args):
    return subprocess.run([str(a) for a in args], check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout

def compiler_identity(compiler):
    binary = Path(shutil.which(compiler) or compiler).resolve(strict=True)
    version = run([binary, "--version"])
    match = re.search(r"clang version (\d+\.\d+\.\d+)", version)
    need(match, "not a supported Clang compiler identity")
    return {"path": str(binary), "sha256": digest(binary), "version": version,
            "release": match.group(1), "target": run([binary, "-print-target-triple"]).strip(),
            "resource_dir": run([binary, "-print-resource-dir"]).strip()}

def source_identity(root):
    paths = [root / p for p in (POLICY, "CMakeLists.txt", "cmake/native_pgo.cmake",
             "cmake/sdk_ir_pgo.cmake", "tools/rendering/sdk_ir_pgo.py",
             "tools/rendering/sdk_ir_pgo_desktop.py", "tools/rendering/sdk_ir_pgo_elf.py",
             "tools/rendering/sdk_ir_pgo_qualify_filter.py", "tools/rendering/sdk_ir_pgo_policy_test.cpp",
             "tools/rendering/sdk_ir_pgo_policy_driver.cpp",
             "tools/rendering/verify-native-boot.py", "generated/rexglue.cmake",
             "vendor/rexglue-sdk/CMakeLists.txt")]
    paths.extend((root / "vendor/rexglue-sdk/cmake").rglob("*.cmake"))
    for folder in ("vendor/rexglue-sdk/include", "vendor/rexglue-sdk/src"):
        paths.extend(p for p in (root / folder).rglob("*") if p.is_file()
                     and (p.suffix in (".h", ".hpp", ".inl", ".inc", ".cpp", ".c", ".cmake")
                          or p.name == "CMakeLists.txt"))
    need(len(paths) <= 10000, "SDK source identity bound")
    entries = {str(p.relative_to(root)): digest(p) for p in sorted(set(paths))}
    return {"files": entries, "sha256": hashlib.sha256(json.dumps(entries, sort_keys=True).encode()).hexdigest()}

def policy_allows(root, symbol, source=None):
    # Qualified compiler gave allow priority over overlapping forbid entries.
    # The committed policy therefore has positive exact method prefixes only.
    if ";" in symbol:
        # LLVM22 prepends the exact main TU to internal-linkage profile names.
        # Preserve that identity in reports/profiles; remove it for matching only
        # after checking the actual owning selected object (never basename-only).
        if source not in SOURCES or symbol.count(";") != 1:
            return False
        prefix, symbol = symbol.split(";", 1)
        if prefix != str(root / source) or not symbol.startswith("_ZZ"):
            return False
    result = False
    for line in (root / POLICY).read_text().splitlines():
        if line.startswith("function:"):
            pattern, category = line[len("function:"):].rsplit("=", 1)
            need(category == "allow", "mixed allow/forbid function policies are not qualified")
            if fnmatch.fnmatchcase(symbol, pattern):
                result = True
    return result

def terminate_helper_certificate(path):
    # This compiler-generated, hidden COMDAT bypasses Clang22's profile-list
    # even with an exact forbid. Pin its complete unlinked x86-64 large/PIC body
    # and relocation targets: one atomic counter increment, begin_catch, then
    # std::terminate. Any other helper/body is outside the qualified scope.
    dump = run(["objdump", "-dr", "--section=.ltext.__clang_call_terminate", path])
    code = "".join(m.group(1).replace(" ", "") for m in re.finditer(
        r"^\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )*[0-9a-f]{2})\s*(?:\t|$)", dump, re.MULTILINE))
    relocations = re.findall(r"^\s*([0-9a-f]+):\s+(R_X86_64_\S+)\s+(\S+)", dump, re.MULTILINE)
    expected = [("a", "R_X86_64_GOTPC64", "_GLOBAL_OFFSET_TABLE_+0x9"),
                ("17", "R_X86_64_GOTOFF64", "__profc___clang_call_terminate.742261418966908927"),
                ("26", "R_X86_64_GOT64", "__cxa_begin_catch"),
                ("33", "R_X86_64_GOT64", "_ZSt9terminatev")]
    code_hash = hashlib.sha256(bytes.fromhex(code)).hexdigest()
    need(code_hash == "c59e3818d2a06f6809f87a3404fa610a3ff8684b8d053d5d5b588975f9049a4f"
         and relocations == expected, "compiler termination helper body/relocations changed")
    return {"symbol": TERMINATE_HELPER, "code_sha256": code_hash,
            "relocations": relocations, "required_training_count": 0}

def profile_counts(counts):
    # Internal profile names contain an absolute path, including workspace
    # spaces. Only exactly two-space function headings delimit a record.
    need("Instrumentation level: IR" in counts, "merged profile is not IR instrumentation")
    functions = {}
    for match in re.finditer(r"^  (\S[^\n]*):\n(.*?)(?=^  \S[^\n]*:\n|^Instrumentation level:|\Z)",
                             counts, re.MULTILINE | re.DOTALL):
        block = re.search(r"Block counts: \[([^]]*)\]", match.group(2))
        if block:
            name = match.group(1)
            need(name not in functions, "duplicate merged function identity")
            functions[name] = sum(int(n.strip()) for n in block.group(1).split(",") if n.strip())
    return functions

def configure(a):
    root, build = Path(a.root).resolve(), Path(a.build).resolve()
    path = Path(a.path)
    need(path.is_absolute(), "profile path must be absolute")
    identity = {"schema": "sdk-ir-pgo-config-v1", "mode": a.mode, "root": str(root),
                "build": str(build), "compiler": compiler_identity(a.compiler),
                "source": source_identity(root), "sources": SOURCES,
                "policy_sha256": digest(root / POLICY), "profile_path": str(path)}
    need(identity["compiler"]["release"].split(".")[0] == "22", "this stage requires qualified Clang22")
    if a.mode == "GENERATE":
        need(not path.exists() or path.is_dir(), "raw path is not a directory")
        need(not path.exists() or not any(path.iterdir()), "use a fresh empty training raw directory")
        path.mkdir(parents=True, exist_ok=True)
    else:
        need(path.is_file(), "merged profile is absent")
        need(re.fullmatch(r"[0-9a-f]{64}", a.profile_sha256 or ""), "exact profile SHA256 required")
        need(digest(path) == a.profile_sha256, "merged profile identity mismatch")
        manifest = read(a.manifest)
        need(manifest["schema"] == "sdk-ir-pgo-merge-v1", "unsupported profile manifest")
        need(manifest["profile_sha256"] == a.profile_sha256, "manifest/profile mismatch")
        need(manifest["compiler"] == identity["compiler"], "compiler changed since training")
        need(manifest["source"] == identity["source"], "SDK source or policy changed since training")
        need(manifest["coverage_passed"] and manifest["filter_qualified"], "unqualified training profile")
        for name, expected in manifest["dependencies"].items():
            need(Path(name).is_file() and digest(name) == expected, "training dependency changed: " + name)
        identity.update(profile_sha256=a.profile_sha256, manifest_sha256=digest(a.manifest))
    save(a.output, identity, fresh=False)
    print("SDK IR-PGO configuration identity: " + str(Path(a.output).resolve()))

def ninja_objects(build, root):
    data = (build / "build.ninja").read_text().replace("$\n", "")
    objects = []
    for block in data.split("\nbuild "):
        lines = block.splitlines()
        if not lines:
            continue
        m = re.match(r"(.+\.(?:o|obj)): (\S+) (.+?)(?: \|\|| \||$)", lines[0])
        if not m or "COMPILER" not in m.group(2):
            continue
        decode = lambda s: s.replace("$ ", " ").replace("$:", ":").replace("$$", "$")
        obj, source = decode(m.group(1)), Path(decode(m.group(3)))
        if not source.is_absolute():
            source = (build / source).resolve()
        fields = {}
        for line in lines[1:]:
            if line.startswith("  ") and " = " in line:
                key, value = line.strip().split(" = ", 1)
                if key in ("FLAGS", "DEFINES", "INCLUDES"):
                    fields[key] = decode(value)
        try:
            relative = str(source.relative_to(root))
        except ValueError:
            relative = str(source)
        flags = shlex.split(fields.get("FLAGS", ""))
        objects.append({"object": obj, "source": relative, "fields": fields,
                        "profile_options": [f for f in flags if f.startswith(("-fprofile", "-fno-profile"))],
                        "base_flags": [f for f in flags if not f.startswith(("-fprofile", "-fno-profile", "-Werror=profile-instr"))]})
    need(objects, "no actual Ninja compiler objects found")
    return objects

def profile_symbols(path, nm):
    # LLVM22 lowers private __profc_ labels away. Decode actual data records and
    # their compressed name table instead of assuming nm retains those labels.
    return sdk_ir_pgo_elf.profile_symbols(path, nm)

def ninja_links(build):
    links = {}
    for block in (build / "build.ninja").read_text().replace("$\n", "").split("\nbuild "):
        lines = block.splitlines()
        if not lines or "_LINKER__" not in lines[0]:
            continue
        name = lines[0].split(": ", 1)[0].replace("$ ", " ")
        fields = {}
        for line in lines[1:]:
            if line.startswith("  ") and " = " in line:
                key, value = line.strip().split(" = ", 1)
                if key in ("FLAGS", "LINK_FLAGS", "LINK_LIBRARIES"):
                    fields[key] = shlex.split(value.replace("$ ", " "))
        links[name] = fields
    need("bin/gow3" in links and "bin/librexruntime.so" in links, "main/runtime linker graph missing")
    return links

def coverage(a):
    root, build = Path(a.root).resolve(), Path(a.build).resolve()
    objects = ninja_objects(build, root)
    links = ninja_links(build)
    selected = [o for o in objects if o["source"] in SOURCES]
    need(len(selected) == len(SOURCES), "selected source missing/duplicated in actual graph")
    mode = a.mode
    config = read(build / "sdk-ir-pgo/config.json") if mode != "OFF" else None
    if config:
        need(config["mode"] == mode and config["source"] == source_identity(root), "configured identity changed")
    for name, fields in links.items():
        options = [f for values in fields.values() for f in values if f.startswith("-fprofile")]
        desired = (["-fprofile-generate=" + config["profile_path"]]
                   if mode == "GENERATE" and name == "bin/librexruntime.so" else [])
        need(options == desired, "profile runtime linker scope escaped: " + name)
    base_links = {name: {key: [v for v in values if not v.startswith("-fprofile")]
                        for key, values in fields.items()} for name, fields in links.items()}
    for obj in objects:
        options = obj["profile_options"]
        expected = obj["source"] in SOURCES and mode != "OFF"
        if obj["source"] in SOURCES:
            need("CMakeFiles/" + SOURCES[obj["source"]] + ".dir/" in obj["object"],
                 "selected source assigned to wrong owning target")
        need(bool(options) == expected, "escaped or missing profiling options: " + obj["object"])
        if expected:
            want = "-fprofile-generate=" if mode == "GENERATE" else "-fprofile-use="
            desired = ["-fprofile-list=" + str(root / POLICY), want + config["profile_path"]]
            if mode == "GENERATE":
                desired.append("-fprofile-update=atomic")
            need(options == desired, "actual profiling options differ from configured scope")
        if obj["source"] in SOURCES:
            need(not any("pch" in f.lower() for f in obj["base_flags"]), "selected source unexpectedly uses PCH")
            need(not any(f.startswith(("-flto", "-ffast-math", "-Ofast", "-march=native")) for f in obj["base_flags"]),
                 "unexpected ABI/math/ISA experiment in selected SDK sources")
    if a.baseline:
        baseline_report = read(a.baseline)
        need(baseline_report["mode"] == "OFF" and baseline_report["passed"], "baseline is not qualified OFF")
        baseline = {o["object"]: o for o in baseline_report["objects"]}
        need(base_links == baseline_report["base_links"], "non-PGO linker settings changed")
        need(set(baseline) == {o["object"] for o in objects}, "object scope changed")
        for obj in objects:
            old = baseline[obj["object"]]
            need(obj["base_flags"] == old["base_flags"], "non-PGO flags changed: " + obj["object"])
            for field in ("DEFINES", "INCLUDES"):
                need(obj["fields"].get(field) == old["fields"].get(field), "definitions/includes changed")
    result = {"schema": "sdk-ir-pgo-coverage-v1", "mode": mode, "passed": True,
              "objects": objects, "selected_sources": sorted(SOURCES), "dependencies": {},
              "runtime_profile_symbols": [], "objects_observed": False,
              "links": links, "base_links": base_links,
              "baseline_compared": bool(a.baseline),
              "baseline_sha256": digest(a.baseline) if a.baseline else None}
    if mode == "USE":
        need(a.manifest, "USE graph must be compared against training manifest")
        manifest = read(a.manifest)
        graph = lambda rows: [{"object": o["object"], "source": o["source"], "base_flags": o["base_flags"],
                               "DEFINES": o["fields"].get("DEFINES"), "INCLUDES": o["fields"].get("INCLUDES")}
                              for o in rows]
        need(graph(objects) == manifest["training_graph"], "USE non-PGO graph differs from GENERATE")
        need(base_links == manifest["training_base_links"], "USE non-PGO linker settings differ")
        need(config["manifest_sha256"] == digest(a.manifest), "USE manifest changed after configure")
    if a.observe and mode in ("OFF", "USE"):
        for obj in selected:
            path = build / obj["object"]
            need(path.is_file() and not profile_symbols(path, a.nm), "OFF/USE object retains instrumentation")
            obj["sha256"] = digest(path)
        for path in (build / "bin/gow3", build / "bin/librexruntime.so"):
            need(path.is_file() and not profile_symbols(path, a.nm), "OFF/USE binary retains instrumentation")
        result.update(objects_observed=True, runtime_sha256=digest(build / "bin/librexruntime.so"),
                      main_sha256=digest(build / "bin/gow3"))
    elif a.observe:
        names = set()
        for obj in selected:
            path = build / obj["object"]
            need(path.is_file(), "selected object not built")
            symbols = profile_symbols(path, a.nm)
            need(symbols, "selected object has no observed profile counters: " + obj["source"])
            need(all(n == TERMINATE_HELPER or policy_allows(root, n, obj["source"]) for n in symbols),
                 "forbidden object counter escaped allowlist: " + obj["source"])
            if TERMINATE_HELPER in symbols:
                obj["compiler_helper"] = terminate_helper_certificate(path)
            obj.update(sha256=digest(path), profile_symbols=symbols)
            names.update(symbols)
            deps = run(["ninja", "-C", build, "-t", "deps", obj["object"]])
            paths = [Path(line.strip()) for line in deps.splitlines() if line.startswith("    ")]
            need(len(paths) > 1, "actual dependency list missing for " + obj["object"])
            for path in paths:
                if not path.is_absolute():
                    path = (build / path).resolve()
                need(path.is_file(), "dependency is missing: " + str(path))
                if str(path) not in result["dependencies"]:
                    result["dependencies"][str(path)] = digest(path)
            need(len(result["dependencies"]) <= 20000, "dependency census bound")
        runtime = build / "bin/librexruntime.so"
        observed = profile_symbols(runtime, a.nm)
        need(set(observed) == names, "runtime profile counters differ from five selected objects")
        need(not profile_symbols(build / "bin/gow3", a.nm), "profiling escaped into game/Native executable")
        for label, pattern in REQUIRED.items():
            need(any(fnmatch.fnmatchcase(n, pattern) for n in names), "required counter absent: " + label)
        result.update(objects_observed=True, runtime_profile_symbols=observed,
                      runtime_sha256=digest(runtime), main_sha256=digest(build / "bin/gow3"))
    save(a.output, result)
    print("PASS SDK PGO coverage: %d objects, %d selected sources, %d observed symbols" %
          (len(objects), len(selected), len(result["runtime_profile_symbols"])))

def train(a):
    config, coverage, proof = read(a.config), read(a.coverage), read(a.filter_proof)
    need(config["mode"] == "GENERATE", "training requires GENERATE")
    need(coverage["passed"] and coverage["objects_observed"] and coverage["baseline_compared"],
         "actual object census or unchanged OFF flag comparison missing")
    need(proof["passed"] and proof["compiler"] == config["compiler"] and
         proof["policy_sha256"] == config["policy_sha256"], "compiler filter qualification mismatch")
    root, build = Path(config["root"]), Path(config["build"])
    need(proof["fixture_sha256"] == digest(root / "tools/rendering/sdk_ir_pgo_policy_test.cpp") and
         proof.get("dso_only_flush") and proof.get("use_strict_warnings_passed") and
         proof.get("local_profile_names_qualified") and proof.get("compiler_terminate_helper_qualified"),
         "fixture source/DSO flush/strict USE qualification missing")
    need(build.resolve() == (root / "build").resolve(), "existing harness is bound to root/build binaries")
    need(source_identity(root) == config["source"], "sources changed after GENERATE configure")
    for name, expected in coverage["dependencies"].items():
        need(digest(name) == expected, "training dependency changed: " + name)
    need(digest(build / "bin/gow3") == coverage["main_sha256"] and
         digest(build / "bin/librexruntime.so") == coverage["runtime_sha256"], "training binary changed")
    command = read(a.command)
    need(isinstance(command, list) and all(isinstance(x, str) for x in command), "command must be a JSON argv array")
    need(len(command) >= 3, "missing harness command")
    python = Path(shutil.which(command[0]) or command[0]).resolve()
    harness = Path(command[1])
    if not harness.is_absolute():
        harness = root / harness
    admitted = {(root / "tools/rendering/verify-native-boot.py").resolve(),
                (root / "tools/rendering/sdk_ir_pgo_desktop.py").resolve()}
    need(python == Path(sys.executable).resolve() and harness.resolve() in admitted,
         "training must use this Python and exact qualified workspace runner")
    desktop = harness.name == "sdk_ir_pgo_desktop.py"
    for flag in ("--hardware", "--raster", "--copy-user-data", "--seconds", "--output"):
        need(command.count(flag) == 1, "required unique training flag: " + flag)
    seconds = int(command[command.index("--seconds") + 1])
    need(60 <= seconds <= 900, "training duration must be bounded to60..900 seconds")
    harness_output = Path(command[command.index("--output") + 1])
    if not harness_output.is_absolute():
        harness_output = root / harness_output
    need(harness_output.resolve().is_relative_to((root / "reports").resolve()) and not harness_output.exists(),
         "harness output must be fresh and inside workspace reports")
    output = Path(a.output)
    need(not output.exists(), "training report already exists")
    output.mkdir(parents=True)
    raw = output.resolve() / "raw"
    raw.mkdir()
    environment = dict(os.environ)
    environment["LLVM_PROFILE_FILE"] = str(raw / "sdk_%m_%p.profraw")
    result = {"schema": "sdk-ir-pgo-training-v1", "config": config, "coverage": coverage,
              "filter_proof": proof, "command": command, "seconds": seconds,
              "instrumented_training_only": True, "desktop": desktop, "performance_claim": False, "passed": False}
    save(output / "startup.json", result)
    print("SDK IR-PGO instrumented training: %ds; compiler %s; SDK %s" %
          (seconds, config["compiler"]["release"], coverage["runtime_sha256"]), flush=True)
    with (output / "harness.log").open("x") as log:
        process = subprocess.Popen(command, cwd=root, env=environment, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            result["returncode"] = process.wait(timeout=seconds + 120)
        except subprocess.TimeoutExpired:
            result["timeout"] = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            result["returncode"] = process.returncode
    files = sorted(raw.glob("*.profraw"))
    result["raw"] = {str(p.resolve()): {"bytes": p.stat().st_size, "sha256": digest(p)} for p in files}
    harness_result = read(harness_output / "result.json") if (harness_output / "result.json").exists() else {}
    actual_binaries = read(harness_output / "build-identity.json") if (harness_output / "build-identity.json").exists() else {}
    identity_matches = (actual_binaries.get("build/bin/gow3", {}).get("sha256") == coverage["main_sha256"] and
                        actual_binaries.get("build/bin/librexruntime.so", {}).get("sha256") == coverage["runtime_sha256"])
    result.update(clean_ui_close=harness_result.get("clean_ui_close", False),
                  harness_identity_matches=identity_matches, harness_output=str(harness_output))
    desktop_health = (not desktop or all(harness_result.get(key) for key in (
        "passed", "execution_complete", "strict_native_unmixed", "queries_retired",
        "gameplay_observed", "normal_config_unchanged", "normal_saves_unchanged")))
    unchanged = (digest(build / "bin/gow3") == coverage["main_sha256"] and
                 digest(build / "bin/librexruntime.so") == coverage["runtime_sha256"])
    result["passed"] = (result["returncode"] == 0 and not result.get("timeout") and bool(files) and
                        all(p.stat().st_size for p in files) and result["clean_ui_close"] and identity_matches
                        and desktop_health and unchanged)
    save(output / "result.json", result)
    need(result["passed"], "training failed or no clean raw profile; preserve report")
    print("PASS bounded instrumented training; raw profile files=" + str(len(files)))

def merge(a):
    records = [read(p) for p in a.training]
    need(1 <= len(records) <= 8 and all(r["passed"] for r in records), "one to eight qualified training reports required")
    first = records[0]
    tool = Path(a.profdata).resolve(strict=True)
    version = run([tool, "--version"])
    need("LLVM version " + first["config"]["compiler"]["release"] in version, "llvm-profdata/compiler release mismatch")
    output = Path(a.output).resolve()
    need(not output.exists() and not Path(a.manifest).exists(), "merge outputs must be fresh")
    output.parent.mkdir(parents=True, exist_ok=True)
    args = [str(tool), "merge", "--failure-mode=any", "-o", str(output)]
    seen = set()
    for record in records:
        for field in ("compiler", "source", "policy_sha256"):
            need(record["config"][field] == first["config"][field], "training identities differ")
        need(record["coverage"]["dependencies"] == first["coverage"]["dependencies"], "training dependencies differ")
        for field in ("runtime_sha256", "main_sha256", "base_links", "objects", "baseline_sha256", "runtime_profile_symbols"):
            need(record["coverage"][field] == first["coverage"][field], "training built scope/identity differs: " + field)
        for name, identity in record["raw"].items():
            need(name not in seen and digest(name) == identity["sha256"], "duplicate/changed raw profile")
            seen.add(name)
            args.append(name)
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    Path(str(output) + ".merge.log").write_text(result.stdout)
    need(result.returncode == 0 and output.is_file(), "profile merge failed")
    counts = run([tool, "show", "--all-functions", "--counts", output])
    Path(str(output) + ".counts.txt").write_text(counts)
    functions = profile_counts(counts)
    root = Path(first["config"]["root"])
    need(set(functions) == set(first["coverage"]["runtime_profile_symbols"]), "merged functions differ from observed runtime counters")
    for name, count in functions.items():
        owners = [obj for obj in first["coverage"]["objects"] if name in obj.get("profile_symbols", [])]
        need(owners, "merged function has no qualified object owner")
        if name == TERMINATE_HELPER:
            need(count == 0 and all(obj.get("compiler_helper", {}).get("symbol") == name for obj in owners),
                 "compiler termination helper executed or lacks qualified object certificate")
        else:
            need(all(policy_allows(root, name, obj["source"]) for obj in owners), "merged profile escaped allowlist")
    for label, pattern in REQUIRED.items():
        need(any(value > 0 and fnmatch.fnmatchcase(name, pattern) for name, value in functions.items()),
             "required SDK function has no observed executions: " + label)
    manifest = {"schema": "sdk-ir-pgo-merge-v1", "profile_sha256": digest(output),
                "compiler": first["config"]["compiler"], "source": first["config"]["source"],
                "dependencies": first["coverage"]["dependencies"],
                "coverage_passed": True, "filter_qualified": True,
                "observed_function_counts": functions,
                "baseline_sha256": first["coverage"]["baseline_sha256"],
                "training_base_links": first["coverage"]["base_links"],
                "training_graph": [{"object": o["object"], "source": o["source"], "base_flags": o["base_flags"],
                                    "DEFINES": o["fields"].get("DEFINES"), "INCLUDES": o["fields"].get("INCLUDES")}
                                   for o in first["coverage"]["objects"]],
                "training_reports": {str(Path(p).resolve()): digest(p) for p in a.training},
                "tool": {"path": str(tool), "sha256": digest(tool), "version": version},
                "raw_files": sorted(seen), "merge_weights": "one per raw file; no silent reweighting"}
    save(a.manifest, manifest)
    print("PASS merged SDK IR profile SHA256=" + manifest["profile_sha256"])

def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="operation", required=True)
    c = sub.add_parser("configure")
    for key in ("root", "build", "compiler", "mode", "path", "output"):
        c.add_argument("--" + key, required=True)
    c.add_argument("--manifest", default="")
    c.add_argument("--profile-sha256", default="")
    c.set_defaults(call=configure)
    c = sub.add_parser("coverage")
    for key in ("root", "build", "mode", "output"):
        c.add_argument("--" + key, required=True)
    c.add_argument("--baseline")
    c.add_argument("--manifest")
    c.add_argument("--observe", action="store_true")
    c.add_argument("--nm", default="nm")
    c.set_defaults(call=coverage)
    c = sub.add_parser("train")
    for key in ("config", "coverage", "filter-proof", "command", "output"):
        c.add_argument("--" + key, required=True)
    c.set_defaults(call=train)
    c = sub.add_parser("merge")
    for key in ("profdata", "output", "manifest"):
        c.add_argument("--" + key, required=True)
    c.add_argument("--training", action="append", required=True)
    c.set_defaults(call=merge)
    a = p.parse_args()
    need(getattr(a, "mode", "OFF") in ("OFF", "GENERATE", "USE"), "invalid SDK PGO mode")
    a.call(a)

if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as e:
        print("FAIL SDK IR-PGO: " + str(e), file=sys.stderr)
        sys.exit(1)
