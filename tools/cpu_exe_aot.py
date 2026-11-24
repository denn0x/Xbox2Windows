"""Lower the CRT-free x64 comparison EXE from its PE entry point (bounded fixture ABI)."""
import argparse
from graphlib import CycleError, TopologicalSorter
from pathlib import Path
import re

import pefile
from iced_x86 import MemorySizeExt, Mnemonic as M, OpKind as K, Register as R, RegisterExt

from cpu_call_aot import lift_body, resolve_import
from cpu_leaf_aot import bounded_code, export_rva
from ffx_version_query import INTEGER_HEADER, lower, region


CPP_KEYWORDS = frozenset("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t
class compl concept const const_cast consteval constexpr constinit continue co_await co_return co_yield
decltype default delete do double dynamic_cast else enum explicit export extern false float for friend
goto if inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private
protected public register reinterpret_cast requires return short signed sizeof static static_assert
static_cast struct switch template this thread_local throw true try typedef typeid typename union
unsigned using virtual void volatile wchar_t while xor xor_eq import module final override
""".split())
GENERATED_NAMES = frozenset("""
read_selector emit_trace lifted main word load store memcpy memset std
""".split())
HEADER_NAMES = re.compile(
    r"(?:u?int(?:8|16|32|64|ptr|max|_least\d+|_fast\d+)_t|"
    r"(?:U?INT|PTRDIFF|SIZE|WCHAR|WINT|SIG_ATOMIC)[A-Z0-9_]*|"
    r"size_t|ptrdiff_t|NULL|offsetof|"
    r"memchr|memcmp|memmove|strcat|strchr|strcmp|strcoll|strcpy|strcspn|strerror|strlen|"
    r"strncat|strncmp|strncpy|strpbrk|strrchr|strspn|strstr|strtok|strxfrm)"
)


def validate_provider_symbol(symbol, providers):
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", symbol) or "__" in symbol:
        raise ValueError(f"unsupported C++ import symbol {symbol!r}")
    if (symbol in providers or symbol in CPP_KEYWORDS or symbol in GENERATED_NAMES
            or symbol.startswith("lifted_") or HEADER_NAMES.fullmatch(symbol)):
        raise ValueError(f"duplicate or reserved import symbol {symbol!r}")


def require_local_pointer(previous, register, call, jump_targets):
    # No path may jump past the LEA that supplies the three-word buffer.
    if (previous.mnemonic != M.LEA or previous.op0_register != register
            or call.ip in jump_targets or previous.memory_displacement % 8):
        raise ValueError(f"expected aligned local trace pointer before call at {call.ip:#x}")


def lift_internal_calls(pe, names, entry_interval, bind_provider):
    pending = list(names)
    intervals = [entry_interval]
    functions = {}
    graph = TopologicalSorter()

    def bind_direct(address):
        if address not in names:
            names[address] = f"lifted_{address:x}"
            pending.append(address)
        return names[address]

    for address in pending:
        native = bounded_code(pe, address)
        end = address + len(native)
        if any(address < stop and begin < end for begin, stop in intervals):
            raise ValueError(f"overlapping internal call target at {address:#x}")
        intervals.append((address, end))
        source, edge = lift_body(address, native, caller=True, name=names[address],
                                 bind_import=bind_provider, allow_distinct_imports=True, bind_direct=bind_direct)
        functions[address] = source
        if edge[0] == "direct":
            graph.add(address, *edge[1])
        else:
            graph.add(address)

    try:
        return "\n".join(functions[address] for address in graph.static_order())
    except CycleError as exc:
        raise ValueError("recursive internal calls are outside the entry profile") from exc


def translate(image, io_provider, mix_provider, extra_providers=(), *, entry_export=None, frame_selector=False):
    providers = {"mix_args": mix_provider}
    for provider, symbol in extra_providers:
        validate_provider_symbol(symbol, providers)
        providers[symbol] = provider
    bound_symbols = {}
    with pefile.PE(str(image), fast_load=True) as pe:
        if (pe.FILE_HEADER.Machine != 0x8664 or pe.OPTIONAL_HEADER.Magic != 0x20b
                or not pe.FILE_HEADER.Characteristics & 2
                or pe.FILE_HEADER.Characteristics & 0x2000):
            raise ValueError("expected an AMD64 PE32+ executable")
        directories = pe.OPTIONAL_HEADER.DATA_DIRECTORY
        if len(directories) > 9 and directories[9].VirtualAddress:
            raise ValueError("TLS startup is outside this entry profile")
        entry = (export_rva(pe, entry_export) if entry_export
                 else pe.OPTIONAL_HEADER.AddressOfEntryPoint)
        body = region(pe, entry, entry + len(bounded_code(pe, entry)))
        if len(body) < 3:
            raise ValueError("incomplete entry stack frame")
        # The virtual frame in lower() supplies the native caller's shadow space
        # and locals. Keep the epilogue as a label for the shared return path.
        prologue, epilogue, ret = body[0], body[-2], body[-1]
        for ins, op in ((prologue, M.SUB), (epilogue, M.ADD)):
            if (ins.mnemonic != op or ins.op0_register != R.RSP
                    or ins.op1_kind != K.IMMEDIATE8TO64 or ins.immediate(1) != 72):
                raise ValueError("unsupported entry stack frame")
        if ret.mnemonic != M.RET or ret.op_count:
            raise ValueError("expected return from process entry")
        pe.parse_data_directories(directories=[1])
        imports = {imp.address - pe.OPTIONAL_HEADER.ImageBase: (module.dll, imp)
                   for module in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) for imp in module.imports}

        def bind_provider(slot):
            item = imports.get(slot)
            if item is None:
                raise ValueError(f"no ordinary import at IAT slot RVA {slot:#x}")
            dll, imp = item
            if imp.import_by_ordinal:
                raise ValueError(f"unbound import {dll.decode('ascii')}!#{imp.ordinal}")
            symbol = imp.name.decode("ascii")
            provider = providers.get(symbol)
            if provider is None:
                raise ValueError(f"unbound import {dll.decode('ascii')}!{symbol}")
            bound_symbols[symbol] = resolve_import(pe, slot, provider, symbol)
            return bound_symbols[symbol]

        io_calls = {b"read_selector": "rax = read_selector();",
                    b"emit_trace": "rax = emit_trace(reinterpret_cast<const word*>(rcx));"}
        bindings = {}
        names = {}
        selector_rva = export_rva(pe, "call_bare") if frame_selector else None
        saw_selector = False
        jump_targets = {ins.near_branch_target for ins in body if ins.op0_kind == K.NEAR_BRANCH64
                        and ins.mnemonic != M.CALL}
        for index, ins in enumerate(body[1:-2], start=1):
            if ins.mnemonic == M.CALL:
                if ins.op0_kind == K.NEAR_BRANCH64:
                    require_local_pointer(body[index - 1], R.R8, ins, jump_targets)
                    target = ins.near_branch_target
                    if target == selector_rva:
                        bindings[ins.ip] = "rax = lifted_call_bare(rcx, rdx, r8);"
                        saw_selector = True
                        continue
                    name = names.setdefault(target, f"lifted_{target:x}" if names else "lifted")
                    bindings[ins.ip] = f"rax = {name}(rcx, rdx, reinterpret_cast<word*>(r8));"
                elif ins.is_ip_rel_memory_operand:
                    slot = ins.ip_rel_memory_address
                    item = imports.get(slot)
                    name = item[1].name if item is not None else None
                    if name not in io_calls:
                        raise ValueError("unsupported entry import")
                    if name == b"emit_trace":
                        require_local_pointer(body[index - 1], R.RCX, ins, jump_targets)
                    resolve_import(pe, slot, io_provider, name.decode("ascii"))
                    bindings[ins.ip] = io_calls[name]
                else:
                    raise ValueError("unsupported indirect entry call")
            elif ins.mnemonic not in (M.MOV, M.LEA, M.CMP, M.TEST, M.JE, M.JNE, M.JMP):
                raise ValueError(f"unsupported entry instruction at {ins.ip:#x}")
            elif (ins.mnemonic in (M.MOV, M.LEA) and ins.op0_kind == K.REGISTER
                    and RegisterExt.full_register(ins.op0_register) == R.RSP):
                raise ValueError("unsupported change to entry stack pointer")
            elif any(ins.op_kind(n) == K.MEMORY for n in range(ins.op_count)):
                offset = ins.memory_displacement
                width = 24 if ins.mnemonic == M.LEA else MemorySizeExt.size(ins.memory_size)
                if (ins.memory_base != R.RSP or ins.memory_index != R.NONE
                        or offset < 32 or offset + width > 72):
                    raise ValueError("entry operand outside local frame")
        if not names:
            raise ValueError("expected an internal call in the entry profile")
        if frame_selector and not saw_selector:
            raise ValueError("expected a direct call to the frame selector")
        callees = lift_internal_calls(pe, names, (entry, ret.next_ip), bind_provider)
        if "mix_args" not in bound_symbols:
            raise ValueError("internal callee must reach the named provider")
        code = lower(body, [], {}, bindings, {}, host_ops={prologue.ip: ";", epilogue.ip: ";"})
    declarations = "".join(f'extern "C" __declspec(dllimport) word {symbol}(word, word, word*);\n'
                           for symbol in bound_symbols)
    selector_decl = 'extern "C" word lifted_call_bare(word, word, word);\n' if frame_selector else ""
    return INTEGER_HEADER + selector_decl + """
extern "C" __declspec(dllimport) uint32_t read_selector();
extern "C" __declspec(dllimport) uint32_t emit_trace(const word*);
""" + declarations + callees + '\nint main() {\n' + code + '\n}\n'


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("image", "io_provider", "mix_provider", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--entry-export", help="translate this return-based export instead of the PE startup")
    parser.add_argument("--frame-selector", action="store_true",
                        help="bind the fixture's call_bare export to its separately translated definition")
    parser.add_argument("--provider", action="append", nargs=2, default=[], metavar=("DLL", "SYMBOL"),
                        help="authorize an additional named import with the same trace ABI")
    args = parser.parse_args()
    if args.output.suffix.lower() != ".cpp":
        parser.error("choose a .cpp output")
    providers = [(Path(path), symbol) for path, symbol in args.provider]
    try:
        source = translate(args.image, args.io_provider, args.mix_provider, providers, entry_export=args.entry_export,
                           frame_selector=args.frame_selector)
        args.output.write_text(source, encoding="ascii")
    except (OSError, ValueError, pefile.PEFormatError) as error:
        parser.exit(1, f"{error}\n")
